# Mooncake NoF GDR 完整对象非对齐 I/O 融合设计

## 1. 文档状态

- 状态：Implemented（待真实 SPDK target/CUDA 环境完成集成与性能验收）
- Mooncake 基线分支：`spdk-gdr-final`
- Mooncake 基线提交：`37322980`（`store: add SPDK GDR support for NoF SSD transfers`）
- 当前两个分支的 Git merge-base：`a1e97616`
- 参考实现：`Non-Aligned`，仅用于提取完整对象非对齐需求与测试场景，不直接合并其提交
- SPDK 基线：固定使用当前 `sdpk-gdr-final` 分支的 SPDK NVMf RDMA target
- SPDK 修改：预计不需要

实现提交（均基于 `spdk-gdr-final`）：

```text
3bcd8aea  store: select SPDK NoF memory domain per task
b3ec778d  store: track logical size for NoF allocations
5016ed88  store: pad host NoF I/O with DMA scratch
ec92c60d  store: pad GPU NoF I/O with registered scratch
```

干净 GDR commit `37322980` 直接位于 `a1e97616` 之上，而 Non-Aligned 的功能提交
序列 `37a82e5e..3e933754` 位于更新的 `f54e8934` 之上。两者不能按整个分支直接
merge；实现时应先同步正式 PR base，再从 Non-Aligned 提交中手工提取必要行为。

## 2. 使用范围确认

本设计只面向以下两条推理链路。

### 2.1 vLLM -> LMCache -> Mooncake

LMCache Mooncake connector 使用：

```python
self.store.batch_get_into(key_strs, buffer_ptrs, buffer_sizes)
```

它按 LMCache chunk key 读取完整 Mooncake 对象，没有调用 `get_into_ranges()`，也没有
向 Mooncake Store 传递对象内部的 `src_offset`、`dst_offset` 或 fragment。

### 2.2 vLLM -> MooncakeStoreConnector -> Mooncake

vLLM MooncakeStoreConnector 使用：

```python
self.store.batch_put_from_multi_buffers(keys, addrs, sizes)
self.store.batch_get_into_multi_buffers(keys, addrs, sizes)
```

一个 key 的完整对象可以由多个不连续 GPU buffer 组成。Mooncake 按顺序把这些 buffer
转换为多个 `Slice`，然后调用完整对象 `BatchPut/BatchGet`。

`multi_buffers` 表示完整对象的 scatter-gather 布局，不表示对象偏移读：

```text
object[0:size0]             <-> Slice0
object[size0:size0+size1]   <-> Slice1
object[...]                 <-> Slice2 ...
```

因此，本设计必须保留“一个完整对象对应多个 Slice/SGE”的能力，但不需要实现 NoF
对象 range read。

### 2.3 与 MooncakeConnector 内存偏移的区别

vLLM `MooncakeConnector` 中出现的 `src_region_offset/dst_region_offset` 用于计算两端 GPU
KV block 的本地/远端内存地址，并最终调用 Transfer Engine 的
`batch_transfer_sync_write()`。它不是 Mooncake Store 对象内部的 `src_offset`，不会进入
NoF `get_into_ranges()` 路径。

## 3. 目标

本设计需要实现：

1. host NoF pool 支持非 NVMe block 对齐的完整对象 Put/Get；
2. GPU GDR NoF pool 支持非 NVMe block 对齐的完整对象 Put/Get；
3. 同时支持单 Slice 和多 Slice 完整对象；
4. 一个 SPDK I/O 内所有 SGE 属于同一个 memory domain；
5. 对齐和非对齐 payload 都保持零拷贝，不引入 host memcpy 或 GPU D2D copy；
6. 非对齐请求不在热路径执行 `spdk_zmalloc`、`cudaMalloc`、dma-buf export 或 RDMA 注册；
7. 元数据区分对象逻辑长度和 NoF 物理分配长度；
8. 不修改 vLLM 和 LMCache，scratch 由 Mooncake 初始化和管理；
9. 原有 host NoF、GPU GDR、memory replica 和非 NoF 路径不发生行为回退。

## 4. 明确非目标

第一版不包含：

- NoF `get_into_ranges()`；
- NoF 对象 `src_offset != 0` 的 range read；
- range read 的 head padding、双 scratch、fragment 映射或 fragment 合并；
- `SGLS.supported = 2` 控制器兼容；
- DWORD 对齐 bounce buffer 或 fallback copy；
- 一个 NoF I/O 中混合 host Slice 与 GPU Slice；
- 一个 I/O 中使用不同 CUDA device 的 Slice；
- 通过 target SPDK 自动生成 padding；
- 将 scratch 内存交给 LMCache 或 vLLM 分配；
- 合并 `Non-Aligned` 分支中的 benchmark 重写和无关修改。

