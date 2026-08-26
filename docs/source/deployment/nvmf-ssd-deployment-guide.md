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
./dependencies.sh  #自动安装编译mooncake所需的依赖库及SPDK库
```

## 2. 编译安装

### 2.1 编译

```bash
mkdir build
cd build
cmake .. -DUSE_NOF=ON  #-DUSE_NOF=ON表示开启NOF池，默认开启可不指定
make -j
```

**说明**：

- `-DUSE_NOF=ON`：开启NOF池功能（默认开启）
- `-DUSE_NOF=OFF`：关闭NOF池功能
- 执行`cmake .. -DUSE_NOF=ON`后若显示`jaraco.functools`版本太旧，
  可以通过`pip3 install --upgrade jaraco.functools`命令更新。

### 2.2 安装

```bash
make install
```

## 3. Mooncake 服务部署

### 3.1 节点拓扑结构

- **Mooncake master及metadata服务节点**：192.168.65.81
- **Mooncake store节点**：192.168.65.82
- **SSD池节点**：192.168.65.56（提供SSD存储资源。）
- **计算节点**：192.168.65.57（用于后续部署vllm对接Mooncake集群进行推理。）
- Mooncake的编译安装要在Mooncake服务节点，Mooncake store节点以及计算节点都进行。

### 3.2 部署Master服务

在Mooncake服务节点进行。

```bash
mooncake_master --rpc_address=192.168.65.81
```

### 3.3 部署Metadata服务

在Mooncake服务节点进行。

```bash
python3 -m mooncake.http_metadata_server --host=192.168.65.81 --port=8080
```

启动可能出现aiohttp相关错误，需安装 aiohttp 库：

```bash
pip3 install aiohttp
```

### 3.4 部署Store服务

在Mooncake store节点进行。

#### 配置文件`store_service.json`

在`home`目录下创建`store_service.json`配置文件：

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

- `device_name`：可通过`ibv_devices`命令查看192.168.65.82节点的网卡名称。

#### 启动服务

store服务启动会初始化spdk环境，需要在store服务节点（192.168.65.82）上配置大页内存：

```bash
echo 4096 > /proc/sys/vm/nr_hugepages
```

说明：启动只需要少量大页内存，建议启动时配置4096个大页内存即可。

启动store服务：

```bash
python3 -m mooncake.mooncake_store_service --config=/home/store_service.json --port=8081
```

提示：需要事先关闭节点上的防火墙，否则可能会出现无法成功连接master节点。
执行命令`systemctl stop firewalld`、`systemctl disable firewalld`关闭防火墙。
如启动出现Timeout错误，需检查192.168.65.81节点是否配置了代理。
如配置了代理，可以通过`unset http_proxy`、`unset https_proxy`取消代理配置。

## 4. NVMF-SSD 池部署

在Mooncake服务节点进行

### 4.1 前置条件

1. 在Mooncake节点（192.168.65.81）配置到SSD池节点（192.168.65.56）的免密登录
   参考[配置免密登录](https://www.hikunpeng.com/document/detail/zh/kunpengsdss/ecosystemEnable/Ceph/kunpengcephblock_04_0017_0.html)。
2. SSD池节点需提前编译好SPDK，参考[SPDK 编译安装](https://github.com/spdk/spdk/blob/master/README.md#build)。

### 4.2 安装SSH依赖

```bash
pip install -r requirements.txt
```

### 4.3 NoF SSD管理工具说明

本文档说明Mooncake NoF SSD池的两个管理工具：

- `mooncake.mooncake_ssd_create_and_register`：创建SPDK NVMe-oF target，并注册SSD namespace到Mooncake master。
- `mooncake.mooncake_ssd_unregister_and_remove`：从Mooncake master解注册SSD namespace，并可选从SPDK target移除namespace。

旧入口`mooncake.spdk_tgt_create`、`mooncake.mooncake_ssd_register`、`mooncake.mooncake_ssd_unregister`已删除，不再使用。

## 5. 创建并注册 SSD

### 功能

`mooncake_ssd_create_and_register`顺序执行两件事：

1. 通过SSH登录target节点，启动或复用SPDK`nvmf_tgt`，创建transport、subsystem、bdev、namespace、listener。
2. 发现target上的active namespace，并注册到Mooncake master。

### 基本用法

```bash
python3 -m mooncake.mooncake_ssd_create_and_register \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk pci:0000:01:00.0,0000:02:00.0"
```

### 多target用法

```bash
python3 -m mooncake.mooncake_ssd_create_and_register \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk pci:0000:01:00.0" \
  --spdk_target_info "ip:192.168.65.57 path:/home/spdk pci:0000:02:00.0"
