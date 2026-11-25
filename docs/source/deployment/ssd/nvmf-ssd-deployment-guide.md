# Mooncake NVMe-oF SSD Pool Deployment Guide

## Overview

This guide shows how to attach an NVMe-oF SSD pool to Mooncake Store. The
deployment has two main phases:

- Start Mooncake services built with NoF support enabled.
- Create SPDK NVMe-oF targets on SSD pool nodes and register their namespaces
  with the Mooncake master.

After registration, the master reports the registered NVMe-oF namespaces as a
remote SSD pool in its metrics, and clients can place NoF replicas through
Mooncake Store.

## 1. Build Mooncake with NoF Support

Follow the "Build with NVMe-oF SSD Pool" section in the
[Build Guide](../../getting_started/build.md) to install SPDK dependencies and
build Mooncake with `-DUSE_NOF=ON`.

## 2. Deploy Mooncake Services

### 2.1 Node Topology

- **Mooncake service node**: 192.168.65.81. This node runs the master, metadata, and store services.
- **SSD pool nodes**: 192.168.65.56 and 192.168.65.57. These nodes provide SSD storage resources.

### 2.2 Deploy the Master Service

```bash
mooncake_master --rpc_address=192.168.65.81
```

### 2.3 Deploy the Metadata Service

```bash
python3 -m mooncake.http_metadata_server --host=192.168.65.81 --port=8080
```

If an aiohttp-related error occurs during startup, install aiohttp:

```bash
pip3 install aiohttp
```

### 2.4 Deploy the Store Service

#### Configure `store_service.json`

Create `store_service.json` under `/home`:

```json
{
  "local_hostname": "localhost",
  "metadata_server": "http://192.168.65.81:8080/metadata",
  "master_server_address": "192.168.65.81:50051",
  "protocol": "rdma",
  "device_name": "mlx5_0",
  "global_segment_size": "50gb",
  "local_buffer_size": 0
}
```

**Notes**:

- `device_name`: Run `ibv_devices` on node 192.168.65.81 to check the RDMA device name.

#### Start the Service

The store service initializes the SPDK environment lazily on its first NoF
operation (segment probe, buffer registration, or data I/O) rather than
during startup, so a successful startup alone does not prove the SPDK
environment is usable. Configure hugepages on the store service node,
192.168.65.81, before the first NoF traffic arrives:

```bash
echo 512 > /proc/sys/vm/nr_hugepages
```

Only a small number of hugepages is required for the SPDK environment itself.
In most cases, 512 hugepages are sufficient.

Start the store service:

```bash
python3 -m mooncake.mooncake_store_service --config=/home/store_service.json --port=8081
```

If a timeout error occurs during startup, check whether a proxy is configured on node 192.168.65.81. If a proxy is configured, unset the proxy configuration and try again.

## 3. Deploy the NVMe-oF SSD Pool

### 3.1 Prerequisites