多 GPU 进程后续可以扩展为 `cuda_device_id -> domain + scratch pool` 映射。NoF range
read 只有在出现真实推理调用方后才单独设计和提交 PR。

## 5. 固定前提与关键约束

### 5.1 固定 SPDK NVMf RDMA target，SGL 无 DWORD 对齐要求

当前固定使用的 SPDK NVMf controller 初始化为：

```cpp
cdata->sgls.supported = 1;
```

`SPDK_NVME_SGLS_SUPPORTED == 1` 表示每个 NVMe SGL data segment 没有 DWORD address 或
length granularity requirement。因此允许：

```text
SGE address % 4 != 0
SGE length  % 4 != 0
```

这项结论来自固定的 target 源码和固定部署方式，不推广到其他 NVMe controller、
VFIO-user transport 或第三方 target。本 PR 不实现 `SGLS=2` fallback。若现有 wrapper
可以直接取得 Identify Controller 能力，可在初始化时记录或 fail-fast；不能为了该检查
扩大 SPDK API 修改范围。

### 5.2 NVMe block 对齐与 SGE DWORD 对齐是两个问题

虽然单个 SGE 不要求 4 字节对齐，NVMe 命令的磁盘 offset 和总传输长度仍必须满足
namespace block size。

设对象逻辑长度为 `L`、namespace block size 为 `B`：

```text
physical_size = align_up(L, B)
padding_size  = physical_size - L
```

NoF allocator 分配 `physical_size`，对象 API、checksum 和返回值只向用户暴露 `L`。

完整对象总是从 NoF allocation 的 block-aligned 起点开始读写，所以：

```text
head padding = 0
tail padding = padding_size，且 0 <= padding_size < B
```

### 5.3 单一 memory domain

每个 SPDK I/O 必须明确属于以下一种类型：

| 类型 | 用户 Slice | tail scratch | SPDK `io_opts.memory_domain` |
| --- | --- | --- | --- |
| `HOST_DMA` | host/DPDK DMA memory | host SPDK DMA memory | `nullptr` |
| `GPU_DMABUF` | 已注册 CUDA memory | 同一 CUDA device 的已注册显存 | GPU dma-buf domain |

禁止：

```text
GPU user SGE + host padding SGE + GPU memory domain
```

输入 Slice 混合 host/GPU，或者来自不同 CUDA device 时，提交前返回明确错误，不能隐式
staging。

### 5.4 多 Slice 是一个连续的逻辑对象

对一个 key 的所有用户 Slice：

```text
logical stream = Slice0 || Slice1 || ... || SliceN
```

padding 只能追加在整个对象末尾，不能给每个 Slice 单独对齐：

```text
正确：Slice0 || Slice1 || Slice2 || one tail padding
错误：align(Slice0) || align(Slice1) || align(Slice2)
```

Get API 的目标 buffer 总容量可以大于对象逻辑长度。SGL planner 必须只消费前 `L`
字节，必要时裁剪最后一个 Slice，不能把多余容量算入 NVMe 传输长度。

## 6. 总体设计

### 6.1 I/O 规划

提交 NoF 请求时生成任务级 plan：

```cpp
enum class SpdkNofMemoryKind {
    HOST_DMA,
    GPU_DMABUF,
};

struct SpdkNofIoPlan {
    SpdkNofMemoryKind memory_kind;
    int cuda_device_id;       // HOST_DMA 时为 -1
    uint64_t logical_size;
    uint64_t physical_size;
    uint32_t padding_size;
    uint32_t block_size;
    std::vector<Slice> logical_slices;
};
```

规划阶段负责：

- 检查输入 Slice 非空、地址有效、长度求和无溢出；
- 根据 descriptor/object metadata 确定 `logical_size`；
- 按顺序裁剪用户 Slice，使 `logical_slices` 总长度严格等于 `logical_size`；
- 如果用户总容量小于 `logical_size`，返回 `INVALID_PARAMS`；
- 判断所有有效 Slice 的 memory kind；
- GPU 请求检查所有 Slice 属于同一 CUDA device 且处于已注册 region；
- 校验 descriptor block size 与 namespace block size 一致；
- 计算 `physical_size` 和 `padding_size`。

Slice 归一化应抽成不依赖 SPDK/CUDA 的纯函数并单元测试：

