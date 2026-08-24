---
title: "vLLM 与 Mooncake 对接源码解读：Prefill/Decode 分离中的 KV Cache 如何跨节点传输"
date: 2026-08-24T16:58:43+08:00
draft: false
tags: ["vLLM", "Mooncake", "KV Cache", "RDMA", "PD 分离", "源码解读"]
categories: ["大模型推理"]
summary: "深入分析 vLLM 内置 MooncakeConnector：从代理拆分请求、Scheduler 元数据、GPU KV Cache 注册，到 Mooncake Transfer Engine 通过 RDMA 将 Prefill KV 直接搬到 Decode 节点。"
---

在大模型在线推理中，Prefill 和 Decode 的计算特征非常不同：

- **Prefill** 一次处理整个 Prompt，矩阵计算密集，适合大 Batch 和高算力利用率；
- **Decode** 每轮通常只生成一个 token，更依赖 KV Cache 容量、访存延迟和调度响应。

如果二者运行在同一组 GPU 上，长 Prompt 的 Prefill 会阻塞正在 Decode 的请求，Decode 的小 Batch 又会降低 Prefill 的吞吐。因此越来越多推理系统采用 **Prefill/Decode Disaggregation（P/D 分离）**：用不同实例或节点分别完成两个阶段。

但 P/D 分离会产生一个新问题：

> Prefill 在自己的 GPU 上计算出 KV Cache 后，Decode 节点怎样以足够低的延迟拿到这些 KV，并继续生成？

vLLM 通过统一的 `KVConnector` 接口回答“什么时候需要外部 KV、需要哪些 block、传输何时完成”；Mooncake 则提供真正的数据通路，使用 RDMA、GPUDirect RDMA、TCP 等协议在节点间传输 GPU KV Cache。

本文基于以下源码提交阅读：

- vLLM：`0ecc284790e5403f74b899524ef82ecb69f83cb3`，提交日期 2026 年 8 月 24 日；
- Mooncake：`bfca1ce2af8419c50dc8d464820a95d97d43c930`，提交日期 2026 年 8 月 24 日。

当前集成仍在快速迭代，本文以这些提交的实际实现为准。

# 一、先区分两种 Mooncake 对接模式

当前 vLLM 代码中与 Mooncake 相关的 Connector 不止一个，容易混淆。

## 1. MooncakeConnector：P/D 节点之间直接搬 KV

这是本文的主角。它用于一次请求的 Prefill 和 Decode 分别运行在不同 vLLM Engine 上的场景：

```text
Prefill GPU KV Cache
        |
Mooncake Transfer Engine
        |  RDMA / GPUDirect RDMA / TCP
        v
Decode GPU KV Cache
```

特点是：

- KV 在 Prefill 结束后直接传给指定 Decode 实例；
- 传输以请求和 KV block 为单位；
- Prefill 必须暂时保留 KV block，直到 Decode 确认接收完成；
- 重点是低延迟、点对点或少量节点间传输；
- vLLM 0.13.0 及以后已内置实现。

## 2. MooncakeStoreConnector：把 KV 放入共享存储层

Mooncake Store 更偏向一个分布式 KV Cache 池：

```text
vLLM Engine
    |
Mooncake Store
    |-- DRAM Cache
    |-- SSD Cache
    `-- Distributed Storage
```

它适用于：

- 多请求前缀复用；
- 跨实例共享 KV；
- KV Cache 分层与持久化；
- 从外部缓存查询已有前缀；
- 不是只服务一次 P/D handoff。

vLLM 还可以通过 `MultiConnector` 组合多个 Connector，例如同时使用 Mooncake 的 P/D 直接传输和外部 KV Store。

下面重点分析 `MooncakeConnector`。

# 二、部署形态与最小配置

典型部署包含三个角色：

```text
                  ┌──────────────────┐
Client ---------->│ Disagg Proxy     │
                  └──────┬─────┬─────┘
                         │     │
                  Prompt │     │ Full Request
                         v     v
               ┌────────────┐  ┌────────────┐
               │ Prefill    │  │ Decode     │
               │ Producer   │  │ Consumer   │
               └─────┬──────┘  └─────▲──────┘
                     │ Mooncake RDMA  │
                     └────────────────┘
