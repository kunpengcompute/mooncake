# SSD Storage

Mooncake Store supports both node-local SSD offload and shared NVMe-over-Fabrics
storage pools. Choose the guide that matches the storage tier in your deployment.

| Storage option | Description |
|----------------|-------------|
| [SSD Offload](ssd-offload) | Configure local SSD offload, eviction, and I/O behavior. |
| [NVMe KV Local-Disk Backend](nvme-kv) | Configure a local NVMe KV namespace for SSD offload. |
| [NVMe-oF SSD Pool](nvmf-ssd-deployment-guide) | Configure a shared NVMe-over-Fabrics storage tier. |
| [Enable the Mooncake Staging Buffer on Ascend A3](ascend-nof-host-staging) | Use vLLM to write KV blocks to an NVMe-oF SSD through a host staging buffer and read them back into NPU memory. |

:::{toctree}
:maxdepth: 1
:hidden:

ssd-offload
nvme-kv
nvmf-ssd-deployment-guide
ascend-nof-host-staging
:::

## Change History

| Version | Date | Description |
| ---- | ---- | ---- |
| 01 | 2026-12-30 | This is the first official release. |