```cpp
remaining = logical_size;
for (slice : input_slices) {
    use = min(slice.size, remaining);
    if (use != 0) logical_slices.push_back({slice.ptr, use});
    remaining -= use;
    if (remaining == 0) break;
}
if (remaining != 0) return INVALID_PARAMS;
```

### 6.2 对齐快速路径

当 `padding_size == 0`：

- 不获取 scratch；
- 直接使用当前 `spdk-gdr-final` vector I/O；
- host 任务使用默认 memory domain；
- GPU 任务显式使用 GPU dma-buf memory domain；
- 单 Slice、多 Slice 都不做 memcpy/cudaMemcpy；
- 保持当前 128 KiB chunk、SGL reset 和 inflight QoS 逻辑。

### 6.3 非对齐完整对象写

当 `padding_size != 0`，构造：

```text
SGE0..N：所有 logical_slices，总长度 L
SGE N+1：同 memory domain 的 zero scratch，长度 padding_size
总长度：physical_size
```

例如：

```text
Slice0 = 3001
Slice1 = 5002
Slice2 = 1000
logical_size  = 9003
block_size    = 4096
physical_size = 12288
padding_size  = 3285
```

最终：

```text
GPU Slice0       3001
GPU Slice1       5002
GPU Slice2       1000
GPU zero scratch 3285
---------------------
总长度          12288
```

因为 `SGLS.supported=1`，不需要把用户尾部 gather 到完整 block，也不需要 GPU D2D
copy。zero scratch 在初始化时清零并在服务期内保持只读。多个并发 write 可以共享同一个
稳定的同域 zero block，前提是 wrapper/SPDK 不会修改它；第一版也可以统一从 slot pool
获取，优先选择容易证明生命周期正确的实现。

### 6.4 非对齐完整对象读

构造：

```text
SGE0..N：所有 logical_slices，总长度 L
SGE N+1：同 memory domain 的 discard scratch，长度 padding_size
总长度：physical_size
```

NVMe/RDMA 直接把前 `L` 字节写入用户 Slice，后 `padding_size` 字节写入 scratch 后丢弃。

该路径没有：

- read completion 后的 host memcpy；
- GPU D2D scatter；
- CUDA stream/event post-copy 状态机；
- 用户 payload bounce buffer。

NVMe completion 到达后即可完成上层 future，但必须先保证所有 sub-I/O 已完成，并归还
本请求独占的 read scratch slot。

### 6.5 Batch 与 multi-buffer 语义

`batch_get_into()`、`batch_get_into_multi_buffers()` 和对应 Put API 都应把每个 key 规划为
独立完整对象 I/O：

- batch 是多个 key 的集合；
- multi-buffer 是单个 key 的多个用户 Slice；
- 每个 key 最多追加一个尾部 scratch SGE；
- padding 基于该 key 的对象逻辑总长度计算，不基于单个 Slice；
- 一个 key 的所有 Slice 和 scratch 必须属于同一 memory domain。

### 6.6 NoF range read 行为

NoF replica 收到 `src_offset != 0` 或 `get_into_ranges()` 时，本 PR 保持明确的
`NOT_SUPPORTED`/等价错误，不能静默当作完整对象读取。Memory replica 原有 range read
能力不受影响。

## 7. Scratch 设计

### 7.1 Host scratch

- 由 Mooncake/SPDK wrapper 初始化；
- 使用一次或少量几次 `spdk_zmalloc(..., SPDK_MALLOC_DMA)` 创建 slab；
- slab 长期存活并使用 SPDK/DPDK 已有 DMA/RDMA 映射；
- write zero block 初始化一次后只读；
- read discard 区按 block size 切成独占 slot；
- 热路径只 acquire/release，不调用 `spdk_zmalloc/free`；
- 建议按 NoF worker 或 I/O channel 分 shard，减少锁竞争。

### 7.2 GPU scratch

- 由 Mooncake 在启用 GDR 后初始化；
- 在当前 CUDA device 上使用 `cudaMalloc` 创建小型 slab；
- 调用现有 `RegisterGpuMemoryRegion` 一次注册整个 slab；
- write zero block 初始化一次后保持只读；
- read discard 区按 block size 切成独占 slot；
- cleanup 顺序：停止新任务、等待 inflight、注销 GPU region、`cudaFree`；
- 不修改 vLLM，显存预算由 Mooncake 配置控制。

建议配置：

