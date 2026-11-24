# Enable the Mooncake Staging Buffer on Ascend A3

Mooncake stages KV cache blocks in host memory when vLLM transfers them between Ascend NPU memory and an NVMe over Fabrics (NoF) SSD. Send a prompt to write the blocks to the SSD, then repeat the prompt to read them back into NPU memory.

## Prerequisites

- **Inference node.** Use one Atlas 800 A3 with 16 NPUs (64 GB each) visible to vLLM. Configure CANN, ADXL, HCCL and RDMA, install the Mooncake build dependencies, and make the quantized `GLM-5.1-w8a8` weights available locally. This guide uses vLLM v0.23.0 and vLLM-Ascend v0.23.0. [GLM-5.1 deployment requirements](https://github.com/vllm-project/vllm-ascend/blob/v0.23.0/docs/source/tutorials/models/GLM5.md)
- **Storage node.** Use an RDMA-enabled SPDK build that includes `nvmf_tgt` and `scripts/rpc.py`. Reserve a dedicated NVMe namespace whose contents can be overwritten. Its controller must be bound to an SPDK-compatible driver. Mooncake writes to the namespace starting at offset zero.
- **Both nodes.** Configure hugepages and RDMA device permissions. The inference node needs enough free hugepage memory for the staging pool and SPDK. It also needs the SPDK and DPDK development headers and libraries, plus an ISA-L static library built with position-independent code. The examples use SPDK v23.01.1.
- **Network and shells.** Use an isolated test network. The inference node must be able to reach the storage node's RDMA IPv4 address on port `4420` and use key-based SSH to run SPDK RPC commands there. Keep one Bash shell open on each node. Both need `jq`. The inference node also needs `curl`, and the storage node needs `watch`. Activate the vLLM Python environment and load the CANN environment in the inference shell.

## Build Mooncake on the inference node

Enter the URL of a Mooncake repository that supports NoF host staging. Clone it with its submodules.

```bash
read -r -p 'Mooncake source repository URL: ' MOONCAKE_REPO_URL
git clone --recurse-submodules -- "$MOONCAKE_REPO_URL" mooncake
cd mooncake
```

### Use an existing SPDK build

Enter the absolute path to the root of the compiled SPDK source tree on the inference node. The next five assignments use the SPDK v23.01.1 directory layout. If you prepared the dependencies with Mooncake's `dependencies.sh --with-spdk`, use `extern/spdk` in that Mooncake checkout. [SPDK's DPDK build configuration](https://github.com/spdk/spdk/blob/v23.01.1/dpdkbuild/Makefile)

```bash
read -r -p 'Absolute path to the compiled SPDK source tree: ' SPDK_ROOT
SPDK_INC="$SPDK_ROOT/include"
SPDK_LIB="$SPDK_ROOT/build/lib"
DPDK_INC="$SPDK_ROOT/dpdk/build/include"
DPDK_LIB="$SPDK_ROOT/dpdk/build/lib"
ISAL_LIB="$SPDK_ROOT/isa-l/.libs"
```

| Variable | Files to look for | Path relative to SPDK_ROOT |
| --- | --- | --- |
| SPDK_INC | spdk/env.h | include |
| SPDK_LIB | libspdk_nvme.a | build/lib |
| DPDK_INC | rte_config.h and the generated rte_build_config.h from the DPDK build used by SPDK | dpdk/build/include |
| DPDK_LIB | librte_eal.a from the same DPDK build | dpdk/build/lib |
| ISAL_LIB | libisal.a built with -fPIC | isa-l/.libs |

If your directories match the table, continue to **Build and install the Mooncake package**.

### Use system development packages or a custom installation

If all dependencies come from development packages, start here. Locate the headers and static libraries with `find`. Add any custom installation prefixes to the search. If a dependency is installed only as a shared library (`.so`), obtain the matching static archive from a development package or an existing build.

```bash
find /usr /opt "$PWD" \( -type f -o -type l \) \
  \( -path '*/spdk/env.h' -o -name libspdk_nvme.a \
     -o -name rte_config.h -o -name rte_build_config.h \
     -o -name librte_eal.a -o -name libisal.a \) -print
```

Replace the example paths below with the directories you found. Set `SPDK_INC` to the directory containing the `spdk/` subdirectory with `env.h`. For the other variables, use the directory containing the corresponding files in the table. For a mixed installation, set the source-tree paths first, then override only the variables for dependencies installed elsewhere.

```bash
SPDK_INC="/usr/local/include"
SPDK_LIB="/usr/local/lib"
DPDK_INC="/usr/include/dpdk"
DPDK_LIB="/usr/lib64"
ISAL_LIB="/usr/lib64"
```

### Build and install the Mooncake package

Build the Mooncake master and Python bindings in the same shell where you set the paths. `USE_ASCEND_DIRECT` enables Ascend device access. `USE_NOF` builds the SPDK initiator and the NoF registration API.

```bash
python -m pip install '.[administration]' \
  -Ccmake.define.USE_ASCEND_DIRECT=ON -Ccmake.define.USE_NOF=ON \
  -Ccmake.define.SPDK_INCLUDE_DIR="$SPDK_INC" -Ccmake.define.SPDK_LIB_DIR="$SPDK_LIB" \
  -Ccmake.define.DPDK_INCLUDE_DIR="$DPDK_INC" -Ccmake.define.DPDK_LIB_DIR="$DPDK_LIB" \
  -Ccmake.define.SPDK_ISA_L_LIB_DIR="$ISAL_LIB"
```

Put the newly installed package and its shared libraries first in the search paths while retaining the CANN paths. The last command imports the NoF registration API and prints the path of the Mooncake package that Python loaded.

```bash
SITE=$(python -c 'import sysconfig; print(sysconfig.get_path("platlib"))')
export PYTHONPATH="$SITE${PYTHONPATH:+:$PYTHONPATH}"
export LD_LIBRARY_PATH="$SITE/mooncake${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
python -c 'from mooncake.store import MooncakeDistributedNoFRegister; import mooncake; print(mooncake.__file__)'
```

## Start the Mooncake master on the inference node

Start a fresh master with local endpoints for RPC (`50051`), metrics (`9003`) and HTTP metadata (`8080`). Continue when the `/health` check succeeds.

```bash
RUN_DIR=$(mktemp -d)
python -m mooncake.cli \
  --rpc_address=127.0.0.1 --rpc_port=50051 \
  --metrics_host=127.0.0.1 --metrics_port=9003 \
  --enable_http_metadata_server=true \
  --http_metadata_server_host=127.0.0.1 --http_metadata_server_port=8080 \
  > "$RUN_DIR/master.log" 2>&1 &
curl -fsS --retry 30 --retry-connrefused --retry-delay 1 http://127.0.0.1:9003/health
```

## Start the SPDK target and export a namespace on the storage node

Start an empty SPDK target and wait for it to initialize. Make sure the default RPC socket `/var/tmp/spdk.sock` is available and choose a core mask from the CPUs the target process is allowed to use.

```bash
read -r -p 'Absolute SPDK path on the storage node (contains build/bin/nvmf_tgt): ' SPDK_DIR
read -r -p 'Target CPU core mask (hex): ' CORE_MASK
read -r -p 'PCI address of the dedicated test SSD: ' SSD_BDF
read -r -p 'IPv4 address of the storage RDMA interface: ' TARGET_RDMA_IP
cd "$SPDK_DIR"
TARGET_LOG=$(mktemp -t nvmf-target.XXXXXX.log)
./build/bin/nvmf_tgt -m "$CORE_MASK" > "$TARGET_LOG" 2>&1 &
./scripts/rpc.py -r 100 framework_wait_init
```

Create the RDMA transport and attach the NVMe controller at the specified PCI address. The attach command prints the block device (bdev) names. Enter the name of the test bdev, such as `Nvme0n1`.

```bash
./scripts/rpc.py nvmf_create_transport -t RDMA
./scripts/rpc.py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a "$SSD_BDF"
read -r -p 'Test bdev name from the attach command: ' BDEV
```

Create an NVMe subsystem, add the selected bdev as a namespace, and add an RDMA listener. The `-a` flag on `nvmf_create_subsystem` allows any initiator on the test network to connect. Check that the last command lists the expected bdev, RDMA address and port `4420`.

```bash
NQN=nqn.2016-06.io.spdk:mooncake-staging
./scripts/rpc.py nvmf_create_subsystem "$NQN" -a -s MCSTAGING01
./scripts/rpc.py nvmf_subsystem_add_ns "$NQN" "$BDEV"
./scripts/rpc.py nvmf_subsystem_add_listener "$NQN" -t RDMA -f IPv4 -a "$TARGET_RDMA_IP" -s 4420
./scripts/rpc.py nvmf_get_subsystems "$NQN"
```

## Register the NoF SSD with the master from the inference node

Enter the SSH login details and the absolute SPDK path on the **storage node**. The registration command queries SPDK over SSH and registers every NVMe namespace exported by that target, so export only the test namespace. Check that registration succeeded for one namespace and that `master_total_nof_capacity_bytes` is greater than zero.

```bash
read -r -p 'Storage node SSH address: ' TARGET_SSH_HOST
read -r -p 'SSH user with access to the SPDK RPC socket: ' TARGET_SSH_USER
read -r -p 'Absolute SPDK path on the storage node (no spaces): ' TARGET_SPDK_DIR
MC_NOF_TRTYPE=RDMA python -m mooncake.mooncake_ssd_register \
  --master_server_address=127.0.0.1:50051 \
  --spdk_target_info="ip:$TARGET_SSH_HOST path:$TARGET_SPDK_DIR" \
  --username "$TARGET_SSH_USER"
curl -fsS http://127.0.0.1:9003/metrics | grep '^master_total_nof_capacity_bytes '
```

## Enable host staging and start vLLM on the inference node

Set `MC_STORE_NOF_HOST_STAGING=1` and allocate a 1 GiB local buffer per worker (16 GiB across 16 workers). On the first device I/O, Mooncake can allocate an additional DMA-capable host staging pool of the same size for each worker. Configure zero memory replicas and one NoF replica, and disable same-node memory placement so that subsequent external cache reads use the SSD.

```bash
unset ASCEND_ENABLE_USE_FABRIC_MEM MOONCAKE_GLOBAL_SEGMENT_SIZE MOONCAKE_MASTER
export MC_STORE_NOF_HOST_STAGING=1
export MC_STORE_REPLICA_NUM=0 MC_STORE_NOF_REPLICA_NUM=1
export MOONCAKE_CONFIG_PATH="$RUN_DIR/mooncake.json"
cat > "$MOONCAKE_CONFIG_PATH" <<'JSON'
{
  "protocol": "ascend",
  "metadata_server": "http://127.0.0.1:8080/metadata",
  "master_server_address": "127.0.0.1:50051",
  "global_segment_size": 0,
  "local_buffer_size": 1073741824,
  "prefer_alloc_in_same_node": false
}
JSON
```

Enter the local path to the `GLM-5.1-w8a8` weights. vLLM uses 16-way tensor parallelism and expert parallelism, with expert weight filtering to speed up loading. `AscendStoreConnector` reads and writes KV blocks in Mooncake. Disable local prefix caching so the repeated request reads from the external store. Wait until `/v1/models` lists `GLM-5.1-w8a8` before sending a request.

```bash
read -r -p 'Local GLM-5.1-w8a8 weights directory: ' MODEL_DIR
python -m vllm.entrypoints.openai.api_server \
  --model "$MODEL_DIR" --served-model-name GLM-5.1-w8a8 --host 127.0.0.1 --port 8000 \
  --tensor-parallel-size 16 --enable-expert-parallel --enable-ep-weight-filter \
  --quantization ascend --trust-remote-code --gpu-memory-utilization 0.95 \
  --max-num-seqs 8 --max-model-len 2048 --block-size 128 \
  --no-enable-prefix-caching \
  --compilation-config '{"cudagraph_mode":"FULL_DECODE_ONLY"}' \
  --kv-transfer-config '{"kv_connector":"AscendStoreConnector","kv_role":"kv_both","kv_connector_extra_config":{"backend":"mooncake"}}' \
  > "$RUN_DIR/vllm.log" 2>&1 &
curl -fsS --retry 1800 --retry-connrefused --retry-delay 2 --retry-max-time 3600 http://127.0.0.1:8000/v1/models
```

## Verify that vLLM writes to and reads from the NoF SSD

**Storage node.** Record the initial counters for the dedicated bdev and leave `watch` running. The master reads one logical block per heartbeat, even when vLLM is idle. `read_bytes_above_baseline` subtracts one logical block per read operation from the total bytes read. It stays flat during heartbeat reads and rises when a KV read spans multiple blocks.

```bash
BLOCK_BYTES=$(./scripts/rpc.py bdev_get_bdevs -b "$BDEV" | jq -er '.[0].block_size')
export BDEV BLOCK_BYTES
watch -n 1 './scripts/rpc.py bdev_get_iostat -b "$BDEV" | jq ".bdevs[0] | {bytes_written, bytes_read, read_bytes_above_baseline: (.bytes_read - .num_read_ops * $BLOCK_BYTES)}"'
```

**Inference node, first request.** Send a 1,024-token prompt (eight complete KV blocks) and generate one token. Wait for `bytes_written` on the storage node to increase and settle before sending the second request. The increase shows the asynchronous write from NPU memory through the host staging buffer to the SSD.

```bash
jq -n '{model:"GLM-5.1-w8a8", prompt:[range(1024) | 42], max_tokens:1, temperature:0}' > "$RUN_DIR/request.json"
curl -fsS http://127.0.0.1:8000/v1/completions \
  -H 'Content-Type: application/json' --data-binary @"$RUN_DIR/request.json"
```

**Inference node, second request.** Send the same request again. Verify that it completes and that both `bytes_read` and `read_bytes_above_baseline` increase from their values immediately before the request. On this dedicated namespace, the increases confirm that vLLM read the KV blocks back from the NoF SSD through the host staging buffer into NPU memory.

```bash
curl -fsS http://127.0.0.1:8000/v1/completions \
  -H 'Content-Type: application/json' --data-binary @"$RUN_DIR/request.json"
```

## Change History

| Version | Date | Description |
| ---- | ---- | ---- |
| 01 | 2026-12-30 | This is the first official release. |