```

Prefill 实例：

```bash
vllm serve Qwen/Qwen2.5-7B-Instruct \
  --port 8010 \
  --kv-transfer-config \
  '{"kv_connector":"MooncakeConnector","kv_role":"kv_producer"}'
```

Decode 实例：

```bash
vllm serve Qwen/Qwen2.5-7B-Instruct \
  --port 8020 \
  --kv-transfer-config \
  '{"kv_connector":"MooncakeConnector","kv_role":"kv_consumer"}'
```

代理：

```bash
python examples/disaggregated/mooncake_connector/mooncake_connector_proxy.py \
  --prefill http://prefill-host:8010 \
  --decode http://decode-host:8020
```

Mooncake Python 包需要与 vLLM 的 CUDA 主版本匹配：

- CUDA 13：`mooncake-transfer-engine-cuda13`；
- CUDA 12：`mooncake-transfer-engine`。

安装错误版本时，Python import 会因找不到对应 `libcudart.so` 失败。

# 三、双方的职责边界

理解源码前，先明确 vLLM 和 Mooncake 各做什么。

## vLLM 负责语义和生命周期

vLLM 知道：

- 哪个请求处于 Prefill 或 Decode；
- Prompt 有多少 token；
- KV Cache 分成哪些 block；
- 每层 KV tensor 的布局；
- Tensor Parallel（TP）和 Pipeline Parallel（PP）拓扑；
- 哪些 block 可以释放；
- 调度何时能继续执行模型。

因此 vLLM 负责：

- 计算远程命中的 token 数；
- 为 Decode 分配本地 KV block；
- 构造传输元数据；
- 管理请求状态和 block 生命周期；
- 在模型执行前启动接收；
- 等待必要的 KV 到达；
- 将传输完成反馈给 Scheduler。

## Mooncake 负责数据面

Mooncake Transfer Engine 知道：

- 本地注册了哪些内存区；
- 远端节点和网卡 endpoint；
- 如何通过 RDMA、GPUDirect RDMA、TCP 或其他 transport 搬运数据；
- 如何把多段地址组织为批量传输；
- 如何查询异步传输状态。

它不理解 token、attention layer 或请求调度，只接收：

```text
源 endpoint + 源地址 + 目标地址 + 长度
```

这种分层让 vLLM 不必自己实现 RDMA transport，Mooncake 也不必侵入模型执行和 KV block manager。

# 四、控制面：代理怎样拆分一次请求

`mooncake_connector_proxy.py` 展示了最小 P/D 控制面。

## 1. 生成统一 transfer_id

代理为每个用户请求生成 UUID，并构造：

```text
transfer_id = "xfer-" + request_id
```

Prefill 和 Decode 两边必须使用相同 `transfer_id`，Mooncake Connector 才能把两阶段识别为同一次 KV handoff。

## 2. 先调用 Prefill

代理发给 Prefill 的请求带有：

```json
{
  "kv_transfer_params": {
    "do_remote_decode": true,
    "do_remote_prefill": false,
    "transfer_id": "xfer-..."
  }
}
```

含义是：

- 当前实例负责计算 Prompt；
- 计算结果将由远端 Decode 消费；
- Prefill 不负责正常持续生成。

通常 Prefill 只运行极少 token，用于完成 KV 生成并触发请求结束逻辑。

## 3. 再调用 Decode

Prefill 请求结束后，代理向 Decode 发送原始请求，并附加：

```json
{
  "kv_transfer_params": {
    "do_remote_decode": false,
    "do_remote_prefill": true,
    "remote_bootstrap_addr": "prefill-host:8998",
    "remote_engine_id": "prefill-engine-id",
    "transfer_id": "xfer-..."
  }
}
```

Decode 由此知道：

- Prompt KV 已在远端计算；
- 应从哪个 Prefill Engine 获取；
- 去哪个 bootstrap server 查询 worker endpoint；
- 本次传输如何与 Prefill 端匹配。

代理本身不传 KV，也不直接操作 RDMA。它只完成请求拆分与控制元数据拼接。

# 五、为什么需要 Bootstrap Server

vLLM Engine 可能使用：

- Data Parallel；
- Tensor Parallel；
- Pipeline Parallel；
- 一台主机上的多块网卡；
- 多个 Prefill 实例。

Decode 不能只知道一个 HTTP 地址，它需要发现真正持有 KV Cache 的每个 Prefill worker 及其 Mooncake endpoint。

Prefill 端因此启动 `MooncakeBootstrapServer`，默认端口为：

```text
VLLM_MOONCAKE_BOOTSTRAP_PORT=8998
```

每个 worker 初始化 Transfer Engine 后，将以下信息注册给 bootstrap：

- Engine ID；
- Data Parallel rank；
- Tensor Parallel rank；
- Pipeline Parallel rank；
- Mooncake server/segment 名称；
- 可用 transport endpoint；
- 必要的拓扑信息。

Decode 收到新的 `remote_engine_id` 时，会查询 Prefill bootstrap，把远端 Engine 展开为：

```text
remote_engine_id
  -> TP rank
     -> PP rank
        -> Mooncake endpoint
