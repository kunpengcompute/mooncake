# Mooncake NVMF-SSD 池部署指南

## 1. 环境准备

### 1.1 获取源码

```bash
git clone https://gitcode.com/boostkit/mooncake.git
cd mooncake
```

### 1.2 切换分支

```bash
git checkout dev-v0.3.8_add_ssd_cache
```

### 1.3 安装依赖

```bash
./dependencies.sh  # 自动安装编译 mooncake 所需的依赖库及 SPDK 库
```

## 2. 编译安装

### 2.1 编译

```bash
mkdir build
cd build
cmake .. -DUSE_NOF=ON  # -DUSE_NOF=ON 表示开启 NOF 池，默认开启可不指定
make -j
```

**说明**：

- `-DUSE_NOF=ON`：开启 NOF 池功能（默认开启）
- `-DUSE_NOF=OFF`：关闭 NOF 池功能
- 执行 `cmake .. -DUSE_NOF=ON` 后若显示 `jaraco.functools` 版本太旧，
  可以通过 `pip3 install --upgrade jaraco.functools` 命令更新

### 2.2 安装

```bash
make install
```

## 3. Mooncake 服务部署

### 3.1 节点拓扑结构

- **Mooncake master 及 metadata 服务节点**：192.168.65.81
- **Mooncake store 节点**：192.168.65.82
- **SSD 池节点**：192.168.65.56（提供 SSD 存储资源）
- **计算节点**：192.168.65.57（用于后续部署 vllm 对接 Mooncake 集群进行推理）
- Mooncake 的编译安装要在 Mooncake 服务节点，Mooncake store 节点以及计算节点都进行

### 3.2 部署 Master 服务

在 Mooncake 服务节点进行

```bash
mooncake_master \
  --rpc_address=192.168.65.81 \
  --nof_eviction_ratio=0.05 \
  --nof_eviction_high_watermark_ratio=0.95
```

#### NoF SSD 淘汰参数

| 参数 | 默认值 | 取值范围 | 说明 |
| --- | --- | --- | --- |
| `--nof_eviction_high_watermark_ratio` | `0.95` | `[0.0, 1.0]` | NoF SSD 池使用率高水位。全局 NoF SSD 池使用率超过该值时触发 NoF 副本淘汰。`0.95` 表示使用率超过 95% 时触发。 |
| `--nof_eviction_ratio` | `0.05` | `[0.0, 1.0]` | 每轮 NoF 淘汰的目标对象比例下限。`0.05` 表示每轮至少以约 5% 的对象为目标执行淘汰；实际目标比例还会根据当前使用率超过高水位的幅度动态增加。 |

NoF 淘汰由 master 的后台淘汰线程执行，优先选择 lease 已过期且 NoF 副本状态完整的对象。当 NoF SSD 池超过高水位，或者空间分配触发主动淘汰请求时，master 会按上述参数计算本轮淘汰目标。参数可以通过命令行设置，也可以在 master JSON/YAML 配置文件中使用 `nof_eviction_ratio` 和 `nof_eviction_high_watermark_ratio` 字段设置。

### 3.3 部署 Metadata 服务

在 Mooncake 服务节点进行

```bash
python3 -m mooncake.http_metadata_server --host=192.168.65.81 --port=8080
```

启动可能出现 aiohttp 相关错误，需安装 aiohttp 库：

```bash
pip3 install aiohttp
```

### 3.4 部署 Store 服务

在 Mooncake store 节点进行

#### 配置文件 `store_service.json`

在 `home` 目录下创建 `store_service.json` 配置文件：

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

**说明**：

- `device_name`：可通过 `ibv_devices` 命令查看 192.168.65.82 节点的网卡名称

#### 启动服务

store 服务启动会初始化 spdk 环境，需要在 store 服务节点（192.168.65.82）上配置大页内存：

```bash
echo 4096 > /proc/sys/vm/nr_hugepages
```

说明：启动只需要少量大页内存，建议启动时配置 4096 个大页内存即可。

启动 store 服务：

