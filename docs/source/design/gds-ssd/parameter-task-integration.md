---
orphan: true
---

# Ascend 参数面与 Host 中转接入

## 合入范围

`parameter-task-new` 基于 `dev-main` 的 `9be26fe3`，迁移 `parameter-task` 的参数面直通和数据面中转功能。迁移保留新版 NoF 接口、内存页登记、队列调度及完成处理；不恢复已经删除的 `SpdkWrapper`。

| 模式 | 内存池写入 | NoF 池写入 |
| --- | --- | --- |
| `parameter_direct` | NPU 显存 → 参数面 UB → 1825 网卡 → RDMA 内存池 | NPU 显存 → 参数面 UB → 1825 网卡 → NVMe-oF SSD 池 |
| `data_staging` | NPU 显存 → Client local_buffer → 数据面网卡 → RDMA 内存池 | NPU 显存 → Client local_buffer → SPDK → NVMe-oF SSD 池 |

读取沿相反方向进行。非直通读取必须等网络读完成，再把 local_buffer 中的数据复制回显存。

P/D 传输仍使用自己的 Ascend 引擎。Store 使用独立 RDMA 引擎，不把 P/D 引擎传入 Store 的 `setup_real()`。
本次只修改 Mooncake。配套 vLLM-Ascend 继续使用已有 Store 独立引擎适配（如 `parameter-plane-v0.30.0rc1`），启动前确认已安装新分支生成的 wheel。

## 与 dev-main 的结合方式

- UB 注册由 `AscendParameterPlane` 管理。显存先注册给 UB；Transfer Engine 和 SPDK 再分别按自己的 PD 获取 MR。
- MR 缓存按 PD、注册范围和访问权限区分。同一范围的小块 I/O 复用 MR；超过网卡 MR 限制的区域按网卡允许的大小分段注册。
- 非直通复用 `StageWriteSlices()`、`ClientBufferAllocator` 和现有 Ascend 设备拷贝实现，没有另外创建中转池。
- local_buffer 在初始化时注册给 RDMA。启用 NoF 时，现有分配流程提供 SPDK DMA 内存；HugeTLB 分配路径继续沿用原有处理。
- 同一进程内可直接拷贝的内存池目标，保留原来的直接拷贝优化。其他 worker 的虚拟地址不能直接当作本进程地址使用。
- NoF 调用链为 `TransferSubmitter → NofWorkerPool → NVMeoFInitiator → SpdkInitiator → SPDK`。新增 `SubmitIOV()` 提交多个 slice；SPDK 的结构体与回调保留在 `spdk_initiator.cpp` 内部。
- Host buffer 继续使用 `RegisterMemory()` 和新版按页登记表；NPU 显存使用 `RegisterDeviceMemory()`。两者不能混用。非直通仅登记显存地址范围，登记给网卡和 NoF 的是 Host 中转 buffer。
- `SpdkInitiator` 使用 SPDK 的 `readv_ext/writev_ext`。请求的扩展选项选择显存转换回调；SPDK 构建请求时提供自己的 PD，回调返回对应的地址、长度、lkey 和 rkey，不复制数据。
- 不同 initiator 若使用同一个 SPDK PD，注册信息保留到最后一个相关 segment 退出，避免一个 initiator 关闭时影响另一个。
- NoF initiator 源地址取当前 worker 的 `rdma_ip`。连接缓存包含源地址，并检查实际 PD 所属网卡是否与配置一致。
- 保留 `dev-main` 的大内存分段注册、注销重试、共享内存登记、DFS、固定策略、`host_id`、session 和网卡亲和性字段。`replica_counts_explicit` 只供 Client 使用，不进入 Master RPC 序列化。
- 新版按请求指定的网卡偏好，不改变当前 worker 固定的参数面出口。远端内存池的副本放置偏好仍按新版逻辑处理。

## 编译选项

| 需要的能力 | 选项 |
| --- | --- |
| 参数面访问 RDMA 内存池 | `USE_ASCEND_NDR=ON` |
| 参数面访问 NoF 池 | 再启用 `USE_NOF=ON`、`USE_SPDK_NPU_UB=ON` |
| 数据面 Host 中转 | `USE_ASCEND_STAGING=ON` |
| 数据面 Host 中转访问 NoF 池 | 再启用 `USE_NOF=ON` |
| 同时使用 Ascend P/D | 按原有 P/D 部署要求启用 `USE_ASCEND_DIRECT` 等选项 |

