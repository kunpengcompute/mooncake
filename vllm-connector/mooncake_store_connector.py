# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright contributors to the vLLM project

import os
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Optional

import torch
from safetensors.torch import load as safetensors_load
from safetensors.torch import save as safetensors_save

from mooncake.mooncake_config import MooncakeConfig
from mooncake.store import MooncakeDistributedStore
try:
    from mooncake.store import ReplicateConfig
except ImportError:
    ReplicateConfig = None

from vllm.config import VllmConfig
from vllm.distributed.kv_transfer.kv_connector.v1.base import (
    KVConnectorBase_V1,
    KVConnectorMetadata,
    KVConnectorRole,
)
from vllm.logger import init_logger
from vllm.model_executor.layers.attention.mla_attention import MLACommonMetadata
from vllm.utils.hashing import safe_hash
from vllm.v1.attention.backend import AttentionMetadata
from vllm.v1.core.sched.output import SchedulerOutput

if TYPE_CHECKING:
    from vllm.forward_context import ForwardContext
    from vllm.v1.core.kv_cache_manager import KVCacheBlocks
    from vllm.v1.kv_cache_interface import KVCacheConfig
    from vllm.v1.request import Request

logger = init_logger(__name__)


def _truthy(value: Any) -> bool:
    if value is None:
        return False
    return str(value).strip().lower() in ("1", "true", "yes", "on")


def _env_int(name: str, default: int) -> int:
    value = os.getenv(name)
    if value is None or value == "":
        return default
    try:
        return int(value)
    except ValueError:
        logger.warning("Invalid integer env %s=%s, using %d", name, value, default)
        return default


def _extra_config(cfg: Any, key: str, default: Any) -> Any:
    return cfg.get_from_extra_config(key, default)


def align_to_block_size(num_tokens: int, block_size: int) -> int:
    if num_tokens <= 0:
        return 0
    return (num_tokens - 1) // block_size * block_size


def group_contiguous(indices: list[int]) -> list[list[int]]:
    if not indices:
        return []
    groups = [[indices[0]]]
    for index in indices[1:]:
        if index == groups[-1][-1] + 1:
            groups[-1].append(index)
        else:
            groups.append([index])
    return groups


@dataclass
class ReqMeta:
    token_ids: torch.Tensor
    slot_mapping: torch.Tensor
    block_ids: list[int]
    is_store: bool
    mm_hashes: list[str]

    @staticmethod
    def make_meta(
        token_ids: list[int],
        block_ids: list[int],
        block_size: int,
        is_store: bool,
        mm_hashes: list[str],
    ) -> "ReqMeta":
        valid_num_tokens = align_to_block_size(len(token_ids), block_size)
        token_ids_tensor = torch.tensor(token_ids)[:valid_num_tokens]
        block_ids = list(block_ids)
        block_ids_tensor = torch.tensor(block_ids)
        num_blocks = block_ids_tensor.shape[0]
        block_offsets = torch.arange(0, block_size)
        slot_mapping = (
            block_offsets.reshape((1, block_size))
            + block_ids_tensor.reshape((num_blocks, 1)) * block_size
        )
        slot_mapping = slot_mapping.flatten()[:valid_num_tokens]
        return ReqMeta(
            token_ids=token_ids_tensor,
            slot_mapping=slot_mapping,
            block_ids=block_ids,
            is_store=is_store,
            mm_hashes=mm_hashes,
        )


@dataclass
class MooncakeStoreConnectorMetadata(KVConnectorMetadata):
    requests: list[ReqMeta] = field(default_factory=list)

    def add_request(
        self,
        token_ids: list[int],
        block_ids: list[int],
        block_size: int,
        is_store: bool,
        mm_hashes: list[str],
    ) -> None:
        self.requests.append(
            ReqMeta.make_meta(token_ids, block_ids, block_size, is_store, mm_hashes)
        )


@dataclass
class PendingSave:
    request: ReqMeta
    ptrs: list[int] = field(default_factory=list)
    sizes: list[int] = field(default_factory=list)
    cpu_layers: dict[str, torch.Tensor] = field(default_factory=dict)
    seen_layers: set[str] = field(default_factory=set)