```text
MC_NOF_HOST_SCRATCH_SLOTS=<count>
MC_NOF_GPU_SCRATCH_SLOTS=<count>
MC_NOF_HOST_SCRATCH_SLOT_SIZE=<bytes>  # 默认 4096
MC_NOF_GPU_SCRATCH_SLOT_SIZE=<bytes>   # 默认 4096
```

以 4096 字节 block 和 1024 个 read slots 为例，每个 pool 约占 4 MiB。容量由最大并发
非对齐 read 决定，与 LMCache/vLLM KV cache 总容量无关。

### 7.3 池耗尽策略

池耗尽时不得回退到逐请求动态分配。当前第一版明确返回 transfer failure，并记录 scratch
acquire failure；不会回退到 host padding、未注册内存或逐请求 `cudaMalloc/spdk_zmalloc`。
后续如需等待队列，可在 worker 内继续 poll completion 后重试，并增加等待时间和高水位
指标，不改变 SGL 语义。

## 8. 任务与生命周期

在 `SpdkNofTask` 中增加或明确保存：

```cpp
SpdkNofMemoryKind memory_kind;
int cuda_device_id;
uint64_t logical_size;
uint64_t physical_size;
uint32_t padding_size;
std::vector<Slice> logical_slices;
std::vector<std::shared_ptr<void>> buffer_owners;
std::optional<ScratchSlot> read_scratch;
std::shared_ptr<const ScratchBlock> write_zero_block;
```

必须保证：

- 用户 buffer、scratch、SGL 和 `io_opts` 存活到最后一个 sub-I/O 完成；
- `nvmf_reset_sgl` 支持 SPDK 任意 byte offset 的重复 reset；
- `nvmf_next_sge` 能跨多个用户 Slice 和最终 scratch 边界；
- SGL 有效总长度严格等于 `physical_size`；
- padding 只出现在整个对象最后；
- I/O error 和取消路径仍只释放一次 scratch；
- 最后一个 sub-I/O 完成后只完成一次 future；
- 任务清理后不保留指向已归还 slot 的裸指针。

SGL window 计算和 Slice 裁剪必须抽成纯函数，覆盖 128 KiB sub-I/O 跨 Slice/scratch 的
情况。

## 9. Mooncake SPDK wrapper 调整

当前 `SubmitRequestVExt()` 根据全局 `MC_SPDK_GPU_DMABUF` 决定是否给所有请求设置 GPU
memory domain。融合后必须改为任务级选择：

```cpp
int SubmitRequestVExt(...,
                      SpdkNofMemoryKind memory_kind,
                      spdk_nvme_ns_cmd_ext_io_opts *io_opts);
```

行为：

- `HOST_DMA`：`opts == nullptr` 或 `opts->memory_domain == nullptr`；
- `GPU_DMABUF`：设置 GPU domain，所有 SGE 都必须位于已注册 GPU region；
- GPU 功能开启时，host I/O 仍正常工作；
- GPU 未开启时，GPU task 在 planner 阶段失败，host task 不受影响；
- 不能继续使用“全局启用 GDR，就把所有 vector I/O 当作 GPU I/O”的规则。

## 10. Master 元数据与分配

NoF descriptor 和内部 replica 保存：

```cpp
struct NoFDescriptor {
    AllocatedBuffer::Descriptor buffer_descriptor; // size_ 为物理长度
    uint64_t object_size;                           // 逻辑长度
    uint32_t block_size;
};
```

### 10.1 Mount 校验

- `block_size > 0`；
- `base % block_size == 0`；
- `size % block_size == 0`；
- 新 segment 的 block size 与现有 NoF pool 一致；
- block size 来自 target bdev/namespace 查询，不能只依赖默认 512。

### 10.2 一致加锁

Master 分配时在同一个 `segment_mutex_` shared lock 下读取 allocator manager、mounted 状态
和 block size。不要在持有 `ScopedAllocatorAccess` 时再次调用内部加同一把锁的
`getBlockSize()`。

建议提供 NoF 专用 access view：

```cpp
auto access = nof_segment_manager_.getAllocatorAccess();
auto block_size = access.getBlockSize();
const auto &manager = access.getAllocatorManager();
```

### 10.3 逻辑与物理容量

- 对象 API、checksum 和返回长度使用 `object_size`；
- NoF allocator 和 capacity metric 使用 `physical_size`；
- padding 不属于用户对象，不参与 checksum；
- delete 和 eviction 释放完整物理 allocation；
- memory replica 原有 `handle.size_ == slices total` 语义不能为 NoF 全局放宽。

### 10.4 HA 与兼容性

