#!/usr/bin/env python3
"""Mooncake Store end-to-end KV benchmark."""

from __future__ import annotations

import argparse
import ctypes
import logging
import math
import os
import random
import statistics
import threading
import time
from collections import Counter
from dataclasses import dataclass, field
from typing import Callable, Iterable, List, Optional
from mooncake.store import (
    MooncakeDistributedStore,
    ReplicateConfig,
    get_alloc_func_addr,
    get_free_func_addr,
)


LOG = logging.getLogger("store_kv_bench")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Mooncake Store end-to-end KV benchmark",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--scenario",
        required=True,
        choices=["verify_write", "fill", "write_perf", "read_perf", "mixed_rw"],
        help="Benchmark scenario to execute.",
    )

    parser.add_argument("--local-hostname", default="127.0.0.1:50071")
    parser.add_argument("--metadata-server", default="http://127.0.0.1:8080/metadata")
    parser.add_argument("--master-server", default="127.0.0.1:50051")
    parser.add_argument("--protocol", default="tcp")
    parser.add_argument("--device-name", default="")
    parser.add_argument("--global-segment-size", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--local-buffer-size", type=int, default=32 * 1024 * 1024)
    parser.add_argument(
        "--io-api",
        choices=["plain", "zcopy"],
        default="plain",
        help="plain uses put/get/put_batch/get_batch, zcopy uses put_from/get_into/batch_put_from/batch_get_into",
    )
    parser.add_argument(
        "--memory-kind",
        choices=["host", "cuda"],
        default="host",
        help="Backing memory for zcopy buffers. CUDA requires a GPU-enabled Mooncake build.",
    )
    parser.add_argument(
        "--buffer-layout",
        choices=["single", "multi"],
        default="single",
        help="Use one buffer or multiple slices for each zcopy object.",
    )
    parser.add_argument(
        "--segments",
        type=int,
        default=3,
        help="Number of balanced slices in multi layout when segment-sizes is omitted.",
    )
    parser.add_argument(
        "--segment-sizes",
        default="",
        help="Comma-separated per-object slice sizes; their sum must equal value-size.",
    )
    parser.add_argument(
        "--buffer-offset",
        type=int,
        default=0,
        help="Byte offset from each registered slot base, useful for unaligned-address tests.",
    )
    parser.add_argument(
        "--segment-gap",
        type=int,
        default=0,
        help="Unused bytes between slices in multi layout, useful for non-contiguous SGL tests.",
    )
    parser.add_argument(
        "--cuda-device",
        type=int,
        default=0,
        help="CUDA device used when memory-kind=cuda.",
    )
    parser.add_argument(
        "--block-size",
        type=int,
        default=4096,
        help="NoF namespace block size used for physical-I/O and padding estimates.",
    )

    parser.add_argument("--numjobs", type=int, default=1)
    parser.add_argument("--iodepth", type=int, default=1)
    parser.add_argument("--batch-size", type=int, default=1)
    parser.add_argument("--runtime", type=int, default=0, help="Seconds. 0 means object-count based.")
    parser.add_argument("--nr-objects", type=int, default=128)
    parser.add_argument("--write-objects", type=int, default=0)
    parser.add_argument(
        "--prepare-objects",
        type=int,
        default=0,
        help="Object count used by the prepare phase. 0 means reuse nr-objects.",
    )
    parser.add_argument("--object-id-start", type=int, default=0)
    parser.add_argument("--key-prefix", default="kvbench")
    parser.add_argument("--key-size", type=int, default=20)
    parser.add_argument("--value-size", type=int, default=4096)
    parser.add_argument("--rand-seed", type=int, default=1)

    parser.add_argument("--memory-replica-num", type=int, default=1)
    parser.add_argument("--nof-replica-num", type=int, default=0)
    parser.add_argument(
        "--preferred-segment",
        default="",
        help="Optional Mooncake segment name used to force a specific NoF target.",
    )

    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--pattern", default="")
    parser.add_argument("--prepare-mode", choices=["auto", "none", "write"], default="auto")
    parser.add_argument("--rwmixread", type=int, default=70)

    parser.add_argument(
        "--phase-gap-mode",
        choices=["none", "sleep", "manual", "file"],
        default="none",
    )
    parser.add_argument("--phase-gap-sec", type=int, default=0)
    parser.add_argument("--phase-gap-file", default="")
    parser.add_argument("--phase-gap-timeout-sec", type=int, default=600)
    parser.add_argument("--log-level", default="INFO")
    return parser


def setup_logging(level: str) -> None:
    logging.basicConfig(
        level=getattr(logging, level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )


@dataclass
class PhaseStats:
    name: str
    request_latencies: List[float] = field(default_factory=list)
    requests: int = 0
    successful_requests: int = 0
    failed_requests: int = 0
    kvs: int = 0
    successful_kvs: int = 0
    failed_kvs: int = 0
    misses: int = 0
    verify_failures: int = 0
    bytes_processed: int = 0
    estimated_physical_bytes: int = 0
    padding_bytes: int = 0
    error_counts: Counter = field(default_factory=Counter)
    start_time: float = 0.0
    end_time: float = 0.0
    dataset_exhausted: bool = False


@dataclass
class RequestResult:
    request_ok: bool
    kv_successes: int
    kv_failures: int
    bytes_processed: int
    estimated_physical_bytes: int
    padding_bytes: int
    successful_object_ids: List[int] = field(default_factory=list)
    misses: int = 0
    verify_failures: int = 0
    error_counts: Counter = field(default_factory=Counter)


class PayloadFactory:
    def __init__(self, value_size: int, pattern: bytes):
        self.value_size = value_size
        self.pattern = pattern
        self._default_cache: dict[int, bytes] = {}
        self._pattern_payload = self._repeat(pattern) if pattern else b""

    def _repeat(self, token: bytes) -> bytes:
        repeat = (self.value_size + len(token) - 1) // len(token)
        return (token * repeat)[: self.value_size]

    def build(self, object_id: int) -> bytes:
        if self.pattern:
            return self._pattern_payload
        fill_byte = object_id & 0xFF
        payload = self._default_cache.get(fill_byte)
        if payload is None:
            payload = bytes([fill_byte]) * self.value_size
            self._default_cache[fill_byte] = payload
        return payload

    def verify_payload(self, object_id: int, payload: bytes) -> bool:
        return payload == self.build(object_id)


class DatasetState:
    def __init__(self, object_id_start: int):
        self.write_lock = threading.Lock()
        self.ids_lock = threading.Lock()
        self.cursor_lock = threading.Lock()
        self.next_write_id = object_id_start
        self.prepared_ids: tuple[int, ...] = ()
        self.written_ids: tuple[int, ...] = ()
        self.read_cursor = 0

    def reserve_write_ids(self, count: int, upper_bound: int) -> List[int]:
        with self.write_lock:
            if self.next_write_id >= upper_bound:
                return []
            end = min(self.next_write_id + count, upper_bound)
            ids = list(range(self.next_write_id, end))
            self.next_write_id = end
            return ids

    def mark_prepared(self, ids: Iterable[int]) -> None:
        ids = tuple(ids)
        if not ids:
            return
        with self.ids_lock:
            self.prepared_ids = self.prepared_ids + ids
            self.written_ids = self.written_ids + ids

    def mark_runtime_written(self, ids: Iterable[int]) -> None:
        ids = tuple(ids)
        if not ids:
            return
        with self.ids_lock:
            self.written_ids = self.written_ids + ids

    def written_count(self) -> int:
        with self.ids_lock:
            return len(self.written_ids)

    def snapshot_written_ids(self) -> List[int]:
        with self.ids_lock:
            return list(self.written_ids)

    def next_read_ids(
        self,
        count: int,
        *,
        loop: bool,
        sequential: bool,
        rng,
        source: str = "written",
    ) -> List[int]:
        with self.ids_lock:
            readable_ids = self.prepared_ids if source == "prepared" else self.written_ids
        if not readable_ids:
            return []
        if sequential:
            with self.cursor_lock:
                result = []
                for _ in range(count):
                    if self.read_cursor >= len(readable_ids):
                        if not loop:
                            break
                        self.read_cursor = 0
                    result.append(readable_ids[self.read_cursor])
                    self.read_cursor += 1
                return result
        return [readable_ids[rng.randrange(len(readable_ids))] for _ in range(count)]


def parse_pattern(pattern_text: str) -> bytes:
    if not pattern_text:
        return b""
    if pattern_text.startswith("0x"):
        hex_text = pattern_text[2:]
        if len(hex_text) % 2 != 0:
            raise ValueError("hex pattern length must be even")
        return bytes.fromhex(hex_text)
    return pattern_text.encode("utf-8")


def parse_segment_sizes(segment_sizes_text: str) -> List[int]:
    if not segment_sizes_text:
        return []
    try:
        sizes = [int(value.strip()) for value in segment_sizes_text.split(",")]
    except ValueError as exc:
        raise ValueError("segment-sizes must be a comma-separated integer list") from exc
    if not sizes or any(size <= 0 for size in sizes):
        raise ValueError("all segment-sizes entries must be > 0")
    return sizes


def resolve_segment_sizes(
    value_size: int,
    buffer_layout: str,
    segment_count: int,
    segment_sizes_text: str,
) -> List[int]:
    explicit_sizes = parse_segment_sizes(segment_sizes_text)
    if buffer_layout == "single":
        if explicit_sizes:
            raise ValueError("segment-sizes requires --buffer-layout=multi")
        return [value_size]

    if explicit_sizes:
        if len(explicit_sizes) < 2:
            raise ValueError("multi layout requires at least two segment sizes")
        if sum(explicit_sizes) != value_size:
            raise ValueError(f"segment-sizes sum={sum(explicit_sizes)} does not match " f"value-size={value_size}")
        return explicit_sizes

    if segment_count < 2:
        raise ValueError("multi layout requires --segments >= 2")
    if segment_count > value_size:
        raise ValueError("segments cannot exceed value-size")
    base_size, remainder = divmod(value_size, segment_count)
    return [base_size + (1 if index < remainder else 0) for index in range(segment_count)]


def align_up(value: int, alignment: int) -> int:
    return ((value + alignment - 1) // alignment) * alignment


def make_key(prefix: str, key_size: int, object_id: int) -> str:
    suffix = f"{object_id:016d}"
    if key_size < len(suffix):
        raise ValueError(f"key_size={key_size} is smaller than suffix length {len(suffix)}")
    prefix_space = key_size - len(suffix)
    prefix_part = prefix[:prefix_space].ljust(prefix_space, "_")
    return f"{prefix_part}{suffix}"


class StoreSession:
    def __init__(
        self,
        args: argparse.Namespace,
        lane_id: int,
        payload_factory: PayloadFactory,
        store_obj,
        zcopy: Optional["ZcopyBufferView"] = None,
    ):
        self.args = args
        self.lane_id = lane_id
        self.payload_factory = payload_factory
        self.store = store_obj
        self.config = ReplicateConfig()
        self.config.replica_num = args.memory_replica_num
        self.config.nof_replica_num = args.nof_replica_num
        if args.preferred_segment:
            self.config.preferred_segment = args.preferred_segment
        self._zcopy = zcopy

    def close(self) -> None:
        self._zcopy = None

    def _successful_byte_counts(self, successful_kvs: int, nof_copies: int) -> tuple[int, int, int]:
        logical_bytes = successful_kvs * self.args.value_size
        if self.args.memory_replica_num != 0 or self.args.nof_replica_num == 0:
            return logical_bytes, logical_bytes, 0
        physical_size = align_up(self.args.value_size, self.args.block_size)
        estimated_physical_bytes = successful_kvs * physical_size * nof_copies
        return (
            logical_bytes,
            estimated_physical_bytes,
            successful_kvs * (physical_size - self.args.value_size) * nof_copies,
        )

    def put_ids(self, object_ids: List[int]) -> RequestResult:
        keys = [make_key(self.args.key_prefix, self.args.key_size, object_id) for object_id in object_ids]
        ret_codes = self._put_keys(keys, object_ids)

        errors: Counter = Counter()
        success_ids: List[int] = []
        if self.args.io_api == "plain" and not (len(object_ids) == 1 and self.args.batch_size == 1):
            ret = ret_codes[0]
            if ret == 0:
                success_ids.extend(object_ids)
            else:
                errors[ret] += 1
        else:
            for object_id, ret in zip(object_ids, ret_codes):
                if ret == 0:
                    success_ids.append(object_id)
                else:
                    errors[ret] += 1
        request_ok = len(success_ids) == len(object_ids)
        logical_bytes, physical_bytes, padding_bytes = self._successful_byte_counts(
            len(success_ids), self.args.nof_replica_num
        )
        return RequestResult(
            request_ok=request_ok,
            kv_successes=len(success_ids),
            kv_failures=len(object_ids) - len(success_ids),
            bytes_processed=logical_bytes,
            estimated_physical_bytes=physical_bytes,
            padding_bytes=padding_bytes,
            successful_object_ids=success_ids,
            error_counts=errors,
        )

    def get_ids(self, object_ids: List[int], verify: bool) -> RequestResult:
        keys = [make_key(self.args.key_prefix, self.args.key_size, object_id) for object_id in object_ids]
        errors: Counter = Counter()
        kv_successes = 0
        misses = 0
        verify_failures = 0
        if self.args.io_api == "plain":
            payloads = self._get_payloads_plain(keys)
            for object_id, payload in zip(object_ids, payloads):
                if payload in (None, b""):
                    misses += 1
                    errors["MISS"] += 1
                    continue
                if verify and not self.payload_factory.verify_payload(object_id, payload):
                    verify_failures += 1
                    errors["VERIFY_FAIL"] += 1
                    continue
                kv_successes += 1
        else:
            assert self._zcopy is not None
            lengths = self._get_lengths_zcopy(keys, len(object_ids))
            for slot, (object_id, length) in enumerate(zip(object_ids, lengths)):
                if length < 0:
                    if self.store.isExist(keys[slot]) == 0:
                        misses += 1
                        errors["MISS"] += 1
                    else:
                        errors[length] += 1
                    continue
                payload = self._zcopy.read_bytes(slot, length)
                if verify:
                    if length != self.args.value_size:
                        verify_failures += 1
                        errors["VERIFY_SIZE_MISMATCH"] += 1
                        continue
                    if not self.payload_factory.verify_payload(object_id, payload):
                        verify_failures += 1
                        errors["VERIFY_FAIL"] += 1
                        continue
                kv_successes += 1

        kv_failures = len(object_ids) - kv_successes
        logical_bytes, physical_bytes, padding_bytes = self._successful_byte_counts(kv_successes, 1)
        return RequestResult(
            request_ok=(kv_failures == 0),
            kv_successes=kv_successes,
            kv_failures=kv_failures,
            bytes_processed=logical_bytes,
            estimated_physical_bytes=physical_bytes,
            padding_bytes=padding_bytes,
            misses=misses,
            verify_failures=verify_failures,
            error_counts=errors,
        )

    def _put_keys(self, keys: List[str], object_ids: List[int]) -> List[int]:
        values = [self.payload_factory.build(object_id) for object_id in object_ids]
        if self.args.io_api == "plain":
            if len(object_ids) == 1 and self.args.batch_size == 1:
                return [self.store.put(keys[0], values[0], self.config)]
            return [self.store.put_batch(keys, values, self.config)]

        assert self._zcopy is not None
        all_ptrs = self._zcopy.fill_write_buffers(values)
        sizes = [len(value) for value in values]
        if self.args.buffer_layout == "multi":
            all_sizes = [self._zcopy.segment_sizes for _ in values]
            return list(self.store.batch_put_from_multi_buffers(keys, all_ptrs, all_sizes, self.config))
        ptrs = [ptrs[0] for ptrs in all_ptrs]
        if len(object_ids) == 1 and self.args.batch_size == 1:
            return [self.store.put_from(keys[0], ptrs[0], sizes[0], self.config)]
        return list(self.store.batch_put_from(keys, ptrs, sizes, self.config))

    def _get_payloads_plain(self, keys: List[str]) -> List[bytes]:
        if len(keys) == 1 and self.args.batch_size == 1:
            return [self.store.get(keys[0])]
        return list(self.store.get_batch(keys))

    def _get_lengths_zcopy(self, keys: List[str], slot_count: int) -> List[int]:
        assert self._zcopy is not None
        all_ptrs = self._zcopy.prepare_read_buffers(slot_count)
        if self.args.buffer_layout == "multi":
            all_sizes = [self._zcopy.segment_sizes for _ in range(slot_count)]
            return list(self.store.batch_get_into_multi_buffers(keys, all_ptrs, all_sizes))
        ptrs = [ptrs[0] for ptrs in all_ptrs]
        sizes = [self.args.value_size] * slot_count
        if slot_count == 1 and self.args.batch_size == 1:
            return [self.store.get_into(keys[0], ptrs[0], sizes[0])]
        return list(self.store.batch_get_into(keys, ptrs, sizes))


class ZcopyBufferPool:
    def __init__(
        self,
        store_obj,
        value_size: int,
        slots: int,
        segment_sizes: List[int],
        buffer_offset: int,
        segment_gap: int,
    ):
        self.store = store_obj
        self.value_size = value_size
        self.slots = slots
        self.segment_sizes = tuple(segment_sizes)
        self.buffer_offset = buffer_offset
        self.segment_gap = segment_gap
        segment_offset = 0
        self.segment_offsets = []
        for index, size in enumerate(self.segment_sizes):
            self.segment_offsets.append(segment_offset)
            segment_offset += size
            if index + 1 < len(self.segment_sizes):
                segment_offset += self.segment_gap
        self.slot_stride = self.buffer_offset + segment_offset
        self.total_size = self.slot_stride * self.slots
        self._registered = False
        self.base_ptr = 0

    def _register(self) -> None:
        ret = self.store.register_buffer(self.base_ptr, self.total_size)
        if ret != 0:
            raise RuntimeError(f"register_buffer failed for zcopy pool ptr={self.base_ptr}: {ret}")
        self._registered = True

    def close(self) -> None:
        if self.base_ptr and self._registered:
            try:
                self.store.unregister_buffer(self.base_ptr)
            except Exception:
                LOG.debug(
                    "unregister_buffer failed for zcopy pool ptr=%s",
                    self.base_ptr,
                    exc_info=True,
                )
            self._registered = False
        self._release()
        self.base_ptr = 0

    def _release(self) -> None:
        raise NotImplementedError

    def slot_ptr(self, slot: int) -> int:
        if slot < 0 or slot >= self.slots:
            raise IndexError(f"zcopy slot {slot} is out of range [0, {self.slots})")
        return self.base_ptr + slot * self.slot_stride + self.buffer_offset

    def segment_ptrs(self, slot: int) -> List[int]:
        slot_ptr = self.slot_ptr(slot)
        return [slot_ptr + offset for offset in self.segment_offsets]

    def write_bytes(self, slot: int, payload: bytes) -> None:
        raise NotImplementedError

    def clear_bytes(self, slot: int) -> None:
        raise NotImplementedError

    def read_bytes(self, slot: int, size: int) -> bytes:
        raise NotImplementedError

    def synchronize(self) -> None:
        pass


class HostZcopyBufferPool(ZcopyBufferPool):
    def __init__(
        self,
        store_obj,
        value_size: int,
        slots: int,
        segment_sizes: List[int],
        buffer_offset: int,
        segment_gap: int,
    ):
        super().__init__(
            store_obj,
            value_size,
            slots,
            segment_sizes,
            buffer_offset,
            segment_gap,
        )
        alloc_addr = get_alloc_func_addr()
        free_addr = get_free_func_addr()
        if alloc_addr is None or free_addr is None:
            raise RuntimeError("store module does not expose hugepage alloc/free helpers")

        self._alloc_fn = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_size_t)(alloc_addr)
        self._free_fn = ctypes.CFUNCTYPE(None, ctypes.c_void_p)(free_addr)
        self._buffer = None

        raw_ptr = self._alloc_fn(self.total_size)
        self.base_ptr = ctypes.cast(raw_ptr, ctypes.c_void_p).value or 0
        if self.base_ptr == 0:
            raise RuntimeError(f"direct hugepage alloc failed for zcopy pool: size={self.total_size}")
        try:
            self._register()
        except Exception:
            self._free_fn(ctypes.c_void_p(self.base_ptr))
            self.base_ptr = 0
            raise
        self._buffer = (ctypes.c_ubyte * self.total_size).from_address(self.base_ptr)

    def _release(self) -> None:
        self._buffer = None
        if self.base_ptr:
            self._free_fn(ctypes.c_void_p(self.base_ptr))

    def write_bytes(self, slot: int, payload: bytes) -> None:
        if len(payload) != self.value_size:
            raise ValueError(f"Payload size {len(payload)} does not match value-size {self.value_size}")
        payload_offset = 0
        for ptr, size in zip(self.segment_ptrs(slot), self.segment_sizes):
            ctypes.memmove(ptr, payload[payload_offset : payload_offset + size], size)
            payload_offset += size

    def clear_bytes(self, slot: int) -> None:
        for ptr, size in zip(self.segment_ptrs(slot), self.segment_sizes):
            ctypes.memset(ptr, 0, size)

    def read_bytes(self, slot: int, size: int) -> bytes:
        if size > self.value_size:
            raise ValueError(f"Read size {size} exceeds slot size {self.value_size}")
        remaining = size
        chunks = []
        for ptr, segment_size in zip(self.segment_ptrs(slot), self.segment_sizes):
            chunk_size = min(remaining, segment_size)
            if chunk_size == 0:
                break
            chunks.append(ctypes.string_at(ptr, chunk_size))
            remaining -= chunk_size
        return b"".join(chunks)


class CudaZcopyBufferPool(ZcopyBufferPool):
    def __init__(
        self,
        store_obj,
        value_size: int,
        slots: int,
        segment_sizes: List[int],
        buffer_offset: int,
        segment_gap: int,
        cuda_device: int,
    ):
        super().__init__(
            store_obj,
            value_size,
            slots,
            segment_sizes,
            buffer_offset,
            segment_gap,
        )
        try:
            import torch
        except ImportError as exc:
            raise RuntimeError("memory-kind=cuda requires PyTorch") from exc
        if not torch.cuda.is_available():
            raise RuntimeError("memory-kind=cuda requires an available CUDA device")
        if cuda_device < 0 or cuda_device >= torch.cuda.device_count():
            raise ValueError(f"cuda-device={cuda_device} is outside [0, {torch.cuda.device_count()})")

        self._torch = torch
        self.cuda_device = cuda_device
        with torch.cuda.device(self.cuda_device):
            self._tensor = torch.empty(self.total_size, dtype=torch.uint8, device=f"cuda:{self.cuda_device}")
        self.base_ptr = self._tensor.data_ptr()
        try:
            self._register()
        except Exception:
            self._tensor = None
            self.base_ptr = 0
            raise

    def _release(self) -> None:
        self._tensor = None

    def _segment_tensor(self, slot: int, segment_index: int, size: int):
        if size > self.segment_sizes[segment_index]:
            raise ValueError(f"Buffer size {size} exceeds segment size " f"{self.segment_sizes[segment_index]}")
        start = slot * self.slot_stride + self.buffer_offset + self.segment_offsets[segment_index]
        return self._tensor.narrow(0, start, size)

    def write_bytes(self, slot: int, payload: bytes) -> None:
        if len(payload) != self.value_size:
            raise ValueError(f"Payload size {len(payload)} does not match value-size {self.value_size}")
        with self._torch.cuda.device(self.cuda_device):
            payload_offset = 0
            for index, segment_size in enumerate(self.segment_sizes):
                payload_chunk = bytearray(payload[payload_offset : payload_offset + segment_size])
                host_tensor = self._torch.frombuffer(payload_chunk, dtype=self._torch.uint8)
                self._segment_tensor(slot, index, segment_size).copy_(host_tensor)
                payload_offset += segment_size

    def clear_bytes(self, slot: int) -> None:
        with self._torch.cuda.device(self.cuda_device):
            for index, segment_size in enumerate(self.segment_sizes):
                self._segment_tensor(slot, index, segment_size).zero_()

    def read_bytes(self, slot: int, size: int) -> bytes:
        if size > self.value_size:
            raise ValueError(f"Read size {size} exceeds slot size {self.value_size}")
        with self._torch.cuda.device(self.cuda_device):
            remaining = size
            chunks = []
            for index, segment_size in enumerate(self.segment_sizes):
                chunk_size = min(remaining, segment_size)
                if chunk_size == 0:
                    break
                chunks.append(self._segment_tensor(slot, index, chunk_size).cpu().numpy().tobytes())
                remaining -= chunk_size
            return b"".join(chunks)

    def synchronize(self) -> None:
        with self._torch.cuda.device(self.cuda_device):
            self._torch.cuda.synchronize(self.cuda_device)


class ZcopyBufferView:
    def __init__(self, pool: ZcopyBufferPool, slot_offset: int, slots: int):
        self.pool = pool
        self.slot_offset = slot_offset
        self.slots = slots

    @property
    def segment_sizes(self) -> List[int]:
        return list(self.pool.segment_sizes)

    def _global_slot(self, slot: int) -> int:
        if slot < 0 or slot >= self.slots:
            raise IndexError(f"zcopy view slot {slot} is out of range [0, {self.slots})")
        return self.slot_offset + slot

    def fill_write_buffers(self, payloads: List[bytes]) -> List[List[int]]:
        all_ptrs: List[List[int]] = []
        for slot, payload in enumerate(payloads):
            global_slot = self._global_slot(slot)
            self.pool.write_bytes(global_slot, payload)
            all_ptrs.append(self.pool.segment_ptrs(global_slot))
        self.pool.synchronize()
        return all_ptrs

    def prepare_read_buffers(self, slot_count: int) -> List[List[int]]:
        all_ptrs: List[List[int]] = []
        for slot in range(slot_count):
            global_slot = self._global_slot(slot)
            self.pool.clear_bytes(global_slot)
            all_ptrs.append(self.pool.segment_ptrs(global_slot))
        self.pool.synchronize()
        return all_ptrs

    def read_bytes(self, slot: int, size: int) -> bytes:
        return self.pool.read_bytes(self._global_slot(slot), size)


class StoreRuntime:
    def __init__(self, args: argparse.Namespace, lane_count: int):
        self.lane_count = lane_count
        if args.memory_kind == "cuda":
            try:
                import torch
            except ImportError as exc:
                raise RuntimeError("memory-kind=cuda requires PyTorch") from exc
            if not torch.cuda.is_available():
                raise RuntimeError("memory-kind=cuda requires an available CUDA device")
            if args.cuda_device < 0 or args.cuda_device >= torch.cuda.device_count():
                raise ValueError(f"cuda-device={args.cuda_device} is outside " f"[0, {torch.cuda.device_count()})")
            torch.cuda.set_device(args.cuda_device)

        self.store = MooncakeDistributedStore()
        setup_ret = self.store.setup(
            args.local_hostname,
            args.metadata_server,
            args.global_segment_size,
            args.local_buffer_size,
            args.protocol,
            args.device_name,
            args.master_server,
        )
        if setup_ret != 0:
            raise RuntimeError(f"setup failed: {setup_ret}")

        self.zcopy_pool: Optional[ZcopyBufferPool] = None
        if args.io_api == "zcopy":
            slots = max(1, args.batch_size) * lane_count
            pool_args = (
                self.store,
                args.value_size,
                slots,
                args.resolved_segment_sizes,
                args.buffer_offset,
                args.segment_gap,
            )
            try:
                if args.memory_kind == "cuda":
                    self.zcopy_pool = CudaZcopyBufferPool(*pool_args, cuda_device=args.cuda_device)
                else:
                    self.zcopy_pool = HostZcopyBufferPool(*pool_args)
            except Exception:
                self.close()
                raise

    def make_session(
        self,
        args: argparse.Namespace,
        lane_id: int,
        payload_factory: PayloadFactory,
    ) -> StoreSession:
        zcopy_view: Optional[ZcopyBufferView] = None
        if self.zcopy_pool is not None:
            slots_per_lane = max(1, args.batch_size)
            zcopy_view = ZcopyBufferView(self.zcopy_pool, lane_id * slots_per_lane, slots_per_lane)
        return StoreSession(
            args,
            lane_id,
            payload_factory,
            self.store,
            zcopy_view,
        )

    def close(self) -> None:
        if self.zcopy_pool is not None:
            self.zcopy_pool.close()
            self.zcopy_pool = None
        if hasattr(self.store, "close"):
            try:
                self.store.close()
            except Exception:
                LOG.debug("shared store close failed", exc_info=True)
        elif hasattr(self.store, "tearDownAll"):
            try:
                self.store.tearDownAll()
            except Exception:
                LOG.debug("shared tearDownAll failed", exc_info=True)


def merge_stats(name: str, stats_list: List[PhaseStats]) -> PhaseStats:
    merged = PhaseStats(name=name)
    if not stats_list:
        return merged
    merged.start_time = min((s.start_time for s in stats_list if s.start_time), default=0.0)
    merged.end_time = max((s.end_time for s in stats_list if s.end_time), default=0.0)
    for stats in stats_list:
        merged.request_latencies.extend(stats.request_latencies)
        merged.requests += stats.requests
        merged.successful_requests += stats.successful_requests
        merged.failed_requests += stats.failed_requests
        merged.kvs += stats.kvs
        merged.successful_kvs += stats.successful_kvs
        merged.failed_kvs += stats.failed_kvs
        merged.misses += stats.misses
        merged.verify_failures += stats.verify_failures
        merged.bytes_processed += stats.bytes_processed
        merged.estimated_physical_bytes += stats.estimated_physical_bytes
        merged.padding_bytes += stats.padding_bytes
        merged.error_counts.update(stats.error_counts)
        merged.dataset_exhausted = merged.dataset_exhausted or stats.dataset_exhausted
    return merged


def percentile(values: List[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * p
    low = math.floor(rank)
    high = math.ceil(rank)
    if low == high:
        return ordered[low]
    return ordered[low] + (ordered[high] - ordered[low]) * (rank - low)


def summarize_stats(stats: PhaseStats) -> dict:
    duration = max(stats.end_time - stats.start_time, 0.0)
    amplification = stats.estimated_physical_bytes / stats.bytes_processed if stats.bytes_processed > 0 else 0.0
    return {
        "requests": stats.requests,
        "successful_requests": stats.successful_requests,
        "failed_requests": stats.failed_requests,
        "kvs": stats.kvs,
        "successful_kvs": stats.successful_kvs,
        "failed_kvs": stats.failed_kvs,
        "misses": stats.misses,
        "verify_failures": stats.verify_failures,
        "bytes": stats.bytes_processed,
        "estimated_physical_bytes": stats.estimated_physical_bytes,
        "padding_bytes": stats.padding_bytes,
        "io_amplification": amplification,
        "duration_sec": duration,
        "req_per_sec": (stats.requests / duration) if duration > 0 else 0.0,
        "kv_per_sec": (stats.kvs / duration) if duration > 0 else 0.0,
        "MiB_per_sec": ((stats.bytes_processed / duration / (1024 * 1024)) if duration > 0 else 0.0),
        "estimated_physical_MiB_per_sec": (
            stats.estimated_physical_bytes / duration / (1024 * 1024) if duration > 0 else 0.0
        ),
        "lat_mean_ms": (statistics.mean(stats.request_latencies) * 1000 if stats.request_latencies else 0.0),
        "lat_p50_ms": percentile(stats.request_latencies, 0.50) * 1000,
        "lat_p95_ms": percentile(stats.request_latencies, 0.95) * 1000,
        "lat_p99_ms": percentile(stats.request_latencies, 0.99) * 1000,
        "dataset_exhausted": stats.dataset_exhausted,
        "error_counts": dict(stats.error_counts),
    }


def log_phase_stats(stats: PhaseStats) -> None:
    summary = summarize_stats(stats)
    LOG.info("=== phase %s ===", stats.name)
    LOG.info(
        "requests=%d successful_requests=%d failed_requests=%d kvs=%d successful_kvs=%d failed_kvs=%d",
        summary["requests"],
        summary["successful_requests"],
        summary["failed_requests"],
        summary["kvs"],
        summary["successful_kvs"],
        summary["failed_kvs"],
    )
    LOG.info(
        "misses=%d verify_failures=%d bytes=%d duration=%.3fs req/s=%.2f kv/s=%.2f MiB/s=%.2f",
        summary["misses"],
        summary["verify_failures"],
        summary["bytes"],
        summary["duration_sec"],
        summary["req_per_sec"],
        summary["kv_per_sec"],
        summary["MiB_per_sec"],
    )
    LOG.info(
        "estimated_physical_bytes=%d padding_bytes=%d " "io_amplification=%.3fx estimated_physical_MiB/s=%.2f",
        summary["estimated_physical_bytes"],
        summary["padding_bytes"],
        summary["io_amplification"],
        summary["estimated_physical_MiB_per_sec"],
    )
    LOG.info(
        "lat_mean=%.3fms lat_p50=%.3fms lat_p95=%.3fms lat_p99=%.3fms dataset_exhausted=%s",
        summary["lat_mean_ms"],
        summary["lat_p50_ms"],
        summary["lat_p95_ms"],
        summary["lat_p99_ms"],
        summary["dataset_exhausted"],
    )
    if summary["error_counts"]:
        LOG.info("errors=%s", summary["error_counts"])


class BenchmarkRunner:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.pattern = parse_pattern(args.pattern)
        self.payload_factory = PayloadFactory(args.value_size, self.pattern)
        self.dataset = DatasetState(args.object_id_start)
        self.lane_count = args.numjobs * args.iodepth
        self._sessions: Optional[List[StoreSession]] = None
        self._runtime: Optional[StoreRuntime] = None
        self._validate_args()

    def _validate_args(self) -> None:
        if self.args.numjobs <= 0 or self.args.iodepth <= 0:
            raise ValueError("numjobs and iodepth must be > 0")
        if self.args.batch_size <= 0:
            raise ValueError("batch-size must be > 0")
        if self.args.value_size <= 0:
            raise ValueError("value-size must be > 0")
        if self.args.block_size <= 0 or (self.args.block_size & (self.args.block_size - 1)):
            raise ValueError("block-size must be a positive power of two")
        if self.args.buffer_offset < 0:
            raise ValueError("buffer-offset must be >= 0")
        if self.args.segment_gap < 0:
            raise ValueError("segment-gap must be >= 0")
        if self.args.io_api == "plain" and self.args.memory_kind != "host":
            raise ValueError("memory-kind=cuda requires --io-api=zcopy")
        if self.args.io_api == "plain" and self.args.buffer_layout != "single":
            raise ValueError("buffer-layout=multi requires --io-api=zcopy")
        if self.args.io_api == "plain" and self.args.buffer_offset != 0:
            raise ValueError("buffer-offset requires --io-api=zcopy")
        if self.args.buffer_layout == "single" and self.args.segment_gap != 0:
            raise ValueError("segment-gap requires --buffer-layout=multi")
        if self.args.key_size <= 0:
            raise ValueError("key-size must be > 0")
        if self.args.nr_objects <= 0:
            raise ValueError("nr-objects must be > 0")
        if self.args.write_objects < 0:
            raise ValueError("write-objects must be >= 0")
        if self.args.prepare_objects < 0:
            raise ValueError("prepare-objects must be >= 0")
        if self.args.rwmixread < 0 or self.args.rwmixread > 100:
            raise ValueError("rwmixread must be within [0, 100]")
        if self.args.verify and not self.pattern:
            raise ValueError("verify mode currently requires --pattern")
        if self.args.memory_replica_num == 0 and self.args.nof_replica_num == 0:
            raise ValueError("memory_replica_num and nof_replica_num cannot both be 0")
        if self.args.phase_gap_mode == "sleep" and self.args.phase_gap_sec <= 0:
            raise ValueError("phase-gap-sec must be > 0 when phase-gap-mode=sleep")
        if self.args.phase_gap_mode == "file" and not self.args.phase_gap_file:
            raise ValueError("phase-gap-file must be set when phase-gap-mode=file")
        if self.args.scenario == "mixed_rw" and self.args.runtime <= 0:
            raise ValueError("mixed_rw requires --runtime > 0")
        self.args.resolved_segment_sizes = resolve_segment_sizes(
            self.args.value_size,
            self.args.buffer_layout,
            self.args.segments,
            self.args.segment_sizes,
        )
        make_key(self.args.key_prefix, self.args.key_size, self.args.object_id_start)

    def _write_budget(self) -> int:
        return self.args.write_objects if self.args.write_objects > 0 else self.args.nr_objects

    def _prepare_budget(self) -> int:
        return self.args.prepare_objects if self.args.prepare_objects > 0 else self.args.nr_objects

    def _make_sessions(self) -> List[StoreSession]:
        if self._sessions is None:
            self._runtime = StoreRuntime(self.args, self.lane_count)
            self._sessions = [
                self._runtime.make_session(self.args, lane_id, self.payload_factory)
                for lane_id in range(self.lane_count)
            ]
        return self._sessions

    def close(self) -> None:
        if self._sessions is not None:
            for session in self._sessions:
                session.close()
            self._sessions = None
        if self._runtime is not None:
            self._runtime.close()
            self._runtime = None

    def _phase_gap(self, label: str) -> None:
        mode = self.args.phase_gap_mode
        if mode == "none":
            return
        LOG.info("phase gap before %s, mode=%s", label, mode)
        if mode == "sleep":
            LOG.info("sleeping %d seconds before %s", self.args.phase_gap_sec, label)
            time.sleep(self.args.phase_gap_sec)
            return
        if mode == "manual":
            input(f"phase '{label}' is waiting, finish external operations then press Enter to continue...")
            return
        deadline = time.time() + self.args.phase_gap_timeout_sec
        while time.time() < deadline:
            if os.path.exists(self.args.phase_gap_file):
                LOG.info(
                    "detected phase gap file %s, continuing to %s",
                    self.args.phase_gap_file,
                    label,
                )
                return
            time.sleep(1.0)
        raise TimeoutError(f"timed out waiting for phase gap file {self.args.phase_gap_file}")

    def _run_threads(
        self,
        phase_name: str,
        worker_builder: Callable[[StoreSession, int], Callable[[PhaseStats], None]],
    ) -> PhaseStats:
        sessions = self._make_sessions()
        per_lane_stats: List[Optional[PhaseStats]] = [None] * self.lane_count
        threads: List[threading.Thread] = []

        def runner(index: int, session: StoreSession) -> None:
            stats = PhaseStats(name=f"{phase_name}/lane{index}")
            stats.start_time = time.perf_counter()
            worker_builder(session, index)(stats)
            stats.end_time = time.perf_counter()
            per_lane_stats[index] = stats

        for lane_id, session in enumerate(sessions):
            thread = threading.Thread(
                target=runner,
                args=(lane_id, session),
                name=f"{phase_name}-lane{lane_id}",
            )
            threads.append(thread)
            thread.start()

        for thread in threads:
            thread.join()

        merged = merge_stats(phase_name, [s for s in per_lane_stats if s is not None])
        log_phase_stats(merged)
        return merged

    def _record(self, stats: PhaseStats, latency: float, request: RequestResult, kv_count: int) -> None:
        stats.request_latencies.append(latency)
        stats.requests += 1
        stats.kvs += kv_count
        if request.request_ok:
            stats.successful_requests += 1
        else:
            stats.failed_requests += 1
        stats.successful_kvs += request.kv_successes
        stats.failed_kvs += request.kv_failures
        stats.misses += request.misses
        stats.verify_failures += request.verify_failures
        stats.bytes_processed += request.bytes_processed
        stats.estimated_physical_bytes += request.estimated_physical_bytes
        stats.padding_bytes += request.padding_bytes
        stats.error_counts.update(request.error_counts)

    def _run_fixed_write(
        self,
        phase_name: str,
        total_objects: int,
        *,
        strict: bool,
        write_scope: str = "runtime",
    ) -> PhaseStats:
        write_upper = self.dataset.next_write_id + total_objects

        def worker(session: StoreSession, _lane_id: int) -> Callable[[PhaseStats], None]:
            def run(stats: PhaseStats) -> None:
                while True:
                    object_ids = self.dataset.reserve_write_ids(self.args.batch_size, write_upper)
                    if not object_ids:
                        break
                    start = time.perf_counter()
                    result = session.put_ids(object_ids)
                    latency = time.perf_counter() - start
                    self._record(stats, latency, result, len(object_ids))
                    if result.successful_object_ids:
                        if write_scope == "prepared":
                            self.dataset.mark_prepared(result.successful_object_ids)
                        else:
                            self.dataset.mark_runtime_written(result.successful_object_ids)

            return run

        stats = self._run_threads(phase_name, worker)
        expected = total_objects
        if stats.successful_kvs < expected:
            stats.dataset_exhausted = True
        if strict and (stats.failed_kvs > 0 or stats.successful_kvs != expected):
            raise RuntimeError(
                f"{phase_name} strict write failed: expected {expected} objects, "
                f"got success={stats.successful_kvs}, failed={stats.failed_kvs}"
            )
        return stats

    def _run_time_based_write(self, phase_name: str, total_objects: int) -> PhaseStats:
        deadline = time.time() + self.args.runtime
        write_upper = self.dataset.next_write_id + total_objects
        stop_event = threading.Event()

        def worker(session: StoreSession, _lane_id: int) -> Callable[[PhaseStats], None]:
            def run(stats: PhaseStats) -> None:
                while time.time() < deadline and not stop_event.is_set():
                    object_ids = self.dataset.reserve_write_ids(self.args.batch_size, write_upper)
                    if not object_ids:
                        stats.dataset_exhausted = True
                        stop_event.set()
                        break
                    start = time.perf_counter()
                    result = session.put_ids(object_ids)
                    latency = time.perf_counter() - start
                    self._record(stats, latency, result, len(object_ids))
                    if result.successful_object_ids:
                        self.dataset.mark_runtime_written(result.successful_object_ids)

            return run

        return self._run_threads(phase_name, worker)

    def _run_read_phase(
        self,
        phase_name: str,
        *,
        verify: bool,
        sequential: bool,
        loop: bool,
        runtime_sec: int = 0,
    ) -> PhaseStats:
        seed_base = self.args.rand_seed
        if runtime_sec > 0:
            deadline = time.time() + runtime_sec

            def worker(session: StoreSession, lane_id: int) -> Callable[[PhaseStats], None]:
                rng = random.Random(seed_base + lane_id)

                def run(stats: PhaseStats) -> None:
                    while time.time() < deadline:
                        object_ids = self.dataset.next_read_ids(
                            self.args.batch_size,
                            loop=True,
                            sequential=sequential,
                            rng=rng,
                            source="prepared",
                        )
                        if not object_ids:
                            stats.dataset_exhausted = True
                            break
                        start = time.perf_counter()
                        result = session.get_ids(object_ids, verify)
                        latency = time.perf_counter() - start
                        self._record(stats, latency, result, len(object_ids))

                return run

            return self._run_threads(phase_name, worker)

        def worker(session: StoreSession, lane_id: int) -> Callable[[PhaseStats], None]:
            rng = random.Random(seed_base + lane_id)

            def run(stats: PhaseStats) -> None:
                while True:
                    object_ids = self.dataset.next_read_ids(
                        self.args.batch_size,
                        loop=loop,
                        sequential=sequential,
                        rng=rng,
                        source="prepared",
                    )
                    if not object_ids:
                        break
                    start = time.perf_counter()
                    result = session.get_ids(object_ids, verify)
                    latency = time.perf_counter() - start
                    self._record(stats, latency, result, len(object_ids))

            return run

        return self._run_threads(phase_name, worker)

    def _run_mixed_phase(self, phase_name: str, extra_write_budget: int) -> PhaseStats:
        deadline = time.time() + self.args.runtime
        write_upper = self.dataset.next_write_id + extra_write_budget
        stop_event = threading.Event()
        seed_base = self.args.rand_seed

        def worker(session: StoreSession, lane_id: int) -> Callable[[PhaseStats], None]:
            rng = random.Random(seed_base + lane_id)

            def run(stats: PhaseStats) -> None:
                while time.time() < deadline and not stop_event.is_set():
                    do_read = rng.randrange(100) < self.args.rwmixread
                    if do_read:
                        object_ids = self.dataset.next_read_ids(
                            self.args.batch_size,
                            loop=True,
                            sequential=False,
                            rng=rng,
                            source="prepared",
                        )
                        if not object_ids:
                            continue
                        start = time.perf_counter()
                        result = session.get_ids(object_ids, verify=self.args.verify)
                        latency = time.perf_counter() - start
                        self._record(stats, latency, result, len(object_ids))
                        continue

                    object_ids = self.dataset.reserve_write_ids(self.args.batch_size, write_upper)
                    if not object_ids:
                        stats.dataset_exhausted = True
                        stop_event.set()
                        break
                    start = time.perf_counter()
                    result = session.put_ids(object_ids)
                    latency = time.perf_counter() - start
                    self._record(stats, latency, result, len(object_ids))
                    if result.successful_object_ids:
                        self.dataset.mark_runtime_written(result.successful_object_ids)

            return run

        return self._run_threads(phase_name, worker)

    def _maybe_prepare_dataset(self) -> Optional[PhaseStats]:
        if self.args.prepare_mode == "none":
            return None
        if self.args.prepare_mode == "write" or self.args.scenario in {
            "read_perf",
            "mixed_rw",
        }:
            stats = self._run_fixed_write(
                "prepare_write",
                self._prepare_budget(),
                strict=True,
                write_scope="prepared",
            )
            self._phase_gap("main_run")
            return stats
        return None

    def run(self) -> List[PhaseStats]:
        LOG.info(
            "scenario=%s io_api=%s memory_kind=%s buffer_layout=%s "
            "segment_sizes=%s buffer_offset=%d segment_gap=%d block_size=%d numjobs=%d "
            "iodepth=%d lanes=%d batch_size=%d value_size=%d nr_objects=%d "
            "prepare_objects=%d write_objects=%d memory_replica_num=%d "
            "nof_replica_num=%d preferred_segment=%s verify=%s",
            self.args.scenario,
            self.args.io_api,
            self.args.memory_kind,
            self.args.buffer_layout,
            self.args.resolved_segment_sizes,
            self.args.buffer_offset,
            self.args.segment_gap,
            self.args.block_size,
            self.args.numjobs,
            self.args.iodepth,
            self.lane_count,
            self.args.batch_size,
            self.args.value_size,
            self.args.nr_objects,
            self._prepare_budget(),
            self.args.write_objects,
            self.args.memory_replica_num,
            self.args.nof_replica_num,
            self.args.preferred_segment or "<none>",
            self.args.verify,
        )

        phases: List[PhaseStats] = []
        if self.args.scenario == "verify_write":
            phases.append(
                self._run_fixed_write(
                    "write_verify",
                    self._write_budget(),
                    strict=True,
                    write_scope="prepared",
                )
            )
            self._phase_gap("verify_read")
            phases.append(self._run_read_phase("verify_read", verify=True, sequential=True, loop=False))
            return phases

        if self.args.scenario == "fill":
            phases.append(self._run_fixed_write("fill_write", self._write_budget(), strict=False))
            return phases

        if self.args.scenario == "write_perf":
            total_objects = self._write_budget()
            if self.args.runtime > 0:
                phases.append(self._run_time_based_write("write_perf", total_objects))
            else:
                phases.append(self._run_fixed_write("write_perf", total_objects, strict=False))
            return phases

        if self.args.scenario == "read_perf":
            prepared = self._maybe_prepare_dataset()
            if prepared is not None:
                phases.append(prepared)
            phases.append(
                self._run_read_phase(
                    "read_perf",
                    verify=self.args.verify,
                    sequential=True,
                    loop=(self.args.runtime > 0),
                    runtime_sec=self.args.runtime,
                )
            )
            return phases

        prepared = self._maybe_prepare_dataset()
        if prepared is not None:
            phases.append(prepared)
        phases.append(self._run_mixed_phase("mixed_rw", self._write_budget()))
        return phases


def log_overall_summary(phases: List[PhaseStats]) -> None:
    overall = merge_stats("overall", phases)
    LOG.info("=== overall summary ===")
    log_phase_stats(overall)


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    setup_logging(args.log_level)
    runner: Optional[BenchmarkRunner] = None
    try:
        runner = BenchmarkRunner(args)
        phases = runner.run()
        log_overall_summary(phases)
        if any(phase.verify_failures > 0 for phase in phases):
            return 20
        if args.verify and any(phase.misses > 0 for phase in phases if "read" in phase.name):
            return 21
        return 0
    except KeyboardInterrupt:
        LOG.warning("benchmark interrupted")
        return 130
    except Exception as exc:  # pragma: no cover - CLI entry path
        LOG.exception("benchmark failed: %s", exc)
        return 1
    finally:
        if runner is not None:
            runner.close()


if __name__ == "__main__":
    raise SystemExit(main())