class MooncakeStoreConnector(KVConnectorBase_V1):
    """vLLM 0.15 Mooncake Store connector.

    The connector has two data paths:
    - GPU direct path: register vLLM KV-cache GPU storage and use
      batch_put_from_multi_buffers / batch_get_into_multi_buffers. This is the
      path that can trigger Mooncake NoF + SPDK GPU DMA-BUF. All layers for one
      request are submitted as one logical value.
    - CPU fallback path: keep the original debug connector behavior, copying KV
      to CPU safetensors bytes and using ordinary put/get. This keeps vLLM
      usable when Mooncake is built without NoF and SPDK GPU DMA-BUF, while
      also storing all layers for one request in one value.
    """

    def __init__(
        self,
        vllm_config: "VllmConfig",
        role: KVConnectorRole,
        kv_cache_config: Optional["KVCacheConfig"] = None,
    ):
        super().__init__(
            vllm_config=vllm_config,
            role=role,
            kv_cache_config=kv_cache_config,
        )
        self._block_size = vllm_config.cache_config.block_size
        self._requests_need_load: dict[str, Request] = {}
        self._registered_gpu_buffers: dict[int, int] = {}
        self._known_gpu_storages: dict[int, int] = {}
        self._pending_saves: dict[str, PendingSave] = {}

        cfg = self._kv_transfer_config
        self._mooncake_config_path = os.getenv("MOONCAKE_CONFIG_PATH", "")
        self._mooncake_config = MooncakeConfig.load_from_env()
        self._cache_prefix = _extra_config(cfg, "cache_prefix", "vllm015")

        self._replica_num = int(
            _extra_config(
                cfg,
                "replica_num",
                _env_int("MC_STORE_REPLICA_NUM", 1),
            )
        )
        self._nof_replica_num = int(
            _extra_config(
                cfg,
                "nof_replica_num",
                _env_int("MC_STORE_NOF_REPLICA_NUM", 0),
            )
        )
        self._spdk_gpu_dmabuf = _truthy(
            _extra_config(
                cfg,
                "spdk_gpu_dmabuf",
                os.getenv("MC_SPDK_GPU_DMABUF"),
            )
        )
        direct_mode = str(
            _extra_config(cfg, "gpu_direct", os.getenv(
                "VLLM_MOONCAKE_GPU_DIRECT", "auto"))
        ).lower()
        self._gpu_direct_enabled = self._resolve_gpu_direct(direct_mode)
        self._replicate_config = self._make_replicate_config()

        self._store = MooncakeDistributedStore()
        logger.info(
            "Mooncake config source: MOONCAKE_CONFIG_PATH=%s",
            self._mooncake_config_path,
        )
        logger.info(
            "Mooncake Store setup begin: local_hostname=%s metadata=%s master=%s "
            "global_segment_size=%d local_buffer_size=%d protocol=%s "
            "device=%s",
            self._mooncake_config.local_hostname,
            self._mooncake_config.metadata_server,
            self._mooncake_config.master_server_address,
            self._mooncake_config.global_segment_size,
            self._mooncake_config.local_buffer_size,
            self._mooncake_config.protocol,
            self._mooncake_config.device_name,
        )
        setup_rc = self._store.setup(
            self._mooncake_config.local_hostname,
            self._mooncake_config.metadata_server,
            self._mooncake_config.global_segment_size,
            self._mooncake_config.local_buffer_size,
            self._mooncake_config.protocol,
            self._mooncake_config.device_name,
            self._mooncake_config.master_server_address,
        )
        if setup_rc != 0:
            raise RuntimeError(f"Mooncake Store setup failed: rc={setup_rc}")

        logger.info(self._kv_transfer_config)
        logger.info(
            "Mooncake Store connector initialized: node=%s metadata=%s "
            "master=%s protocol=%s device=%s prefix=%s replica_num=%d "
            "nof_replica_num=%d spdk_gpu_dmabuf=%s gpu_direct=%s",
            self._mooncake_config.local_hostname,
            self._mooncake_config.metadata_server,
            self._mooncake_config.master_server_address,
            self._mooncake_config.protocol,
            self._mooncake_config.device_name,
            self._cache_prefix,
            self._replica_num,
            self._nof_replica_num,
            self._spdk_gpu_dmabuf,
            self._gpu_direct_enabled,
        )

    def _resolve_gpu_direct(self, direct_mode: str) -> bool:
        if direct_mode in ("1", "true", "yes", "on"):
            return True
        if direct_mode in ("0", "false", "no", "off"):
            return False
        if direct_mode != "auto":
            logger.warning("Unknown gpu_direct=%s, using auto", direct_mode)
        return self._nof_replica_num > 0 and self._spdk_gpu_dmabuf

    def _make_replicate_config(self):
        if ReplicateConfig is None:
            if self._nof_replica_num > 0:
                raise RuntimeError("Mooncake ReplicateConfig is unavailable")
            return None
        config = ReplicateConfig()
        config.replica_num = self._replica_num
        config.nof_replica_num = self._nof_replica_num
        return config

    def register_kv_caches(self, kv_caches: dict[str, torch.Tensor]) -> None:
        if not self._gpu_direct_enabled:
            logger.info("Mooncake Store GPU direct disabled; skip KV registration")
            return

        required = (
            "batch_put_from_multi_buffers",
            "batch_get_into_multi_buffers",
            "register_buffer",
        )
        missing = [name for name in required if not hasattr(self._store, name)]
        if missing:
            raise RuntimeError(
                "Mooncake Store GPU direct requires APIs: " + ", ".join(missing)
            )

        for layer_name, cache_or_caches in kv_caches.items():
            cache_list = (
                list(cache_or_caches)
                if isinstance(cache_or_caches, (list, tuple))
                else [cache_or_caches]
            )
            for cache in cache_list:
                if not isinstance(cache, torch.Tensor):
                    continue
                self._record_tensor_storage(cache, layer_name)

    def _record_tensor_storage(self, tensor: torch.Tensor, label: str) -> None:
        if not tensor.is_cuda:
            raise RuntimeError(
                "GPU direct path requires CUDA-compatible GPU tensor for " + label
            )
        storage = tensor.untyped_storage()
        base_ptr = int(storage.data_ptr())
        size = int(storage.nbytes())
        if base_ptr in self._known_gpu_storages:
            return
        rc = self._store.register_buffer(base_ptr, size)
        if rc != 0:
            raise RuntimeError(
                "Mooncake GPU RDMA/SPDK buffer registration failed for "
                f"{label}: ptr={base_ptr} size={size} rc={rc}"
            )
        self._known_gpu_storages[base_ptr] = size
        self._registered_gpu_buffers[base_ptr] = size
        logger.info(
            "Mooncake Store GPU KV storage registered for RDMA DMA-BUF and "
            "SPDK DMA-BUF: label=%s ptr=%d size=%d",
            label,
            base_ptr,
            size,
        )

    def start_load_kv(self, forward_context: "ForwardContext",
                      **kwargs: Any) -> None:
        metadata = self._get_connector_metadata()
        assert isinstance(metadata, MooncakeStoreConnectorMetadata)

        if metadata is None:
            logger.warning("start_load_kv called with empty connector metadata")
            return

        attn_metadata = forward_context.attn_metadata
        if attn_metadata is None:
            logger.warning("start_load_kv called with empty attention metadata")
            return

        for request in metadata.requests:
            if request.is_store:
                continue
            logger.info("Mooncake Store load request: tokens=%d direct=%s",
                        len(request.slot_mapping), self._gpu_direct_enabled)
            key = self._request_key(request)
            direct_ptrs: list[int] = []
            direct_sizes: list[int] = []
            cpu_layers: list[tuple[str, torch.Tensor]] = []
            for layer_name, layer in forward_context.no_compile_layers.items():
                kv_cache_attr = getattr(layer, "kv_cache", None)
                if kv_cache_attr is None:
                    continue
                kv_cache_layer = kv_cache_attr[forward_context.virtual_engine]
                if self._gpu_direct_enabled:
                    self._maybe_register_for_direct_read(kv_cache_layer, key)
                    ptrs, sizes = self._block_buffer_vectors(
                        kv_cache_layer, request, attn_metadata
                    )
                    direct_ptrs.extend(ptrs)
                    direct_sizes.extend(sizes)
                else:
                    cpu_layers.append((layer_name, kv_cache_layer))

            if self._gpu_direct_enabled:
                if not direct_ptrs:
                    continue
                results = self._store.batch_get_into_multi_buffers(
                    [key], [direct_ptrs], [direct_sizes], False
                )
                if len(results) != 1 or results[0] != 0:
                    raise RuntimeError(
                        f"Mooncake batch_get_into_multi_buffers failed key={key} "
                        f"results={results}"
                    )
                logger.info(
                    "Mooncake Store GET direct request key=%s bytes=%d "
                    "buffers=%d",
                    key,
                    sum(direct_sizes),
                    len(direct_ptrs),
                )
                continue

            if not cpu_layers:
                continue
            payload = self._store.get(key)
            if payload is None:
                raise RuntimeError(f"Mooncake Store GET returned None: {key}")
            tensors = safetensors_load(payload)
            for layer_name, kv_cache_layer in cpu_layers:
                tensor_key = self._cpu_layer_key(layer_name)
                if tensor_key not in tensors:
                    raise RuntimeError(
                        f"Mooncake Store GET payload missing layer={layer_name} "
                        f"key={key}"
                    )
                self._inject_kv_into_layer(
                    kv_cache_layer,
                    tensors[tensor_key].to(device=kv_cache_layer.device),
                    request.slot_mapping,
                    attn_metadata,
                )
            logger.info(
                "Mooncake Store GET cpu request key=%s bytes=%d layers=%d",
                key,
                len(payload),
                len(cpu_layers),
            )

    def wait_for_layer_load(self, layer_name: str) -> None:
        return

    def save_kv_layer(
        self,
        layer_name: str,
        kv_layer: torch.Tensor,
        attn_metadata: AttentionMetadata,
        **kwargs: Any,
    ) -> None:
        connector_metadata = self._get_connector_metadata()
        assert isinstance(connector_metadata, MooncakeStoreConnectorMetadata)

        for request in connector_metadata.requests:
            if not request.is_store:
                continue
            key = self._request_key(request)
            pending = self._pending_saves.get(key)
            if pending is None:
                pending = PendingSave(request=request)
                self._pending_saves[key] = pending
            if layer_name in pending.seen_layers:
                continue
            pending.seen_layers.add(layer_name)
            if self._gpu_direct_enabled:
                ptrs, sizes = self._block_buffer_vectors(
                    kv_layer, request, attn_metadata
                )
                pending.ptrs.extend(ptrs)
                pending.sizes.extend(sizes)
            else:
                kv_cache = self._extract_kv_from_layer(
                    kv_layer, request.slot_mapping, attn_metadata
                )
                pending.cpu_layers[layer_name] = (
                    kv_cache.detach().cpu().contiguous()
                )

    def wait_for_save(self) -> None:
        pending_saves = list(self._pending_saves.items())
        self._pending_saves.clear()
        for key, pending in pending_saves:
            if self._gpu_direct_enabled:
                if not pending.ptrs:
                    continue
                results = self._store.batch_put_from_multi_buffers(
                    [key], [pending.ptrs], [pending.sizes], self._replicate_config
                )
                if len(results) != 1 or results[0] != 0:
                    raise RuntimeError(
                        f"Mooncake batch_put_from_multi_buffers failed key={key} "
                        f"results={results}"
                    )
                total_bytes = sum(pending.sizes)
                logger.info(
                    "Mooncake Store PUT direct request key=%s bytes=%d "
                    "buffers=%d layers=%d",
                    key,
                    total_bytes,
                    len(pending.ptrs),
                    len(pending.seen_layers),
                )
            else:
                if not pending.cpu_layers:
                    continue
                payload = safetensors_save(
                    {
                        self._cpu_layer_key(layer_name): tensor
                        for layer_name, tensor in pending.cpu_layers.items()
                    }
                )
                status = self._put(key, payload)
                if status != 0:
                    raise RuntimeError(
                        f"Mooncake Store PUT failed key={key} rc={status}"
                    )
                logger.info(
                    "Mooncake Store PUT cpu request key=%s bytes=%d layers=%d",
                    key,
                    len(payload),
                    len(pending.cpu_layers),
                )
            self._put_marker(pending.request.token_ids, pending.request.mm_hashes)

    def get_finished(
        self, finished_req_ids: set[str]
    ) -> tuple[set[str] | None, set[str] | None]:
        return None, None

    def shutdown(self) -> None:
        unregister = getattr(self._store, "unregister_buffer", None)
        if callable(unregister):
            for ptr in list(self._registered_gpu_buffers):
                try:
                    unregister(ptr)
                except Exception:
                    logger.debug(
                        "Mooncake unregister_buffer failed ptr=%d",
                        ptr,
                        exc_info=True,
                    )
        self._registered_gpu_buffers.clear()
        self._known_gpu_storages.clear()
        self._pending_saves.clear()

    def __del__(self):
        try:
            self.shutdown()
        except Exception:
            pass

    def get_num_new_matched_tokens(
        self,
        request: "Request",
        num_computed_tokens: int,
    ) -> tuple[int | None, bool]:
        if not self._found_match_for_request(request):
            return 0, False
        logger.info("Mooncake Store external cache hit")
        token_ids = list(request.prompt_token_ids or [])
        num_tokens_to_check = align_to_block_size(
            len(token_ids) - 1, self._block_size
        )
        return num_tokens_to_check - num_computed_tokens, False

    def update_state_after_alloc(
        self,
        request: "Request",
        blocks: "KVCacheBlocks",
        num_external_tokens: int,
    ) -> None:
        if num_external_tokens > 0:
            self._requests_need_load[request.request_id] = request

    def build_connector_meta(
        self,
        scheduler_output: SchedulerOutput,
    ) -> KVConnectorMetadata:
        meta = MooncakeStoreConnectorMetadata()
        total_need_load = 0

        for new_req in scheduler_output.scheduled_new_reqs:
            token_ids = list(new_req.prompt_token_ids or [])
            mm_hashes = [f.identifier for f in new_req.mm_features]
            if new_req.req_id in self._requests_need_load:
                meta.add_request(
                    token_ids=token_ids,
                    block_ids=new_req.block_ids[0],
                    block_size=self._block_size,
                    is_store=False,
                    mm_hashes=mm_hashes,
                )
                total_need_load += 1
            elif not self._found_match_for_prompt(token_ids, mm_hashes):
                meta.add_request(
                    token_ids=token_ids,
                    block_ids=new_req.block_ids[0],
                    block_size=self._block_size,
                    is_store=True,
                    mm_hashes=mm_hashes,
                )

        cached_reqs = scheduler_output.scheduled_cached_reqs
        for i, req_id in enumerate(cached_reqs.req_ids):
            resumed_from_preemption = req_id in cached_reqs.resumed_req_ids
            if not resumed_from_preemption or req_id not in self._requests_need_load:
                continue

            num_computed_tokens = cached_reqs.num_computed_tokens[i]
            num_new_tokens = scheduler_output.num_scheduled_tokens[req_id]
            new_block_ids = cached_reqs.new_block_ids[i]
            request = self._requests_need_load[req_id]
            total_tokens = num_computed_tokens + num_new_tokens
            token_ids = request.all_token_ids[:total_tokens]
            assert new_block_ids is not None

            meta.add_request(
                token_ids=token_ids,
                block_ids=new_block_ids[0],
                block_size=self._block_size,
                is_store=False,
                mm_hashes=[f.identifier for f in request.mm_features],
            )
            total_need_load += 1

        assert total_need_load == len(self._requests_need_load)
        self._requests_need_load.clear()
        return meta

    def request_finished(
        self,
        request: "Request",
        block_ids: list[int],
    ) -> tuple[bool, dict[str, Any] | None]:
        return False, None

    def _maybe_register_for_direct_read(self, tensor: torch.Tensor,
                                        label: str) -> None:
        register_buffer = getattr(self._store, "register_buffer", None)
        if not callable(register_buffer):
            return
        if not tensor.is_cuda:
            return
        storage = tensor.untyped_storage()
        base_ptr = int(storage.data_ptr())
        size = int(storage.nbytes())
        if base_ptr in self._registered_gpu_buffers:
            return
        rc = register_buffer(base_ptr, size)
        if rc != 0:
            logger.warning(
                "Mooncake direct-read GPU RDMA/SPDK registration failed for %s: "
                "ptr=%d size=%d rc=%s. Continuing without pre-registration.",
                label,
                base_ptr,
                size,
                rc,
            )
            return
        self._registered_gpu_buffers[base_ptr] = size
        logger.info(
            "Mooncake Store registered GPU KV storage with RDMA and SPDK for "
            "direct read: "
            "label=%s ptr=%d size=%d",
            label,
            base_ptr,
            size,
        )

    def _block_buffer_vectors(
        self,
        kv_layer: torch.Tensor,
        request: ReqMeta,
        attn_metadata: AttentionMetadata,
    ) -> tuple[list[int], list[int]]:
        block_ids = request.block_ids
        if not block_ids:
            return [], []

        groups = group_contiguous(block_ids)
        ptrs: list[int] = []
        sizes: list[int] = []

        if isinstance(attn_metadata, MLACommonMetadata):
            block_bytes = int(kv_layer[0].nbytes)
            for group in groups:
                ptrs.append(int(kv_layer[group[0]].data_ptr()))
                sizes.append(block_bytes * len(group))
            return ptrs, sizes

        block_bytes = int(kv_layer[0, 0].nbytes)
        for kv_index in (0, 1):
            for group in groups:
                ptrs.append(int(kv_layer[kv_index, group[0]].data_ptr()))
                sizes.append(block_bytes * len(group))
        return ptrs, sizes

    def _put(self, key: str, value: bytes) -> int:
        if self._replicate_config is None:
            return self._store.put(key, value)
        try:
            return self._store.put(key, value, self._replicate_config)
        except TypeError:
            return self._store.put(key, value)

    def _request_key(self, request: ReqMeta) -> str:
        return self._object_key(
            "__all_layers__", request.token_ids, request.mm_hashes
        )

    def _cpu_layer_key(self, layer_name: str) -> str:
        digest = safe_hash(
            layer_name.encode("utf-8"), usedforsecurity=False
        ).hexdigest()
        return f"layer_{digest}"

    def _put_marker(self, token_ids: torch.Tensor, mm_hashes: list[str]) -> None:
        marker_key = self._marker_key(token_ids, mm_hashes)
        self._put(marker_key, b"1")

    def _found_match_for_request(self, request: "Request") -> bool:
        return self._found_match_for_prompt(
            list(request.prompt_token_ids or []),
            [f.identifier for f in request.mm_features],
        )

    def _found_match_for_prompt(
        self,
        prompt_token_ids: list[int],
        mm_hashes: list[str],
    ) -> bool:
        num_tokens_to_check = align_to_block_size(
            len(prompt_token_ids) - 1, self._block_size
        )
        marker_key = self._marker_key(
            torch.tensor(prompt_token_ids)[:num_tokens_to_check], mm_hashes
        )
        is_exist = getattr(self._store, "is_exist", None)
        if callable(is_exist):
            try:
                return int(is_exist(marker_key)) == 1
            except Exception:
                logger.debug("Mooncake is_exist failed for %s", marker_key,
                             exc_info=True)
        try:
            return self._store.get(marker_key) is not None
        except Exception:
            return False

    def _marker_key(self, token_ids: torch.Tensor, mm_hashes: list[str]) -> str:
        return self._object_key("__marker__v2", token_ids, mm_hashes)

    def _object_key(
        self,
        layer_name: str,
        token_ids: torch.Tensor,
        mm_hashes: list[str],
    ) -> str:
        token_bytes = token_ids.cpu().numpy().tobytes()
        if mm_hashes:
            token_bytes += ("-".join(mm_hashes)).encode("utf-8")
        digest = safe_hash(token_bytes, usedforsecurity=False).hexdigest()
        data_format = "raw-gpu-blocks" if self._gpu_direct_enabled else "safetensors"
        return f"{self._cache_prefix}:{data_format}:{digest}:{layer_name}"

    def _extract_kv_from_layer(
        self,
        layer: torch.Tensor,
        slot_mapping: torch.Tensor,
        attn_metadata: AttentionMetadata,
    ) -> torch.Tensor:
        if isinstance(attn_metadata, MLACommonMetadata):
            num_pages, page_size = layer.shape[0], layer.shape[1]
            return layer.reshape(num_pages * page_size, -1)[slot_mapping, ...]
        num_pages, page_size = layer.shape[1], layer.shape[2]
        return layer.reshape(2, num_pages * page_size, -1)[:, slot_mapping, ...]

    def _inject_kv_into_layer(
        self,
        dst_kv_cache_layer: torch.Tensor,
        src_kv_cache: torch.Tensor,
        slot_mapping: torch.Tensor,
        attn_metadata: AttentionMetadata,
    ) -> None:
        dst_shape = dst_kv_cache_layer.shape
        if isinstance(attn_metadata, MLACommonMetadata):
            num_pages = dst_shape[0]
            page_size = dst_shape[1]
            dst = dst_kv_cache_layer.reshape(num_pages * page_size, -1)
            dst[slot_mapping, ...] = src_kv_cache
            dst.reshape(dst_shape)
            return
        num_pages = dst_shape[1]
        page_size = dst_shape[2]
        dst = dst_kv_cache_layer.reshape(2, num_pages * page_size, -1)
        dst[:, slot_mapping, ...] = src_kv_cache
        dst.reshape(dst_shape)