- snapshot、oplog、standby reconstruction 保存并恢复 `object_size/block_size`；
- standby 重建 Replica 后显式恢复 NoF metadata；
- 旧 snapshot NoF payload 缺字段时按 `object_size == physical_size` 解释；
- `NoFSegment/NoFDescriptor` 的 RPC struct layout 已增加字段，当前实现要求 Master 与
  Mooncake client/SSD register 同步升级，不支持新旧二进制混跑；完成升级后再注册 NoF
  segment。若后续必须滚动升级，应把新增字段改为 struct-pack compatible extension，并
  增加跨版本 RPC fixture，不能仅依赖 C++ 字段默认值。

## 11. SPDK 侧结论

本设计不修改 SPDK。当前固定 SPDK NVMf RDMA target 已提供：

- `SGLS.supported = 1`，单个 SGE 无 DWORD 对齐要求；
- `spdk_nvme_ns_cmd_readv_ext/writev_ext`；
- 单 I/O memory domain；
- GPU dma-buf domain 创建、注册、失效和注销；
- GPU region 对 RDMA domain/PD 的注册；
- host 请求使用默认 SPDK/DPDK DMA 映射。

实现前只验证：

1. SPDK 构建启用了 RDMA 与 CUDA/GPU dma-buf；
2. GPU dma-buf symbols 进入最终链接产物；
3. host `opts=nullptr` 和 GPU `opts=GPU domain` 能在同一进程交替提交；
4. 非 4 字节地址/长度的 SGE 能在固定 target 上正确传输；
5. GPU scratch slab 注册一次后可被 NoF I/O 使用；
6. 注销不会发生在 inflight I/O 期间。

只有验证发现当前固定分支行为与上述源码契约不一致时，才单独分析 SPDK；不在本 PR
预先实现其他 controller 的兼容层。

## 12. 实施步骤与提交拆分

### Step 0：冻结 PR base

- 明确 Mooncake PR 的 upstream branch/commit；
- 从目标 base 建立最终开发分支，将干净 GDR commit `37322980` 重放到该 base；
- 先跑通现有 aligned GDR 测试；
- 不 merge 整个 `Non-Aligned`，不带入其大量 upstream 差异。

### Commit 1：任务级 memory domain 与完整对象 planner

- 新增 `SpdkNofMemoryKind`；
- 实现 Slice 裁剪、总长度计算和完整对象 I/O plan；
- `SubmitRequestVExt` 改为 task 显式选择 host/GPU domain；
- 拒绝混合 domain Slice；
- NoF range read 保持明确不支持；
- 保持 aligned I/O 行为不变。

### Commit 2：NoF 逻辑/物理元数据

- segment 注册并校验 block size；
- Master 使用同一锁视图读取 block size 并分配 `align_up(L, B)`；
- `NoFDescriptor` 增加 `object_size/block_size`；
- 补齐 HA、snapshot、standby 和序列化恢复；
- 保持 memory replica 原有校验语义。

### Commit 3：Host 完整对象尾部 padding

- 初始化 host zero/discard scratch；
- 实现单 Slice、多 Slice 后追加一个 host tail SGE；
- 保留 aligned 快速路径；
- 增加 host correctness、并发和 pool exhaustion 测试。

### Commit 4：GPU GDR 完整对象尾部 padding

- 初始化并一次注册 GPU scratch slab；
- 实现单 Slice、多 Slice 后追加一个 GPU tail SGE；
- 不引入 CUDA D2D copy 或每请求 CUDA event；
- 增加 vLLM multi-buffer 形态的 GPU GDR 集成测试；
- 验证热路径无 CUDA allocation/registration。

### Commit 5：文档、指标与性能验证

- 增加 scratch 使用率、等待和失败指标；
- 记录 aligned/non-aligned、host/GPU 请求数；
- 补齐固定 `SGLS=1` 前提和 NoF range read 非目标；
- 提供基准结果。

## 13. 测试计划

### 13.1 纯单元测试

对 `B=512` 和 `B=4096` 测试：

```text
L = 1, 3, 4, B-1, B, B+1, 2B-1, 2B, 2B+1
```

覆盖：

- 单 Slice、多 Slice；
- Slice 边界落在任意 byte offset；
- 最后一个输入 Slice 容量大于剩余对象长度时正确裁剪；
- 用户容量小于对象长度时拒绝；
- padding 只追加一次且总 SGL 长度等于 `physical_size`；
- SGL reset offset 位于第一个 Slice、中间 Slice、最后 Slice 和 scratch；
- 128 KiB sub-I/O 跨 Slice/scratch；
- 非 4 字节地址和非 4 字节长度不触发软件 bounce；
- host/GPU 混合、不同 GPU device 混合拒绝；
- scratch acquire/release、耗尽、取消和 I/O error。