1. Configure passwordless SSH login from the Mooncake node, 192.168.65.81, to the SSD pool nodes, 192.168.65.56 and 192.168.65.57.
   See [OpenSSH key-based authentication](https://help.ubuntu.com/community/SSH/OpenSSH/Keys).
2. Build SPDK on each SSD pool node in advance.
   See [SPDK build instructions](https://github.com/spdk/spdk/blob/master/README.md#build).

### 3.2 Install SSH Dependencies

```bash
python3 -m pip install "paramiko>=3.4.0"
```

### 3.3 Create and Register SSDs

#### Workflow

`mooncake_ssd_create_and_register` performs two steps in order:

1. Connect to each target node over SSH, start or reuse the SPDK `nvmf_tgt`, and create the transport, subsystem, bdevs, namespaces, and listener.
2. Discover active namespaces on the target and register them with the Mooncake master.

#### Basic Usage

```bash
python3 -m mooncake.mooncake_ssd_create_and_register \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk pci:0000:01:00.0,0000:02:00.0"
```

#### Multiple Targets

```bash
python3 -m mooncake.mooncake_ssd_create_and_register \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk pci:0000:01:00.0" \
  --spdk_target_info "ip:192.168.65.57 path:/home/spdk pci:0000:02:00.0"
```

#### Create Target Resources Only

This mode does not require a master server address.

```bash
python3 -m mooncake.mooncake_ssd_create_and_register \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk pci:0000:01:00.0" \
  --skip-register
```

#### Register Namespaces from an Existing Target

```bash
python3 -m mooncake.mooncake_ssd_create_and_register \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk" \
  --skip-create
```

#### Add SSDs to a Running Target

To add SSDs to a running SPDK target and register them with the Mooncake master, run the create/register tool again with the PCI addresses of the new drives:

```bash
python3 -m mooncake.mooncake_ssd_create_and_register \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk pci:0000:03:00.0"
```

The command reuses the running `nvmf_tgt`, transport, subsystem, and listener. It creates bdevs and adds namespaces for the new PCI devices, then registers the new namespaces with the master. Existing namespaces and listeners are detected and skipped to avoid duplicate creation.

#### Parameters

| Parameter | Required | Description |
| --- | --- | --- |
| `--master_server_address` | Conditional | Required unless `--skip-register` is set. For example, `192.168.65.81:50051`. |
| `--spdk_target_info` | Yes | Target description. Repeat for multiple targets. Format: `ip:<target_ip> path:<spdk_path> [pci:<pci1>,<pci2>]`. |
| `--skip-create` | No | Skip target creation and only register namespaces. |
| `--skip-register` | No | Create target resources without registering them with the master. |
| `--dry-run` | No | Print the workflow without executing remote operations. |
| `--core-mask` | No | CPU core mask used to start `nvmf_tgt`. Default: `0xff`. |
| `--transport-type` | No | NVMe-oF transport type. Default: `RDMA`. |
| `--max-queue-depth` | No | Maximum transport queue depth. Default: `128`. |
| `--max-io-qpairs-per-ctrlr` | No | Maximum number of I/O queue pairs per controller. Default: `127`. |
| `--max-io-size` | No | Maximum I/O size in bytes. Default: `4096`. |
| `--in-capsule-data-size` | No | Maximum in-capsule data size in bytes. Default: `131072`. |
| `--io-unit-size` | No | I/O unit size in bytes. Default: `131072`. |
| `--max-aq-depth` | No | Admin queue depth. Default: `128`. |
| `--num-shared-buffers` | No | Number of shared transport buffers. Default: `4096`. |
| `--buf-cache-size` | No | Buffer cache size per poll group. Default: `32`. |
| `--username` | No | SSH username. Default: `root`. |
| `--port` | No | SSH port. Default: `22`. |
| `--password` | No | SSH password. |
| `--key-file` | No | SSH private key file. |
| `-D`, `--define` | No | Override registration configuration fields, for example `-Dtrsvcid=4420`. |

## 4. Unregister and Optionally Remove SSDs

### Workflow

By default, `mooncake_ssd_unregister_and_remove` performs the following steps in order:

1. Unregister the namespace from the Mooncake master so the corresponding NoF segment no longer participates in allocation.
2. If `--remove-target-namespace` is specified, call the SPDK `nvmf_subsystem_remove_ns` RPC to remove the namespace from the target subsystem.
3. If `--detach-bdev` is also specified, call `bdev_nvme_detach_controller` to release the corresponding SPDK NVMe bdev controller.

Use `--target-only` to clean up target resources independently, without connecting to the master or unregistering namespaces from it. This mode removes namespaces automatically; `--remove-target-namespace` is not required.

### Unregister a Specific Namespace from the Master Only

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk ns:1 nqn:nqn.2016-06.io.spdk:cnode1"
```

### Unregister All Namespaces on a Target

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk"
```

### Unregister and Remove a Specific Namespace

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk ns:1 nqn:nqn.2016-06.io.spdk:cnode1" \
  --remove-target-namespace
```

### Unregister and Remove All Namespaces

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk" \
  --remove-target-namespace
```

### Unregister a Namespace, Remove It, and Detach Its Bdev Controller

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk ns:1 nqn:nqn.2016-06.io.spdk:cnode1" \
  --remove-target-namespace \
  --detach-bdev
```

### Unregister All Namespaces, Remove Them, and Detach Their Bdev Controllers

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk" \
  --remove-target-namespace \
  --detach-bdev
```

### Remove Failed Drives or Leftover Resources from the Target Independently

Use this mode when the master has already unmounted a failed drive through heartbeat detection, but its namespace and bdev remain on the target and must be cleaned up before replacing the drive. Confirm that client I/O for the affected resources has stopped and that the target can complete resource removal. Remove the namespace and finish detaching the controller before switching drivers or physically replacing the drive, so leftover resources do not interfere with later creation and registration.

**Risk of incorrect operation:** Running `setup.sh reset` before the target has finished removing the namespaces and detaching their controllers unbinds the devices and rebinds them to their original drivers, while the old target may still retain controllers, queues, and in-flight requests. A reset without a restricted device scope can affect multiple drives. Do not perform a global reset after cleaning up only some drives. A segment being unmounted from the master does not mean its target resources have been released.

The following failures may occur:

- `nvmf_get_subsystems` still lists the namespace, but the drive can no longer serve reads or writes. Master probes time out and the corresponding segment is unmounted.
- The local NVMe controller repeatedly reports Identify timeouts, `controller reinitialization failed`, or `Resetting controller failed`.
- Namespace removal waits for in-flight requests and the RPC times out. A retry returns `subsystem busy, retry later`. A successful detach command does not necessarily mean that all asynchronous resource cleanup has completed.
- A subsequent create/register operation may remain at `Setting up SPDK with PCI devices` for a long time. The old target may report `COMMAND ID CONFLICT` or `cpl does not map to outstanding cmd`, or terminate unexpectedly. If the creation tool then finds that the process is absent, it starts a new target and configures only the drives specified in that invocation. Other namespaces from the previous target are not restored automatically.

Recovery from these conditions is not guaranteed by `--target-only`.

Remove a specific namespace and detach its NVMe controller. The `ns` field is the namespace ID on the current target, not a PCI address:

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --target-only \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk ns:1 nqn:nqn.2016-06.io.spdk:cnode1" \
  --detach-bdev
```

Remove namespaces from all NVMe subsystems on the target and detach their NVMe controllers:

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --target-only \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk" \
  --detach-bdev
```

### Parameters

| Parameter | Required | Description |
| --- | --- | --- |
| `--master_server_address` | Conditional | Required for the default workflow; unused in `--target-only` mode. |
| `--target-only` | No | Skip the master and remove target namespaces independently. Combine with `--detach-bdev` to clean up the underlying controllers. |
| `--spdk_target_info` | Yes | Target/namespace description. Repeat for multiple targets. Format: `ip:<target_ip> path:<spdk_path> [ns:<nsid>] [nqn:<subsystem_nqn>]`. |
| `--remove-target-namespace` | No | Remove matching namespaces from the SPDK target subsystem after successful unregistration from the master. |
| `--detach-bdev` | No | Detach the corresponding SPDK NVMe bdev controller after namespace removal. Requires `--remove-target-namespace` or `--target-only`. |
| `--dry-run` | No | Print the workflow without executing remote operations. |
| `--username` | No | SSH username. Default: `root`. |
| `--port` | No | SSH port. Default: `22`. |
| `--password` | No | SSH password. |
| `--key-file` | No | SSH private key file. |
| `-D`, `--define` | No | Override unregistration configuration fields, for example `-Dtrsvcid=4420`. |

### 4.8 Inspect Drive Information on the Target

On an SSD pool node, change to the SPDK directory and run the following commands:

1. List subsystems, including their NQNs and namespace IDs:

   ```bash
   ./scripts/rpc.py nvmf_get_subsystems
   ```

2. Show drive details, including block sizes and PCI addresses:

   ```bash
   ./scripts/rpc.py bdev_get_bdevs
   ```

### 4.9 Usage Recommendations

- Use `mooncake_ssd_create_and_register` for routine expansion to create target resources and register them with the master in one command.
- Use `--skip-create` when the target already exists and only needs to be registered with the master again.
- For scale-down, the default workflow only unregisters namespaces from the master. Add `--remove-target-namespace` when the corresponding target resources should also be removed.
- `--detach-bdev` releases the underlying SPDK bdev controller and has a broader impact. Use it only when intentionally taking the drive offline.
- Use `--dry-run` to review the matching scope before performing high-risk operations.

## 5. Performance Tests

### 5.1 Use the Built-in Benchmark Tool

```bash
./build/mooncake-store/benchmarks/nof_worker_pool_bench \
    --endpoints='traddr:192.168.65.56 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1 trtype:RDMA adrfam:IPv4 ns:1, traddr:192.168.65.56 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1 trtype:RDMA adrfam:IPv4 ns:2' \
    --op=read \
    --io_size=1048576 \
    --iodepth=8 \
    --warmup_sec=3 \
    --duration_sec=30
```

#### Parameters

| Parameter | Description |
|-----------|-------------|
| `--endpoints` | Disk information used for the test. |
| `--op` | I/O operation type, either `read` or `write`. |
| `--io_size` | Block size in bytes. |
| `--iodepth` | Read/write queue depth. |
| `--warmup_sec` | Warmup duration in seconds. |
| `--duration_sec` | Test duration in seconds. |

### 5.2 Use NoF with vLLM + LMCache

For the general VLLM + LMCache + Mooncake deployment flow, see
[vLLM V1 Disaggregated Serving with Mooncake Store and LMCache](../integrations/lmcache/vllmv1-lmcache-integration.md).
After the NVMe-oF SSD pool is registered with Mooncake, add the NoF-specific
Mooncake configuration below.

#### NoF Environment Variables

```bash
export LMCACHE_CONFIG_FILE="/path/vllm-lmcache-mooncake-config.yaml"
export MC_NOF_WORKERS=4
export MC_NOF_SUBMIT_CHUNK_BYTES=$((1 << 17))  # 128KB
export MC_NOF_INFLIGHT_BYTES_LIMIT=$((1 << 25))  # 32MB
```

#### NoF LMCache Configuration

```yaml
chunk_size: 256
remote_url: "mooncakestore://192.168.65.81:50051/"
remote_serde: "naive"
local_cpu: True
max_local_cpu_size: 8

extra_config:
  local_hostname: "localhost"
  metadata_server: "http://192.168.65.81:8080/metadata"
  master_server_address: "192.168.65.81:50051"
  global_segment_size: 0
  local_buffer_size: 1073741824
  protocol: "rdma"
  device_name: "mlx5_0"
```

**Notes**:

- Writing KV cache objects to the registered NoF pool is controlled per put
  by `nof_replica_num` in the store client's `ReplicateConfig` (exposed in
  the Python binding), and the master must be built with `USE_NOF`. The
  LMCache Mooncake connector does not currently parse or forward such a
  setting, so enabling the NoF pool for LMCache writes needs a connector
  change or a direct store client.
- `global_segment_size: 0` means the inference process does not contribute a
  memory segment to the Mooncake cluster.
- Keep `local_buffer_size` non-zero because the client still needs local
  staging buffers for Mooncake transfers. With NoF enabled (and
  `MC_STORE_USE_HUGEPAGE` unset), this staging buffer is allocated from the
  SPDK hugepage pool and is therefore DMA-registered by construction.
- Buffers registered through the store client's `register_buffer` are also
  registered with the SPDK RDMA translation table, so NoF-over-RDMA I/O can
  address them directly. This requires SPDK 22.05 or newer on the client
  node (older versions build with a CMake warning and leave NoF-over-RDMA
  user-buffer I/O failing with "No translation" errors). Hugepage-backed,
  2MB-aligned buffers are expected; a non-aligned buffer is registered by
  its containing 2MB pages and logs a `RegisterMemory` warning. A buffer
  that cannot be registered at all (for example plain 4 KiB-page or device
  memory) keeps working for non-NoF transfers: the failure is logged as a
  warning instead of failing `register_buffer`.
- The parameters in `extra_config` should use the same Mooncake master,
  metadata server, protocol, and RDMA device as the store service.
- Each Mooncake process component (store client, embedded-master probe) keeps
  its own NVMe-oF connection per endpoint instead of sharing one
  process-wide connection. A process embedding both therefore opens two
  connections per target endpoint; plan target-side queue/connection limits
  accordingly.

| Environment Variable | Description | Default |
|----------------------|-------------|---------|
| `MC_NOF_WORKERS` | Number of worker threads used to process SPDK NoF I/O operations. | 4 |
| `MC_NOF_SUBMIT_CHUNK_BYTES` | Size of each I/O operation submitted to SPDK. | 128KB |
| `MC_NOF_INFLIGHT_BYTES_LIMIT` | Maximum number of in-flight I/O bytes allowed in the system. | 32MB |
| `MC_NOF_BACKEND` | NoF backend pair: `spdk` or `none`. `none` disables NoF at runtime in a `USE_NOF` build — one binary can be deployed on nodes without a working SPDK/hugepage environment. Case-insensitive; unrecognized values fall back to `spdk` with a warning. | `spdk` |
| `MC_NOF_IOVA_MODE` | SPDK IOVA mode passed to `spdk_env_init`: `pa` or `va`. Set `va` inside containers without physical-address access (restricted `/proc/self/pagemap`), where the default PA mode fails with `Cannot use IOVA as 'PA'`. | unset (SPDK default, PA-preferred) |

The `MC_NOF_WORKERS`, `MC_NOF_SUBMIT_CHUNK_BYTES` and
`MC_NOF_INFLIGHT_BYTES_LIMIT` parameters together provide QoS control for SPDK
NoF I/O.