```bash
python3 -m mooncake.mooncake_store_service --config=/home/store_service.json --port=8081
```

提示：需要事先关闭节点上的防火墙，否则可能会出现无法成功连接 master 节点。
执行命令 `systemctl stop firewalld`、`systemctl disable firewalld` 关闭防火墙。
如启动出现 Timeout 错误，需检查 192.168.65.81 节点是否配置了代理。
如配置了代理，可以通过 `unset http_proxy`、`unset https_proxy` 取消代理配置。

## 4. NVMF-SSD 池部署

在 Mooncake 服务节点进行

### 4.1 前置条件

1. 在 Mooncake 节点（192.168.65.81）配置到 SSD 池节点（192.168.65.56）的免密登录
   参考 [配置免密登录](https://www.hikunpeng.com/document/detail/zh/kunpengsdss/ecosystemEnable/Ceph/kunpengcephblock_04_0017_0.html)
2. SSD 池节点需提前编译好 SPDK
   参考 [SPDK 编译安装](https://github.com/spdk/spdk/blob/master/README.md#build)

### 4.2 安装 SSH 依赖

```bash
pip install -r requirements.txt
```

### 4.3 NoF SSD 管理工具说明

本文档说明 Mooncake NoF SSD 池的两个管理工具：

- `mooncake.mooncake_ssd_create_and_register`：创建 SPDK NVMe-oF target，并注册 SSD namespace 到 Mooncake master。
- `mooncake.mooncake_ssd_unregister_and_remove`：从 Mooncake master 解注册 SSD namespace，并可选从 SPDK target 移除 namespace。

旧入口 `mooncake.spdk_tgt_create`、`mooncake.mooncake_ssd_register`、`mooncake.mooncake_ssd_unregister` 已删除，不再使用。

## 5. 创建并注册 SSD

### 功能

`mooncake_ssd_create_and_register` 顺序执行两件事：

1. 通过 SSH 登录 target 节点，启动或复用 SPDK `nvmf_tgt`，创建 transport、subsystem、bdev、namespace、listener。
2. 发现 target 上的 active namespace，并注册到 Mooncake master。

### 基本用法

```bash
python3 -m mooncake.mooncake_ssd_create_and_register \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk pci:0000:01:00.0,0000:02:00.0"
```

### 多 target 用法

```bash
python3 -m mooncake.mooncake_ssd_create_and_register \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk pci:0000:01:00.0" \
  --spdk_target_info "ip:192.168.65.57 path:/home/spdk pci:0000:02:00.0"
```

### 只创建 target，不注册 master

```bash
python3 -m mooncake.mooncake_ssd_create_and_register \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk pci:0000:01:00.0" \
  --skip-register
```

### target 已创建，只注册 master

```bash
python3 -m mooncake.mooncake_ssd_create_and_register \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk" \
  --skip-create
```

### target 运行中新增 SSD

当 SPDK target 已经运行，需要把新 SSD 盘加入现有 target 并注册到 Mooncake master 时，指定新增盘的 PCI 号重新执行创建注册工具：

```bash
python3 -m mooncake.mooncake_ssd_create_and_register \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk pci:0000:03:00.0"
```

该命令会复用已运行的 `nvmf_tgt`、transport、subsystem 和 listener，仅对新增 PCI 盘创建 bdev、加入 namespace，并将新 namespace 注册到 master。已有 namespace 和 listener 会被识别并跳过，不会重复创建。

### 参数说明

| 参数 | 是否必选 | 说明 |
| --- | --- | --- |
| `--master_server_address` | 是 | Mooncake master 地址，例如 `192.168.65.81:50051`。 |
| `--spdk_target_info` | 是 | target 描述，可重复指定。格式为 `ip:<target_ip> path:<spdk_path> [pci:<pci1>,<pci2>]`。 |
| `--skip-create` | 否 | 跳过 target 创建阶段，只执行注册。 |
| `--skip-register` | 否 | 跳过 master 注册阶段，只执行 target 创建。 |
| `--dry-run` | 否 | 只打印流程，不执行远端操作。 |
| `--core-mask` | 否 | 启动 `nvmf_tgt` 使用的 CPU core mask，默认 `0xff`。 |
| `--transport-type` | 否 | NVMe-oF transport 类型，默认 `RDMA`。 |
| `--max-queue-depth` | 否 | transport 最大队列深度，默认 `128`。 |
| `--max-io-qpairs-per-ctrlr` | 否 | 每个 controller 最大 I/O qpair 数，默认 `127`。 |
| `--max-io-size` | 否 | 最大 I/O 大小，默认 `4096`。 |
| `--in-capsule-data-size` | 否 | in-capsule data size，默认 `131072`。 |
| `--io-unit-size` | 否 | I/O unit size，默认 `131072`。 |
| `--max-aq-depth` | 否 | admin queue depth，默认 `128`。 |
| `--num-shared-buffers` | 否 | transport shared buffer 数量，默认 `4096`。 |
| `--buf-cache-size` | 否 | 每个 poll group 的 buffer cache size，默认 `32`。 |
| `--username` | 否 | SSH 用户名，默认 `root`。 |
| `--port` | 否 | SSH 端口，默认 `22`。 |
| `--password` | 否 | SSH 密码。 |
| `--key-file` | 否 | SSH 私钥文件。 |
| `-D, --define` | 否 | 注册阶段字段覆盖，例如 `-Dtrsvcid=4420`。 |

## 6. 解注册并可选移除 SSD

### 功能

`mooncake_ssd_unregister_and_remove` 固定按照以下顺序执行：

1. 先从 Mooncake master 解注册 namespace，停止该 NoF segment 继续参与分配。
2. 如果指定 `--remove-target-namespace`，再通过 SPDK RPC 执行 `nvmf_subsystem_remove_ns`，从 target subsystem 中移除对应 namespace。
3. 如果同时指定 `--detach-bdev`，最后执行 `bdev_nvme_detach_controller`，释放对应 SPDK NVMe bdev controller。

工具不支持“只移除 target、不解注册 master”的模式，避免出现 target 盘已下线但 master 仍残留可分配元数据的风险。

### 只从 master 解注册指定 namespace

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk ns:1 nqn:nqn.2016-06.io.spdk:cnode1"
```

### 解注册 target 上所有 namespace

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk"
```

### 解注册 master，并从 target 移除 namespace

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk ns:1 nqn:nqn.2016-06.io.spdk:cnode1" \
  --remove-target-namespace
```

### 解注册 master，并从 target 移除所有 namespace

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk" \
  --remove-target-namespace
```

### 解注册 master、移除指定 namespace，并 detach bdev

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk ns:1 nqn:nqn.2016-06.io.spdk:cnode1" \
  --remove-target-namespace \
  --detach-bdev
```

### 解注册 master、移除所有 namespace，并 detach bdev

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk" \
  --remove-target-namespace \
  --detach-bdev
```

### 参数说明

| 参数 | 是否必选 | 说明 |
| --- | --- | --- |
| `--master_server_address` | 是 | Mooncake master 地址，例如 `192.168.65.81:50051`。 |
| `--spdk_target_info` | 是 | target/namespace 描述，可重复指定。格式为 `ip:<target_ip> path:<spdk_path> [ns:<nsid>] [nqn:<subsystem_nqn>]`。 |
| `--remove-target-namespace` | 否 | master 解注册成功后，从 SPDK target subsystem 中移除匹配 namespace。 |
| `--detach-bdev` | 否 | 移除 namespace 后 detach 对应 SPDK NVMe bdev controller。必须配合 `--remove-target-namespace` 使用。 |
| `--dry-run` | 否 | 只打印流程，不执行远端操作。 |
| `--username` | 否 | SSH 用户名，默认 `root`。 |
| `--port` | 否 | SSH 端口，默认 `22`。 |
| `--password` | 否 | SSH 密码。 |
| `--key-file` | 否 | SSH 私钥文件。 |
| `-D, --define` | 否 | 解注册阶段字段覆盖，例如 `-Dtrsvcid=4420`。 |

### 6.8 获取 Target 端盘信息

进入 SSD池 节点的 SPDK 目录，执行以下命令：

1. 查看子系统信息（nqn 和 namespace 号）：

```bash
./scripts/rpc.py nvmf_get_subsystems
```

1. 查看盘的详细信息（块大小、PCI 号等）：

```bash
./scripts/rpc.py bdev_get_bdevs
```

### 6.9 使用建议

- 日常扩容使用 `mooncake_ssd_create_and_register`，一个命令完成 target 创建和 master 注册。
- target 已存在、只是 master 需要重新感知时，使用 `--skip-create`。
- 缩容时默认只解注册 master；确认需要 target 侧同步移除时，再加 `--remove-target-namespace`。
- `--detach-bdev` 会释放底层 SPDK bdev controller，影响更大，只建议在明确下线该盘时使用。
- 执行高风险操作前可先加 `--dry-run` 检查匹配范围。

## 7. 性能测试

### 7.1 使用内置压测工具

在 Mooncake 服务节点进行

```bash
ENDPOINTS='traddr:192.168.65.56 trsvcid:4420 '
ENDPOINTS+='subnqn:nqn.2016-06.io.spdk:cnode1 trtype:RDMA '
ENDPOINTS+='adrfam:IPv4 ns:1, traddr:192.168.65.56 trsvcid:4420 '
ENDPOINTS+='subnqn:nqn.2016-06.io.spdk:cnode1 trtype:RDMA '
ENDPOINTS+='adrfam:IPv4 ns:2'

./build/mooncake-store/benchmarks/nof_worker_pool_bench \
    --endpoints="$ENDPOINTS" \
    --op=read \
    --io_size=1048576 \
    --iodepth=8 \
    --warmup_sec=3 \
    --duration_sec=30
```

#### 压测参数说明

| 参数             | 说明                       |
| ---------------- | -------------------------- |
| `--endpoints`    | 测试盘信息                 |
| `--op`           | 读写操作类型（read/write） |
| `--io_size`      | 块大小（字节）             |
| `--iodepth`      | 读写队列深度               |
| `--warmup_sec`   | 预热时间（秒）             |
| `--duration_sec` | 测试时长（秒）             |

### 7.2 VLLM+LMCache+Mooncake 端到端测试

在 192.168.65.57 节点部署 VLLM 服务，并配置 LMCache 插件。
192.168.65.57 作为推理节点，需要配置显卡资源(本文档以 1 张 Nvidia A100 显卡为例)。

#### 安装 Nvidia 驱动

下载并安装显卡对应的 CUDA 驱动，根据 GPU 型号选择合适的驱动版本。
参考 [Nvidia 驱动安装](https://www.nvidia.com/Download/index.aspx)。
A100 显卡对应的 CUDA 驱动本文档选择版本为 12.9.0。

下载 CUDA 12.9.0 驱动：

```bash
wget https://developer.download.nvidia.com/compute/cuda/12.9.0/local_installers/cuda_12.9.0_575.51.03_linux_sbsa.run
```

安装 CUDA 12.9.0 驱动：

```bash
sudo sh cuda_12.9.0_575.51.03_linux_sbsa.run
```

**提示**：

- 驱动安装需要解压，gcc、内核等工具包，安装之前确保系统已有这些工具包。
  可以通过以下命令安装：

  ```bash
  yum install -y kernel-devel-$(uname -r) kernel-headers-$(uname -r) \
      gcc make g++ tar
  ```

#### 安装 pytorch 及 torch 相关库

用一个干净的 python 环境安装 pytorch 及 torch 相关库，使用 conda 构建一个新 python 3.11 的环境。

下载conda安装脚本：

```bash
wget https://repo.anaconda.com/archive/Anaconda3-2025.12-2-Linux-aarch64.sh
```

安装conda：

```bash
bash Anaconda3-2025.12-2-Linux-aarch64.sh
```

重启shell会话，使conda生效：

```bash
source /root/anaconda3/etc/profile.d/conda.sh
```

创建新的python 3.11环境：

```bash
conda create -n vllm python=3.11
conda activate vllm
```

安装pytorch及torch相关库：

```bash
pip install torch==2.9.0 \
    -f https://mirrors.aliyun.com/pytorch-wheels/cu129 \
    --trusted-host mirrors.aliyun.com
```

#### 安装 LMCache

用 conda 环境安装 LMCache ，设置 LMCache 环境变量：

```bash
export CUDA_HOME=/usr/local/cuda/
export PATH=$CUDA_HOME/bin:$PATH
export LD_LIBRARY_PATH=$CUDA_HOME/lib64:$LD_LIBRARY_PATH
export TORCH_CUDA_ARCH_LIST="8.0"
```

下载 LMCache 代码并安装：

```bash
git clone https://gitcode.com/boostkit/LMCache.git
cd LMCache
git checkout v0.3.13_support_hugepage_memory
pip install -e . --no-build-isolation
```

#### 安装 VLLM 及相关库

用 conda 环境安装 VLLM 及相关库：

```bash
git clone https://gitcode.com/vllm-project/vllm.git
cd vllm
git checkout v0.15.2rc0
python use_existing_torch.py   # 指向已经安装的 pytorch
pip install -r requirements/build.txt
# 编译 vllm 非常吃内存，建议用 taskset -c 限制下编译的核心数
taskset -c 0-31 pip install -e . --no-build-isolation
```

#### 启动 VLLM 服务

1. 设置环境变量：

```bash
export LMCACHE_CONFIG_FILE="/path/vllm-lmcache-mooncake-config.yaml"
export MC_STORE_NUMA_SOCKET_ID=0
export MC_NOF_WORKERS=4
export MC_NOF_SUBMIT_CHUNK_BYTES=$((1 << 17))  # 128KB
export MC_NOF_INFLIGHT_BYTES_LIMIT=$((1 << 23))  # 8MB
```

1. 启动服务：

```bash
vllm serve --port 7070 \
           --tensor-parallel-size 1 \
           --gpu-memory-utilization 0.8 \
           --trust-remote-code \
           --kv-transfer-config \
           '{"kv_connector":"LMCacheConnectorV1","kv_role":"kv_both"}' \
           --model /home/Qwen3-8B
```

参数说明：

| 参数                       | 说明                            |
| -------------------------- | ------------------------------- |
| `--port`                   | VLLM 服务端口号                 |
| `--tensor-parallel-size`   | 张量并行度（与 GPU 数量一致）   |
| `--gpu-memory-utilization` | GPU 内存利用率（0.8 表示 80%）  |
| `--trust-remote-code`      | 信任远程代码执行                |
| `--kv-transfer-config`     | KV 缓存传输配置（LMCache 插件） |
| `--model`                  | 模型路径（Qwen3-8B）            |

#### LMCache 配置文件

```yaml
chunk_size: 256
remote_url: "mooncakestore://192.168.65.81:50051/"
remote_serde: "naive"
local_cpu: True
max_local_cpu_size: 8
enable_mooncake_nof_pool: True

extra_config:
  save_chunk_meta: False
  local_hostname: "localhost"
  metadata_server: "http://192.168.65.81:8080/metadata"
  master_server_address: "192.168.65.81:50051"
  global_segment_size: 0
  protocol: "rdma"
  device_name: "mlx5_0"
```

**说明**：

- `extra_config` 中的 `local_hostname`、`metadata_server`、
  `master_server_address`、`protocol` 参数与 `store_service.json` 一致。由于本节点
  单独作为计算节点不提供存储服务，`global_segment_size` 配置为 `0`，
  `device_name` 同样可通过 `ibv_devices` 命令查看 192.168.65.57 节点的网卡名称
- `enable_mooncake_nof_pool=True`：启用 NOF 池化功能（使用 SPDK 申请大页内存）
- 若设为 `False`，则申请普通内存，KVCache 无法写入 SSD 池

#### 环境变量说明

| 环境变量                      | 说明                                           | 默认值               |
| ----------------------------- | ---------------------------------------------- | -------------------- |
| `MC_STORE_NUMA_SOCKET_ID`     | NoF worker 绑定的 NUMA 节点编号                | 当前 CPU 所在 NUMA 节点 |
| `MC_NOF_WORKERS`              | 处理 SPDK NoF IO 操作的工作线程数量            | 4                    |
| `MC_NOF_SUBMIT_CHUNK_BYTES`   | 每次向 SPDK 提交的 IO 操作大小                 | 128KB                |
| `MC_NOF_INFLIGHT_BYTES_LIMIT` | 单个 NoF segment、单个读或写方向允许的最大未完成 IO 字节数 | 32MB |

**注意**：后三个参数共同构成 SPDK NoF IO 的 QoS 控制机制；`MC_STORE_NUMA_SOCKET_ID` 用于控制 NoF worker 的 NUMA affinity。

#### `MC_NOF_INFLIGHT_BYTES_LIMIT` 推荐配置

该参数按 NoF segment（通常对应一个 namespace）分别生效，并非整个 NoF 池共享的全局上限。使用多个 namespace 时，所有 segment 的在途 I/O 会叠加；例如每盘配置为 `8MB`、Target 挂载 9 块盘时，写方向的总在途量最高约为 `72MB`。

建议先根据 Target 的 `nvmf_create_transport -q` 队列深度确定单盘上限。为保留 completion 回收和突发流量的余量，单盘在途请求数建议不超过 `-q` 的约 50%：

```text
MC_NOF_INFLIGHT_BYTES_LIMIT <= Target queue depth * 50% * MC_NOF_SUBMIT_CHUNK_BYTES
```

以 `MC_NOF_SUBMIT_CHUNK_BYTES=128KB` 为例，Target 使用 `-q 128` 时，单盘推荐配置为 `8MB`，即最多约 64 个请求在途。对于多盘 Target，可使用以下保守起始配置：

| Target 内 namespace 数 | 推荐 `MC_NOF_INFLIGHT_BYTES_LIMIT` | 说明 |
| --- | --- | --- |
| 1 - 8 | `8MB` | 适用于 `-q 128` 的稳定起始值。 |
| 9 - 12 | `4MB` | 限制多个 qpair 叠加后的总在途量，建议先使用该值完成稳定性压测。 |

9 块盘场景如已完成长时间压测且未出现 `submit io fail`，可尝试使用 `8MB` 以换取更高吞吐。若 Target 队列深度提高到 `-q 256`，可在压测验证后将 1 - 4 块盘的配置提高到 `16MB`。不要仅因盘数较少就直接使用代码默认值 `32MB`，该值在高并发 vLLM/LMCache 写入下可能超出 Target qpair 或共享 RDMA 资源的承受范围。

#### 执行多轮对话推理测试

```bash
vllm bench serve \
--backend openai \
--model /home/Qwen3-8B \
--dataset-name sharegpt \
--dataset-path /home/ShareGPT.json \
--burstiness 1 \
--request-rate 10 \
--max-concurrency 4 \
--save-result \
--result-dir ./results \
--host localhost \
--port 7070
```

参数说明：

| 参数                | 说明                            |
| ------------------- | ------------------------------- |
| `--backend`         | 后端服务类型（openai）          |
| `--model`           | 模型路径（Qwen3-8B）            |
| `--dataset-name`    | 测试数据集名称                  |
| `--dataset-path`    | 测试数据集路径（ShareGPT.json） |
| `--burstiness`      | 最大请求并发数                  |
| `--input-len`       | 输入序列长度                    |
| `--output-len`      | 输出序列长度                    |
| `--request-rate`    | 请求速率（QPS）                 |
| `--max-concurrency` | 最大并发数                      |
| `--save-result`     | 保存测试结果                    |
| `--result-dir`      | 测试结果保存目录                |
| `--host`            | 服务主机名                      |
| `--port`            | 服务端口号                      |
| `--num-clients`     | 客户端数量                      |
| `--num-rounds`      | 测试轮数                        |
| `--save-detailed`   | 保存详细测试结果                |