### 13.2 Master/元数据测试

- block size 为 0、错误对齐、不同 pool block size；
- 逻辑长度向上取整后的物理 allocation；
- quota、capacity metric、delete 和 eviction；
- descriptor 序列化/反序列化；
- snapshot/standby 恢复后逻辑长度不丢失；
- 并发 mount/unmount 与 allocation，无重复加锁和除零。

### 13.3 Host 集成测试

- aligned/non-aligned 完整对象 Put/Get 逐字节一致；
- 单 Slice和多 Slice 对象；
- 写入物理 padding 为零；
- GDR 开启时 host 请求仍成功；
- 高并发 read scratch 不串数据；
- 运行期间不发生新 hugepage expansion/MR registration；
- 对齐路径与当前 `spdk-gdr-final` 性能对比。

### 13.4 GPU 集成测试

- 已注册 GPU buffer 的 aligned/non-aligned Put/Get；
- 模拟 LMCache 的完整 chunk 单 buffer；
- 模拟 MooncakeStoreConnector 的一个 key 多 GPU buffer；
- GPU scratch 与全部用户 Slice 位于同一 device/domain；
- 用户 payload 直接 GDR，无 host staging、无 GPU D2D tail copy；
- host/GPU 请求交替提交；
- GPU scratch slab 只注册一次；
- shutdown 和异常路径无 use-after-free。

### 13.5 明确不支持行为测试

- NoF `src_offset != 0` 返回明确错误；
- NoF `get_into_ranges()` 返回明确错误；
- memory replica 的 `get_into_ranges()` 继续工作；
- mixed-domain SGL 在 SPDK submit 前被拒绝。

## 14. 性能与可观测性

当前实现记录：

- host/GPU aligned/non-aligned 请求数；
- pool exhaustion、mixed-domain rejection 次数；
- 非对齐请求累计 padding bytes；

read scratch 当前使用量、高水位和等待时间留到引入等待队列时增加；当前池耗尽立即失败，
不存在隐式等待。

## 14.1 本地验证状态

- `USE_NOF=ON, USE_SPDK_GPU_DMABUF=OFF` 下 `mooncake_store` 静态库编译通过；
- `master_service_test.cpp`、`transfer_task_test.cpp`、`client_buffer_test.cpp`
  测试对象编译通过；
- benchmark 与测试可编译到 object，当前本地 SPDK 安装缺少
  `/usr/local/lib/libspdk_event.a` 等完整静态库，无法完成最终链接；
- 当前机器没有 CUDA toolkit，因此 GPU dma-buf 条件分支尚未做本机构建和设备实测；
- 合入前仍必须在目标部署机完成 host/GPU 非对齐 Put/Get、multi-buffer、pool exhaustion、
  target `SGLS=1` 和性能验收。

验收目标：

- 对齐 host/GPU 吞吐相对当前分支回退不超过 5%；
- 非对齐用户 payload 仍为零拷贝；
- 非对齐路径无逐请求 allocation、MR registration、dma-buf export 或 D2D tail copy；
- 长时间并发无数据错误、MR/显存泄漏和 hugepage 持续增长；
- scratch 大小可配置，默认值下无频繁耗尽。

## 15. PR 验收条件

1. LMCache 完整对象 `batch_get_into` 形态通过 host/GPU 非对齐测试；
2. MooncakeStoreConnector 完整对象 `multi_buffers` 形态通过 GPU 非对齐测试；
3. padding 只追加在完整对象末尾，不在 Slice 之间插入；
4. 一个 SPDK I/O 内不存在 host/GPU 混合 SGE；
5. aligned host、aligned GPU、non-aligned host、non-aligned GPU 均有测试；
6. NoF 逻辑/物理长度在 HA/重启后正确；
7. block size 来自可信来源并在 Master、Client 两侧校验；
8. 热路径无 scratch 动态分配和注册；
9. non-aligned payload 无 host memcpy 或 GPU D2D copy；
10. NoF range read 明确不支持，memory range read 无回归；
11. SPDK 仓库无非必要改动，不包含 `SGLS=2` fallback；
12. `Non-Aligned` 的无关 benchmark/格式化修改未进入 PR；
13. 文档、测试、性能数据和已知限制随 PR 提交。