```

之后的数据面不再经过 bootstrap。它只承担服务发现和连接建立，不搬运 KV。

# 六、Scheduler 侧：把远程 KV 当作“外部命中”

`MooncakeConnector` 在 vLLM 中分成 Scheduler 和 Worker 两部分。

Scheduler 侧不接触 GPU 地址，只回答调度问题。

## 1. `get_num_new_matched_tokens()`

Decode 请求携带 `do_remote_prefill=true` 时，Scheduler 把远端 Prefill 产生的 Prompt KV 视为外部缓存命中。

它根据 Prompt token 数和当前本地已计算 token 数，返回：

```text
外部可加载 token 数
是否异步加载
```

这会影响 vLLM 的 token budget：Decode 不必重新执行完整 Prompt，只需为将要导入的 KV 分配 block，然后从合适位置继续生成。

对于普通 Transformer，通常整个 Prompt 都可从远端加载。

对于带 Mamba/GDN 状态的模型，源码会保留最后一个 token 在 Decode 端重算。原因是 Decode 需要从 `h(N-1)` 推导正确的 `h(N)`，不能简单把所有状态当作标准 attention KV 搬运。

## 2. `update_state_after_alloc()`

当 vLLM KV Cache Manager 为请求分配完本地 block 后，Connector 记录：

- 请求 ID；
- `transfer_id`；
- 远端 Engine ID；
- bootstrap 地址；
- Decode 本地目标 block IDs；
- 每个 KV Cache group 的 block 分布。

这些信息稍后被打包进 `MooncakeConnectorMetadata`，随 `SchedulerOutput` 发送给 Worker。

Prefill 侧也在这里记录“该请求完成后需要发送 KV”，但此时 block 可能仍在计算，暂不能发。

## 3. `build_connector_meta()`

每个调度 step 结束前，Scheduler 将待发送、待接收和未处理请求构造为 metadata：

```text
reqs_to_recv
reqs_to_send
reqs_not_processed
```

Worker 不需要重新理解 Request 对象，只按 metadata 启动后台传输。

# 七、Worker 初始化：把 vLLM KV Tensor 注册给 Mooncake

Mooncake 要执行 RDMA，必须先注册本地 GPU 内存。

## 1. 创建 Transfer Engine

Worker 初始化时导入：

```python
from mooncake.engine import TransferEngine
```

随后以本机 endpoint、metadata/handshake 模式和 transport protocol 初始化 Engine。默认 protocol 是 `rdma`，也可通过 `kv_connector_extra_config` 配置。

常用配置包括：

```json
{
  "kv_connector": "MooncakeConnector",
  "kv_role": "kv_producer",
  "kv_connector_extra_config": {
    "num_workers": 10,
    "mooncake_protocol": "rdma",
    "device_name": "mlx5_0,mlx5_1"
  }
}
```

`device_name` 可以限制参与拓扑发现的 RDMA 网卡，避免双方选择不同 link layer 或错误端口。

## 2. `register_kv_caches()`

模型 Runner 建立 KV Cache tensor 后，Connector 遍历每一层或每个 KV Cache group，提取：

- Tensor `data_ptr()`；
- 总字节数；
- 每个 block 的字节长度；
- attention layer 名称和 layer index；
- KV Cache group；
- tensor 是否 dense/contiguous；
- 实际 kernel block layout。

然后调用 Mooncake：

```text
TransferEngine.register_memory(base_addr, total_size)
```

注册完成后，RDMA NIC 才能直接访问 GPU KV Cache 区域。

## 3. `WITH_NVIDIA_PEERMEM`

Mooncake 注册 GPU 内存有两条常见路径：

- `WITH_NVIDIA_PEERMEM=1`：通过 `ibv_reg_mr()`，要求加载 `nvidia-peermem`；
- `WITH_NVIDIA_PEERMEM=0`：使用 DMA-BUF 路径，不依赖 `nvidia-peermem`，GB200 等环境可能需要此模式。

如果环境没有加载 `nvidia-peermem` 却保持默认配置，典型错误是：

```text
Failed to register memory <addr>: Bad address [14]
```

这不是 vLLM block 分配错误，而是 RDMA Memory Region 注册路径不匹配。

# 八、数据面：Decode 主动从 Prefill 拉取 KV

当前 `MooncakeConnector` 的关键模式是：**Decode/Consumer 发起远程读取，Prefill/Producer 提供源地址。**

完整时序如下：

```text
Proxy          Prefill Scheduler/Worker       Decode Scheduler/Worker
  |                       |                              |
  |-- prefill request --->|                              |
  |                       |-- compute prompt KV          |
  |                       |-- retain KV blocks           |
  |<-- prefill done ------|                              |
  |                                                      |
  |---------------- decode request --------------------->|
  |                                                      |-- allocate local blocks
  |                                                      |-- query bootstrap
  |                                                      |-- send xfer metadata
  |                       |<--- ask source block addrs ---|
  |                       |--- source addrs/endpoints --->|
  |                                                      |-- RDMA READ KV
  |                                                      |-- mark recv finished
  |                       |<------ completion ack --------|
  |                       |-- release source blocks       |
  |                                                      |-- start/continue decode