需要匹配 A5 驱动和 CANN。参数面依赖 `ascend_hal` 中的 UB 注册接口。非默认驱动安装位置可通过 `ASCEND_DRIVER_PATH` 或 CMake 的 `ASCEND_HAL_LIBRARY` 指定。
本次没有修改 SPDK 源码；使用 SPDK 原有地址转换回调和扩展 NVMe I/O 接口。
远端普通 RDMA 内存池不需要启用 Ascend 编译选项。

## 配置

通过 `MOONCAKE_CONFIG_PATH` 指向 worker 共用的配置文件。下面仅列出一个物理 NPU；8 卡机器应在 `devices` 中补齐实际使用的物理设备编号。

### 参数面直通

```json
{
  "protocol": "rdma",
  "ascend_transfer": {"mode": "parameter_direct"},
  "parameter_plane": {
    "schema_version": 1,
    "network": "rdma",
    "memory_registration": "ub",
    "strict_route": true,
    "devices": {
      "0": {
        "eid": "0000:0000:0000:0000:0000:0000:0000:0001",
        "rdma_device": "hrn5_0",
        "rdma_ip": "192.0.2.10"
      }
    }
  }
}
```

原来只有 `parameter_plane`、没有 `ascend_transfer` 的配置继续按直通模式解析。
`eid` 是部署时提供的 UB 标识，代码校验其格式；实际 NPU、UB 与 RNIC 的物理连接仍需由机器配置保证。

### 数据面中转

```json
{
  "local_hostname": "198.51.100.10",
  "metadata_server": "P2PHANDSHAKE",
  "master_server_address": "198.51.100.20:50051",
  "protocol": "rdma",
  "global_segment_size": "0",
  "local_buffer_size": "1GB",
  "replica_num": 1,
  "nof_replica_num": 0,
  "ascend_transfer": {"mode": "data_staging"},
  "data_plane": {
    "schema_version": 1,
    "network": "rdma",
    "strict_route": true,
    "devices": {
      "0": {"rdma_device": "data_0", "rdma_ip": "192.0.2.10"}
    }
  }
}
```

写入 NoF 池时设置 `replica_num=0`、`nof_replica_num=1`，并按已有流程部署和注册 SSD 池。
`local_buffer_size` 是临时中转容量；`global_segment_size` 是该 Client 贡献给内存池的容量，两者用途不同。

- 每个 worker 根据当前 ACL 逻辑设备获取物理设备编号，再选取对应的网卡配置，不直接把 rank 当作物理编号。
- `rdma_device` 和 `rdma_ip` 用于数据传输。代码从网卡状态与 GID 表中确定 port/GID；匹配不唯一时需要补充 `rdma_port` 或 `gid_index`。
- `local_hostname` 沿用原有元数据、建链和端点命名规则，与 RDMA 源 IP 分开配置。放置策略的 `host_id` 也保持独立。
- 两个 NPU 共用一张 1825 时，可以分别配置该网卡不同物理口对应的 RDMA device/IP。
- 只查询对象的 Scheduler Client 使用已有 `rpc_only` 路径，容量与 local_buffer 都为 0，不解析 NPU 数据路由、不初始化 SPDK 环境。
- 同一进程使用固定 NPU 路由；不支持在进程运行中切换配置文件或 NPU→网口映射。

## 内存和完成时机

应用仍调用 Store 的 `register_buffer()`。直通模式会进行 UB/RDMA 注册；中转模式只记录显存地址范围，不把显存注册给数据面 RNIC。

中转读写持有注册记录的读锁，注销和关闭等待正在进行的操作退出。中转 buffer 在网络传输结束后归还；读操作还要完成显存回填。NoF 中转 buffer 使用 SPDK DMA allocator，或登记过的 HugeTLB 内存。登记或清理失败时返回错误；仍可能被设备访问的 buffer 不归还分配器。

Transfer Engine 提交失败或等待超时后，必须等已提交的操作结束，才能向上层报告结束并允许复用 buffer。
NDS 模式还会等待新版 batch 的完成回调退出，再返回结果或释放请求。`freeBatchID()` 只调用一次，不查询已经移交回收的 batch 句柄。普通模式保留新版默认行为。
网络始终不能结束操作时，安全等待可能继续超过原有超时；本次没有增加硬件级取消接口。