```

### 只创建target，不注册master

```bash
python3 -m mooncake.mooncake_ssd_create_and_register \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk pci:0000:01:00.0" \
  --skip-register
```

### target已创建，只注册master

```bash
python3 -m mooncake.mooncake_ssd_create_and_register \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk" \
  --skip-create
```

### target运行中新增SSD

当SPDK target已经运行，需要把新SSD盘加入现有target并注册到Mooncake master时，指定新增盘的PCI号重新执行创建注册工具：

```bash
python3 -m mooncake.mooncake_ssd_create_and_register \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk pci:0000:03:00.0"
```

该命令会复用已运行的`nvmf_tgt`、transport、subsystem和listener，仅对新增PCI盘创建bdev、加入namespace，并将新namespace注册到master。已有namespace和listener会被识别并跳过，不会重复创建。

### 参数说明

| 参数 | 是否必选 | 说明 |
| --- | --- | --- |
| `--master_server_address` | 是 | Mooncake master地址，例如`192.168.65.81:50051`。 |
| `--spdk_target_info` | 是 | target描述，可重复指定。格式为`ip:<target_ip> path:<spdk_path> [pci:<pci1>,<pci2>]`。 |
| `--skip-create` | 否 | 跳过target创建阶段，只执行注册。 |
| `--skip-register` | 否 | 跳过master注册阶段，只执行target创建。 |
| `--dry-run` | 否 | 只打印流程，不执行远端操作。 |
| `--core-mask` | 否 | 启动 `nvmf_tgt` 使用的CPU core mask，默认`0xff`。 |
| `--transport-type` | 否 | NVMe-oF transport类型，默认`RDMA`。 |
| `--max-queue-depth` | 否 | transport最大队列深度，默认`128`。 |
| `--max-io-qpairs-per-ctrlr` | 否 | 每个controller最大I/O qpair数，默认`127`。 |
| `--max-io-size` | 否 | 最大I/O大小，默认`4096`。 |
| `--in-capsule-data-size` | 否 | in-capsule data size，默认`131072`。 |
| `--io-unit-size` | 否 | I/O unit size，默认`131072`。 |
| `--max-aq-depth` | 否 | admin queue depth，默认`128`。 |
| `--num-shared-buffers` | 否 | transport shared buffer数量，默认`4096`。 |
| `--buf-cache-size` | 否 | 每个poll group的buffer cache size，默认`32`。 |
| `--username` | 否 | SSH用户名，默认`root`。 |
| `--port` | 否 | SSH端口，默认`22`。 |
| `--password` | 否 | SSH密码。 |
| `--key-file` | 否 | SSH私钥文件。 |
| `-D, --define` | 否 | 注册阶段字段覆盖，例如`-Dtrsvcid=4420`。 |

## 6. 解注册并可选移除 SSD

### 功能

`mooncake_ssd_unregister_and_remove`固定按照以下顺序执行：

1. 先从Mooncake master解注册namespace，停止该NoF segment继续参与分配。
2. 如果指定`--remove-target-namespace`，再通过SPDK RPC执行`nvmf_subsystem_remove_ns`，从target subsystem中移除对应namespace。
3. 如果同时指定`--detach-bdev`，最后执行`bdev_nvme_detach_controller`，释放对应SPDK NVMe bdev controller。

工具不支持“只移除target、不解注册master”的模式，避免出现target盘已下线但master仍残留可分配元数据的风险。

### 只从master解注册指定namespace

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk ns:1 nqn:nqn.2016-06.io.spdk:cnode1"
```