```

## 1. Decode 向每个 Prefill worker 请求源地址

Decode 已知道自己的目标 block IDs，但还不知道 Prefill 的源 block IDs 和具体 GPU 地址。

它通过 Connector side channel 向 Prefill worker 发送 `MooncakeXferMetadata`，其中包含：

- `transfer_id`；
- Decode endpoint；
- 目标 block IDs；
- TP/PP rank 与 KV layout；
- 异构并行需要的 region 信息。

Prefill worker 根据 `transfer_id` 找到被保留的源 block，并构造源地址列表。

## 2. 地址不是逐 token 传，而是逐 region/block 计算

vLLM KV Cache 通常是大 tensor，每个逻辑 block 对应固定字节跨度：

```text
block_addr = base_addr + block_id * block_len
```

Connector 先把每层 KV tensor 展开成 `TransferRegion`：

```text
layer_name
layer_index
base_addr
block_len
kv_block_len
group_index
```

随后将源 block 和目标 block 对齐，生成批量 copy entry。

相邻且地址连续的 block 会被合并为更大的传输，减少 RDMA Work Request 数量和 Python/C++ 调用开销。

## 3. Mooncake 执行批量远程读

最终数据面调用类似：

```text
batch_transfer_sync_read(
  remote_segment,
  local_addresses,
  remote_addresses,
  lengths
)
```

源数据位于 Prefill GPU，目标位于 Decode GPU。配置 GPUDirect RDMA 时，数据可由 NIC 直接在两端 GPU 显存之间搬运，不经过 CPU 数据拷贝。

CPU 仍参与控制面、地址规划和完成处理，但不作为 KV payload 的 bounce buffer。

# 九、为什么 Prefill 不能计算完就释放 KV block

在同机推理中，请求结束后 block 可以立即归还 KV Cache Manager。但 P/D 分离中，Prefill 请求结束只表示 KV 已计算完，不代表 Decode 已经读完。

因此 `request_finished()` 会改变释放语义：

1. Prefill 将请求对应的 block IDs 记录到 `reqs_need_send`；
2. block 被标记为仍由 Connector 使用；
3. sender thread 等待 Decode 提供传输 metadata；
4. Decode 完成 RDMA read；
5. 完成消息回到 Prefill；
6. Connector 将请求放入 `finished_sending`；
7. Scheduler 最终释放这些 block。

这是一种跨节点引用计数协议。若没有它，Prefill block 可能在 Decode 读取期间被新请求复用，造成静默数据破坏。

# 十、完成通知、超时与请求中止

分布式推理的困难往往不是 happy path，而是请求中止和节点故障。

## 1. Prefill block 超时回收

环境变量：

```text
VLLM_MOONCAKE_ABORT_REQUEST_TIMEOUT=480
```

控制 Prefill 最长保留某请求 KV block 的时间。

如果客户端中止、代理失败或 Decode 永远没有发来请求，Prefill 不能无限持有 block。超时后 Connector 自动释放，防止 KV Cache 容量永久泄漏。

## 2. Decode 接收完成

Worker 周期性获取：

```text
finished_recving
finished_sending
```

并通过 `KVConnectorOutput` 返回 Scheduler。Scheduler 只有在收到完成状态后，才允许依赖这些 KV 的请求正常推进或释放相应资源。

## 3. 传输失败

Connector 记录失败 block，并通过：

```text
invalid_block_ids
```

传回模型 Runner/Scheduler。这样 vLLM 可以避免把部分写入或失败的 KV block 当成有效缓存继续使用。

## 4. Proxy 的职责有限

示例 Proxy 主要展示基本请求编排，并不是完整生产级控制面。生产部署还需要：

- Prefill/Decode 负载感知路由；
- transfer_id 全局唯一性；
- 请求取消传播；
- 节点故障重试；
- Prefill 成功但 Decode 失败时的清理；
- 跨可用区网络策略；
- 限流和背压；
- 端到端 trace。

# 十一、TP/PP 不一致时怎样搬 KV

生产环境中 Prefill 和 Decode 不一定采用相同并行度。例如：

```text
Prefill: TP=8，追求吞吐
Decode:  TP=4，追求单请求效率
```

当前 Connector 包含异构 TP 传输规划。

## 1. 本地 TP 大于远端 TP

一个 Decode rank 可能需要接收多个 Prefill shard，或一个 Prefill shard 的数据需要写入 Decode 更大的 KV region。

Connector 计算 TP ratio，并为每个 rank 生成源/目标 region offset。

## 2. 本地 TP 小于远端 TP

一个本地 rank 可能需要从多个远端 rank gather 数据。Connector 按远端 TP rank 选择多个 endpoint，并把不同 shard 写入目标 KV block 的不同偏移。

## 3. KV 是否复制会改变规划

某些 attention backend 或并行拓扑会复制 KV Cache，而不是严格分片。Connector 通过 `TransferTopology` 判断 producer cache 是否 replicated，避免重复传输或错误拼接。

## 4. PP 维度

每个 PP rank 只持有部分 layer。Bootstrap 保存 TP rank 到 PP rank endpoint 的映射；Decode 按本地 layer 对应的远端 PP rank拉取。

如果两侧 PP 划分不同，Connector 会根据 layer index 和 region metadata 对齐，而不是假设“同 rank 即同层”。

# 十二、Hybrid KV Cache 和特殊模型布局

vLLM 已支持多种 KV Cache spec：

- Full Attention；
- Sliding Window Attention；
- Mamba/GDN 状态；
- Hybrid KV Cache Manager；
- 不同 attention backend 的物理布局。

Mooncake Connector 因此不能简单假设所有层都有相同 block 大小。

## Sliding Window

只需传输窗口内仍有效的 block。Scheduler 会按每个 KV Cache group 的 sliding window 截断 block IDs，避免搬运已经不会再被 attention 使用的历史 KV。

## Hybrid KV Cache

不同 group 可能拥有不同 block size 和布局。Metadata 按 group 保存 block list，Worker 再展开为独立 `TransferRegion`。

## Kernel Block Size

逻辑 block 与 attention kernel 实际使用的 block 可能不完全一致。Connector 初始化时会同步 kernel block size，并将逻辑 block IDs 转换为 kernel block IDs，确保地址步幅正确。

这也是 Mooncake 集成放进 vLLM 主仓库的重要原因：它需要紧跟 KV Cache layout、attention backend 和 Scheduler 内部变化，单纯的外部插件很难长期稳定适配。

# 十三、从 OOT Connector 到 vLLM 内置实现

Mooncake 仓库仍保留 `mooncake_connector_v1.py`，用于兼容 vLLM 0.10.1–0.12.0。

该模块明确提示：

- vLLM 0.13.0 之前使用 Mooncake wheel 中的 Out-of-Tree Connector；
- vLLM 0.13.0 及以后应使用 vLLM 内置 `MooncakeConnector`。

这个迁移有现实原因。

Connector 深度依赖：

- `SchedulerOutput`；
- `Request` 状态；
- KV Cache group/spec；
- attention backend；
- block table；
- TP/PP 拓扑；
- Model Runner 生命周期。

这些内部接口随 vLLM 快速演进。内置实现可以与 KV Cache Manager、Scheduler 和测试一起修改，降低版本漂移成本；Mooncake wheel 则专注稳定的数据传输 API。

# 十四、性能取决于哪些因素

Mooncake 提供零拷贝或低拷贝通路，不代表任何部署都会自动变快。

## 1. Prompt 长度

Prompt 很短时，远程传输和两次 HTTP 调度的固定成本可能高于重算。P/D 分离更适合长 Prompt 或可批量化 Prefill。

## 2. KV 大小

KV 字节数大致随以下因素增长：

```text
层数 × KV heads × head dimension × token 数 × dtype bytes
```

GQA/MQA、量化 KV、MLA 会显著改变传输量。

## 3. 网络拓扑

需要关注：

- GPU 与 NIC 是否位于同一 NUMA/PCIe Root；
- 是否启用 GPUDirect RDMA；
- RoCE PFC/ECN 配置；
- InfiniBand 路由；
- 多 NIC 是否被正确选择；
- PCIe ACS/IOMMU；
- `nvidia-peermem` 或 DMA-BUF 支持。

## 4. Block 合并率

连续 block 越多，Connector 越能合并传输。高度碎片化的 block table 会增加 RDMA WR 数量和控制开销。

## 5. Prefill 与 Decode 的资源配比

P/D 分离最终是排队系统问题。Prefill 太少会让请求等待 Prompt 计算；Decode 太少会让 KV 已传到但无法及时生成。Proxy 需要根据 Prompt 长度、输出长度和当前队列动态路由。

# 十五、一次完整请求的源码调用链

把所有步骤串起来：

```text
Client
  |
  v