## 当前约束与验证

- 该功能接入 C++ Store；`MOONCAKE_STORE_BACKEND` 保持默认 `cpp`，不适用于新版 Rust Store（`rs`）。
- Ascend 新模式使用独立 classic Transfer Engine，不使用 TENT、TEV1 或 Dummy/Real IPC 代理模式。
- 1825 驱动、Verbs provider 和 UB 链路必须配套；如驱动要求，按其说明设置 `IBV_EXTEND_DRIVERS`。不同物理网口之间的 RoCE 通信仍需实际网络连通，不能按本机 TCP 回环推断可以免接线。
- 非直通面向完整对象读写；未接入 session、分片范围写入或文件 offload 的显存 DMA。P/D 与 Store 同用时，存储 connector 应关闭 layerwise。
- 中转空间不足时对应对象返回分配失败，不淘汰 KV 对象来腾中转空间。批量请求中的中转 buffer 会保留到该批次写入结束。
- 参数面 NoF 单次请求暂不混合 Host 与显存 slice；中转模式会先把它们复制到同一块 Host buffer。
- NoF 继续要求对象总长度与 SSD 偏移按设备块大小对齐；本次未引入旧分支的对象补齐及 Master 存储格式改动。
- CPU/TCP 模拟测试覆盖中转读写逻辑，不代表 A5 参数面、RDMA 网卡或远端 SSD 已完成实机验证。

测试入口（CPU 环境可验证配置、注册管理与中转流程）：

```bash
cmake --build <build> --target ascend_storage_config_test transport_uint_test \
  ascend_parameter_plane_mock_test parameter_plane_support_test \
  ascend_store_staging_test real_client_direct_read_test nvmeof_initiator_test \
  nof_page_registry_test nof_probe_context_pool_test client_buffer_test \
  runtime_accelerator_test rdma_unregister_retry_test rdma_gid_probe_test \
  rdma_transport_submit_task_test
ctest --test-dir <build> --output-on-failure \
  -R '^(ascend_storage_config_test|ascend_parameter_plane_mock_test|parameter_plane_support_test|ascend_store_staging_test|real_client_direct_read_test|nvmeof_initiator_test|nof_page_registry_test|nof_probe_context_pool_test|client_buffer_test|runtime_accelerator_test|rdma_unregister_retry_test|rdma_gid_probe_test|rdma_transport_submit_task_test)$'
```

`transport_uint_test` 中的 `TransferEngineAscendTransportTest.*` 验证独立引擎；部分其他测试需要 RDMA 设备。`ascend_parameter_plane_mock_test` 只在未启用真实 `USE_ASCEND_NDR` 的 Linux 测试构建中提供：它编译实际注册管理器，使用模拟 CANN 声明和 Verbs 调用，不访问硬件。

### 本地验证记录（2026-10-24）

- CPU 构建下，Transfer Engine、C++ Store 和 Store Python 扩展编译通过；新扩展的导入、能力查询及显式副本标记检查通过。
- 最后一轮 13 个相关 CTest 测试程序全部通过；另外运行的 7 个原有 NoF 清理测试、5 个独立引擎测试全部通过。SPDK CMake 依赖检查的 7 个测试及原有依赖安装检查的 8 个测试通过。
- 配置及 MR 范围、UB 引用复用、不同 PD 的 MR 缓存、并发首次创建、注销失败重试测试通过。
- 使用模拟 Ascend 设备和真实 TCP 传输，验证多 slice 写入、读回、重复读取、拷贝失败、重复登记、注销与写入并发，以及 NoF 显存登记失败后的回滚和重试。
- 验证非直通不会被新版直接读取优化绕过；结果返回前会等待完成回调释放 batch 引用。
- `spdk_initiator.cpp` 使用本地 SPDK 头文件，在普通、中转、参数面 NoF 三种编译条件下通过语法检查。其他 Ascend 条件编译路径使用模拟 CANN 声明检查；这不能代替真实 SDK 编译。
- 本机没有 A5、CANN 或可用 RNIC；本地 SPDK 构建未开启 RDMA，未修改该 SPDK 仓库。真实 CANN/驱动构建、NoF/RDMA 完整链接、1825 参数面读写、远端 SSD 读写及异常重连仍需在目标环境验证。
- 文档 `make html` 构建通过，并检查了生成页面中的配置示例、接口名和验证说明。