### 解注册target上所有namespace

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk"
```

### 解注册master，并从target移除namespace

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk ns:1 nqn:nqn.2016-06.io.spdk:cnode1" \
  --remove-target-namespace
```

### 解注册master，并从target移除所有namespace

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk" \
  --remove-target-namespace
```

### 解注册master、移除指定namespace，并detach bdev

```bash
python3 -m mooncake.mooncake_ssd_unregister_and_remove \
  --master_server_address 192.168.65.81:50051 \
  --spdk_target_info "ip:192.168.65.56 path:/home/spdk ns:1 nqn:nqn.2016-06.io.spdk:cnode1" \
  --remove-target-namespace \
  --detach-bdev
```

### 解注册master、移除所有namespace，并detach bdev

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
| `--master_server_address` | 是 | Mooncake master地址，例如`192.168.65.81:50051`。 |
| `--spdk_target_info` | 是 | target/namespace描述，可重复指定。格式为`ip:<target_ip> path:<spdk_path> [ns:<nsid>] [nqn:<subsystem_nqn>]`。 |
| `--remove-target-namespace` | 否 | master解注册成功后，从SPDK target subsystem中移除匹配namespace。 |
| `--detach-bdev` | 否 | 移除namespace后detach对应SPDK NVMe bdev controller。必须配合`--remove-target-namespace`使用。 |
| `--dry-run` | 否 | 只打印流程，不执行远端操作。 |
| `--username` | 否 | SSH用户名，默认`root`。 |
| `--port` | 否 | SSH 端口，默认`22`。 |
| `--password` | 否 | SSH密码。 |
| `--key-file` | 否 | SSH私钥文件。 |
| `-D, --define` | 否 | 解注册阶段字段覆盖，例如`-Dtrsvcid=4420`。 |

### 6.8 获取 Target 端盘信息

进入SSD池节点的SPDK目录，执行以下命令：

1. 查看子系统信息（nqn和namespace号）：

```bash
./scripts/rpc.py nvmf_get_subsystems
```

1. 查看盘的详细信息（块大小、PCI号等）：

```bash
./scripts/rpc.py bdev_get_bdevs
```

### 6.9 使用建议

- 日常扩容使用`mooncake_ssd_create_and_register`，一个命令完成target创建和master注册。
- target已存在、只是master需要重新感知时，使用`--skip-create`。
- 缩容时默认只解注册master；确认需要target侧同步移除时，再加`--remove-target-namespace`。
- `--detach-bdev`会释放底层SPDK bdev controller，影响更大，只建议在明确下线该盘时使用。
- 执行高风险操作前可先加`--dry-run`检查匹配范围。

## 7. 性能测试

### 7.1 使用内置压测工具

在Mooncake服务节点进行。

```bash
ENDPOINTS='traddr:192.168.65.56 trsvcid:4420'
ENDPOINTS+='subnqn:nqn.2016-06.io.spdk:cnode1 trtype:RDMA'
ENDPOINTS+='adrfam:IPv4 ns:1, traddr:192.168.65.56 trsvcid:4420'
ENDPOINTS+='subnqn:nqn.2016-06.io.spdk:cnode1 trtype:RDMA'
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

### 7.2 VLLM+LMCache+Mooncake端到端测试

在192.168.65.57节点部署VLLM服务，并配置LMCache插件。
192.168.65.57作为推理节点，需要配置显卡资源(本文档以1张Nvidia A100显卡为例)。

#### 安装Nvidia驱动