mooncake_connector_proxy.py
  |-- 生成 request_id / transfer_id
  |-- 请求 Prefill: do_remote_decode=true
  |
  v
Prefill Scheduler
  |-- 正常分配 KV blocks
  |-- update_state_after_alloc(): 记录待发送请求
  |
  v
Prefill Model Runner
  |-- 计算 Prompt KV
  |-- KV tensor 已 register_memory 到 Mooncake
  |
  v
Prefill request_finished()
  |-- 不立即释放 block
  `-- 保存 transfer_id -> source block IDs

Proxy
  |-- 请求 Decode: do_remote_prefill=true
  |                  remote_engine_id
  |                  remote_bootstrap_addr
  v
Decode Scheduler
  |-- get_num_new_matched_tokens(): Prompt 外部命中
  |-- 为外部 KV 分配本地 blocks
  |-- build_connector_meta(): reqs_to_recv
  v
Decode Worker
  |-- start_load_kv()
  |-- 查询 Prefill bootstrap
  |-- 获取 TP/PP worker endpoints
  |-- 向 Prefill side channel 请求源地址
  |-- 对齐 source/destination TransferRegion
  |-- 合并连续 blocks
  `-- Mooncake batch remote read
          |
          `-- RDMA/GDR: Prefill GPU -> Decode GPU

Decode Worker
  |-- finished_recving
  |-- KV 可被 attention kernel 使用
  `-- 开始/继续 Decode

Prefill Worker
  |-- 收到完成通知
  |-- finished_sending
  `-- Scheduler 释放源 KV blocks
```

# 十六、设计评价

## 优点

### vLLM 保留调度主导权

Mooncake 不接管请求和 block manager。它只实现 transport，避免把模型语义耦合进网络层。

### 数据面可以真正绕过 CPU payload copy

使用 GPUDirect RDMA 时，KV 从 Prefill GPU 直接进入 Decode GPU。CPU 负责控制，不承载 payload。

### 与 vLLM 多种 KV 布局共同演进

内置 Connector 能处理 Hybrid KV、Sliding Window、TP/PP 和特殊模型状态，而不只是固定 shape 的 tensor copy。

### 生命周期协议完整

Prefill block 保留、完成确认、超时释放和失败 block 标记，解决了分布式传输最容易被忽略的资源安全问题。

## 代价

### 控制面复杂

一次请求经历两次推理服务调用、bootstrap 查询、side channel 握手和 RDMA 传输。生产 Proxy 远比示例复杂。

### 基础设施要求高

GPUDirect RDMA 依赖网卡、驱动、PCIe 拓扑和网络配置。任何一层不匹配都可能退化或失败。

### 远程传输不是免费的

对于短 Prompt、小模型或低速网络，重算可能比搬 KV 更快。系统需要成本模型决定是否 P/D 分离。