下载并安装显卡对应的CUDA驱动，根据GPU型号选择合适的驱动版本。
参考[Nvidia 驱动安装](https://www.nvidia.com/Download/index.aspx)。
A100显卡对应的CUDA驱动本文档选择版本为12.9.0。

下载CUDA 12.9.0驱动：

```bash
wget https://developer.download.nvidia.com/compute/cuda/12.9.0/local_installers/cuda_12.9.0_575.51.03_linux_sbsa.run
```

安装CUDA 12.9.0驱动：

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

用一个干净的python环境安装pytorch及torch相关库，使用conda构建一个新python 3.11的环境。

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

创建新的python3.11环境：

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

#### 安装LMCache

用conda环境安装LMCache，设置LMCache环境变量：

```bash
export CUDA_HOME=/usr/local/cuda/
export PATH=$CUDA_HOME/bin:$PATH
export LD_LIBRARY_PATH=$CUDA_HOME/lib64:$LD_LIBRARY_PATH
export TORCH_CUDA_ARCH_LIST="8.0"
```

下载LMCache代码并安装：

```bash
git clone https://gitcode.com/boostkit/LMCache.git
cd LMCache
git checkout v0.3.13_support_hugepage_memory
pip install -e . --no-build-isolation
```

#### 安装VLLM及相关库

用conda环境安装VLLM及相关库：

```bash
git clone https://gitcode.com/vllm-project/vllm.git
cd vllm
git checkout v0.15.2rc0
python use_existing_torch.py   #指向已经安装的pytorch
pip install -r requirements/build.txt
#编译vllm非常吃内存，建议用taskset -c限制下编译的核心数
taskset -c 0-31 pip install -e . --no-build-isolation
```

#### 启动 VLLM 服务

1. 设置环境变量：

```bash
export LMCACHE_CONFIG_FILE="/path/vllm-lmcache-mooncake-config.yaml"
export MC_NOF_WORKERS=4
export MC_NOF_SUBMIT_CHUNK_BYTES=$((1 << 17))  #128KB
export MC_NOF_INFLIGHT_BYTES_LIMIT=$((1 << 25))  #32MB
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
| `--port`                   | VLLM服务端口号                 |
| `--tensor-parallel-size`   | 张量并行度（与GPU数量一致）   |
| `--gpu-memory-utilization` | GPU内存利用率（0.8表示80%）  |
| `--trust-remote-code`      | 信任远程代码执行                |
| `--kv-transfer-config`     | KV缓存传输配置（LMCache插件） |
| `--model`                  | 模型路径（Qwen3-8B）            |

#### LMCache配置文件

```yaml
chunk_size: 256
remote_url: "mooncakestore://192.168.65.81:50051/"
remote_serde: "native"
local_cpu: True
max_local_cpu_size: 8
enable_mooncake_nof_pool: True

extra_config:
  local_hostname: "localhost"
  metadata_server: "http://192.168.65.81:8080/metadata"
  master_server_address: "192.168.65.81:50051"
  global_segment_size: 0
  protocol: "rdma"
  device_name: "mlx5_0"
```

**说明**：

- `extra_config`中的`local_hostname`、`metadata_server`、
  `master_server_address`、`protocol`参数与`store_service.json`一致。由于本节点单独作为计算节点不提供存储服务，`global_segment_size`配置为`0`，
  `device_name`同样可通过`ibv_devices`命令查看192.168.65.57节点的网卡名称。
- `enable_mooncake_nof_pool=True`：启用NOF池化功能（使用SPDK申请大页内存）。
- 若设为 `False`，则申请普通内存，KVCache无法写入SSD池。

#### 环境变量说明

| 环境变量                      | 说明                                | 默认值 |
| ----------------------------- | ----------------------------------- | ------ |
| `MC_NOF_WORKERS`              | 处理SPDK NoF IO操作的工作线程数量。 | 4      |
| `MC_NOF_SUBMIT_CHUNK_BYTES`   | 每次向SPDK提交的IO操作大小。      | 128KB  |
| `MC_NOF_INFLIGHT_BYTES_LIMIT` | 系统中允许的最大未完成IO字节数。    | 32MB   |

**注意**：这三个参数共同构成了SPDK NoF IO的QoS控制机制。

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

## 修订记录

| 文档版本 | 发布日期  | 修改说明       |
| ------- | -------|----------|
| 01 | 2026-09-30 | 第一次正式发布。 |