### 版本耦合仍然存在

虽然 Connector 已内置 vLLM，但 Mooncake Transfer Engine、CUDA、RDMA 驱动和 vLLM 版本仍需兼容验证。

# 总结

vLLM 与 Mooncake 的对接并不是简单地“在 vLLM 中调用一个 RDMA copy API”。它是一套跨控制面、调度器、模型执行器和传输引擎的协议：

- Proxy 用统一 `transfer_id` 把 Prefill 和 Decode 请求关联起来；
- vLLM Scheduler 把远程 Prompt KV 建模为外部 cache hit；
- Decode 先分配本地 KV block，再构造传输 metadata；
- Prefill bootstrap 暴露真正持有 KV 的 TP/PP worker endpoint；
- Worker 将 vLLM KV tensor 注册给 Mooncake Transfer Engine；
- Decode 通过 RDMA/GDR 从 Prefill GPU 批量读取 block；
- 完成协议保证 Prefill 只在远端读完后释放 KV；
- 超时和失败路径防止 block 泄漏与错误 KV 被继续使用。

二者的职责划分非常清晰：

```text
vLLM 决定搬什么、何时搬、搬到哪个 block；
Mooncake 决定通过哪张网卡和哪种 transport 把字节搬过去。
```

这也是该集成能够支持复杂 KV Cache layout 和异构并行的根本原因。对于准备部署 P/D 分离的团队，真正需要评估的不只是 RDMA 带宽，还包括请求路由、Prompt 长度分布、KV block 碎片、TP/PP 组合、失败回收和网络拓扑。数据通路只是系统的一半，另一半是如何让 KV 的生命周期与 vLLM 调度状态始终一致。

# 参考资料

- [vLLM MooncakeConnector Usage Guide](https://docs.vllm.ai/en/latest/features/mooncake_connector_usage/)
- [vLLM MooncakeConnector 源码](https://github.com/vllm-project/vllm/tree/main/vllm/distributed/kv_transfer/kv_connector/v1/mooncake)
- [vLLM P/D 分离示例](https://github.com/vllm-project/vllm/tree/main/examples/disaggregated/mooncake_connector)
- [Mooncake 项目](https://github.com/kvcache-ai/Mooncake)
- [Mooncake 文档](https://kvcache-ai.github.io/Mooncake/)
