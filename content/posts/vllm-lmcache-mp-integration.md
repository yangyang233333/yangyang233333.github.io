---
title: "vLLM 与 LMCache MP 模式如何对接：从调度器到缓存服务的数据链路"
date: 2026-09-06T18:50:00+08:00
draft: false
tags: ["vLLM", "LMCache", "KV Cache", "多进程", "大模型推理"]
categories: ["大模型推理"]
summary: "从 LMCacheMPConnector 源码出发，分析 vLLM 调度器、GPU Worker、LMCache MP Server、L1/L2 存储之间的控制面和数据面，并分别梳理 STORE、LOOKUP、RETRIEVE 的完整时序。"
---

LMCache MP 模式把缓存系统从 vLLM 进程中拆成独立服务。vLLM 仍负责模型执行、Paged KV Cache 和请求调度；LMCache Server 负责 token 哈希、外部缓存查询、CPU L1、远端 L2、淘汰和预取。两者通过 `LMCacheMPConnector` 与消息队列协议连接。

理解这套架构最重要的是区分两条链路：

- **控制面**：请求 token、GPU block ID、命中长度、STORE/RETRIEVE 指令和完成通知；
- **数据面**：真正的 KV Tensor 在 GPU、CPU L1 和 L2 之间搬运。

控制面主要通过 ZMQ Request/Response 完成；数据面则根据设备和配置选择 CUDA IPC 驱动的 `lmcache_driven`，或者由推理引擎主动 gather/scatter 的 `engine_driven`。

## 一、整体架构

```text
┌────────────────────────── vLLM ──────────────────────────┐
│                                                         │
│  Scheduler                                               │
│  ├── Request                                             │
│  ├── GPU block allocator                                 │
│  └── LMCacheMPConnector（scheduler role）                 │
│          │  lookup / metadata / completion               │
│          │                                               │
│  GPU Worker                                              │
│  ├── Model Runner                                        │
│  ├── Paged KV Cache                                      │
│  └── LMCacheMPConnector（worker role）                    │
│          └── LMCacheMPWorkerAdapter                      │
└───────────────────────┬─────────────────────────────────┘
                        │ ZMQ 控制协议
                        │ CUDA IPC / SHM / Pickle 数据路径
                        ▼
┌──────────────────── LMCache MP Server ───────────────────┐
│ MessageQueueServer                                       │
│        ↓ 按 RequestType 分发                              │
│ MPCacheServer                                            │
│ ├── LookupModule                                         │
│ ├── LMCacheDrivenTransferModule                          │
│ ├── EngineDrivenTransferModule                           │
│ ├── ManagementModule                                     │
│ └── MPCacheServerContext                                 │
│      ├── TokenHasher / SessionManager                    │
│      ├── LayoutDescRegistry                              │
│      └── distributed.StorageManager                      │
│           ├── L1Manager → CPU DRAM / DAX / GDS           │
│           ├── StoreController → L2 Adapter               │
│           ├── PrefetchController ← L2 Adapter            │
│           └── EvictionController                         │
└──────────────────────────────────────────────────────────┘
```

MP 模式的收益不只是“多了一个进程”。LMCache 的哈希、内存管理、L2 I/O 和淘汰不再与 vLLM 的推理线程竞争同一个 Python GIL；多个 vLLM 实例还可以共享同一节点上的 L1 缓存。

## 二、vLLM Connector 在哪里

入口类是：

```python
class LMCacheMPConnector(KVConnectorBase_V1, SupportsHMA):
    ...
```

它同时运行在 vLLM 的 scheduler 侧和 worker 侧，但职责不同。

| 位置 | 主要职责 |
| --- | --- |
| Scheduler 侧 | 查询 LMCache 命中、跟踪请求状态、生成 STORE/RETRIEVE metadata、处理完成通知 |
| Worker 侧 | 注册 GPU KV Tensor、执行 STORE/RETRIEVE 数据搬运、等待异步操作完成 |

Connector 通过 vLLM 的 `KVConnectorBase_V1` 生命周期接入调度和模型执行过程。关键回调包括：

```text
Scheduler：
get_num_new_matched_tokens
on_new_request
update_state_after_alloc
build_connector_meta
update_connector_output
request_finished

Worker：
register_kv_caches
start_load_kv
wait_for_layer_load
wait_for_save
get_finished
```

## 三、初始化：先确定缓存几何

Connector 初始化时首先从 vLLM 的 `KVCacheConfig` 中提取缓存几何：

```python
group_tokens_per_block = get_group_tokens_per_block(...)
scheduler_block_size = get_vllm_scheduler_block_size(...)
cache_model_name = get_dcp_decorated_model_name(...)
```

这里不能简单假设所有模型只有一个标准 KV group。混合模型可能包含：

- Full Attention KV；
- Sliding Window KV；
- MLA；
- Mamba/recurrent state；
- Circular Buffer；
- 多个 shape 或 tokens-per-block 不同的 object group。

`group_tokens_per_block` 告诉 LMCache：每个 group 的一个 vLLM block 对应多少逻辑 token。后续 STORE/RETRIEVE 都要根据它把 token 区间转换成各 group 的 block ID 切片。

Connector 还会解析 LMCache Server 地址：

```text
lmcache.mp.server_urls
```

或者兼容单服务器配置：

```text
lmcache.mp.host + lmcache.mp.port
```

每个地址会创建一个 `RequestClient`。当前默认传输是 ZMQ TCP；工厂层为其他协议保留了扩展点。

## 四、注册阶段：让 Server 看见 GPU KV Cache

模型初始化后，vLLM 调用：

```python
register_kv_caches(kv_caches)
```

其中 `kv_caches` 是：

```text
layer name → GPU KV Tensor
```

Connector 会先根据 vLLM layout hints 调整 Tensor view，再生成 `EngineGroupInfo`：

```python
engine_group_infos = create_engine_group_infos_from_vllm(...)
```

这些信息包含：

- 每个 cache group 对应哪些层；
- Tensor shape 和 dtype；
- tokens per block；
- vLLM block layout；
- TP/DCP 等并行信息。

然后 worker adapter 发出注册请求。LMCache MP 协议中的 `REGISTER_KV_CACHE` 包含：

```text
instance_id
KVCache 描述或 IPC handle
model_name
world_size
engine_type
layout_hints
engine_group_infos
```

Server 收到后建立两类状态：

1. **实例与传输上下文**：后续根据 `instance_id` 找到对应 GPU Worker；
2. **LayoutDescRegistry**：根据 `(model_name, world_size)` 找到 LMCache 对象布局。

没有注册就无法正确计算每个 LMCache chunk 需要多大的 L1 对象，也无法在 lookup 后把数据放回正确的 GPU block。

## 五、请求跟踪器：连接 vLLM 调度状态与 LMCache 状态

Scheduler 侧为每个请求维护 `LMCacheMPRequestTracker`。核心字段包括：

```text
all_token_ids
allocated_block_ids[group]
num_scheduled_tokens
num_stored_tokens
num_vllm_hit_tokens
num_lmcache_hit_tokens
state
cache_salt
```

它的状态机可以简化为：

```text
PREFETCHING
    │ 完成 lookup，等待 GPU block 分配
    ▼
WAITING_FOR_LOAD
    │ 生成并下发 RETRIEVE metadata
    ▼
READY
    │ 加载失败
    ▼
BYPASS_LMCACHE
```

这里有三个容易混淆的 token 计数：

- `num_vllm_hit_tokens`：vLLM 自己的 GPU Prefix Cache 已命中的前缀；
- `num_lmcache_hit_tokens`：LMCache 外部缓存命中的前缀；
- `num_stored_tokens`：已经确认属于 LMCache 存储范围的 token 数。

它们不能简单相加，因为 vLLM hit 和 LMCache hit 通常是同一请求前缀的重叠范围。

## 六、LOOKUP 链路

当新请求进入 vLLM，Scheduler Connector 会调用 LMCache lookup。若开启 eager prefetch，可以在请求仍处于 waiting queue 时提前发起。

完整控制链路如下：

```text
vLLM Request
    │ token_ids + model_name + request_id + cache_salt
    ▼
LMCacheMPSchedulerAdapter.lookup
    │ ZMQ LOOKUP
    ▼
MessageQueueServer
    ▼
LookupModule.lookup
    │
    ├── LayoutDescRegistry 查找模型布局
    ├── TokenHasher 将 token 序列切成 LMCache chunk hash
    └── StorageManager.prefetch
            ├── 查询 L1
            └── 必要时从 L2 异步预取到 L1
```

`LOOKUP` 本身是提交任务，不一定同步等待所有 L2 读取完成。Server 会按 `request_id` 注册一个 prefetch job。Scheduler 随后使用：

```text
QUERY_PREFETCH_STATUS
WAIT_PREFETCH_STATUS
QUERY_PREFETCH_LOOKUP_HITS
```

查询结果。

当命中完成后，Scheduler 得到命中的 chunk 数，再换算成 token 数：

```text
LMCache hit tokens = hit chunks × LMCache chunk size
```

为了保证 Prefix Cache 语义，命中必须是从请求开头连续的前缀。中间缺块之后的缓存即使存在，也不能直接作为普通 prefix hit 使用。

## 七、为什么 lookup 与 retrieve 分开

Lookup 时 vLLM 还不一定已经为该请求分配 GPU block。LMCache 此时只能确认“哪些 chunk 在 L1/L2 中存在”，不能直接把数据写入 GPU Paged KV Cache。

因此流程被拆为：

```text
LOOKUP：确定外部缓存命中，并把 L2 数据预取到 L1
ALLOC：vLLM Scheduler 为请求分配 GPU block
RETRIEVE：根据已分配 block ID 把 L1 数据写回 GPU
```

这也是 `LMCacheMPRequestTracker` 需要同时跟踪 token 命中和 block 分配的原因。

## 八、Scheduler 如何生成 RETRIEVE metadata

vLLM 分配 GPU block 后调用：

```python
update_state_after_alloc(...)
```

Connector 把各 group 的 block IDs 追加到 tracker。如果 LMCache 命中大于 vLLM 自身命中：

```python
num_lmcache_hit_tokens > num_vllm_hit_tokens
```

请求就需要 retrieve。

随后 `build_connector_meta()` 调用：

```python
LMCacheMPRequestMetadata.GetRetrieveMetadata(...)
```

生成：

```python
LoadStoreOp(
    token_ids=...,
    block_ids=...,
    start=...,
    end=...,
    skip_first_n_tokens=...,
)
```

含义是：

- `token_ids`：该操作覆盖的完整 token 序列；
- `start/end`：实际需要加载的 token 区间；
- `block_ids`：每个 cache group 对应的 vLLM GPU block；
- `skip_first_n_tokens`：避免覆盖 vLLM APC 已共享的前缀位置。

`skip_first_n_tokens` 很重要。LMCache 使用独立 CUDA stream 写入 GPU，而 vLLM 的其他请求可能正在读取 APC 共享 block；跳过重叠部分可以避免跨 stream 数据竞争。

## 九、Worker 如何执行 RETRIEVE

Scheduler 生成的 `LMCacheMPConnectorMetadata` 会随调度输出传到 Worker。模型 forward 前，vLLM 调用：

```python
start_load_kv(forward_context)
```

Connector 收集所有 `direction == "RETRIEVE"` 的操作，记录一个 device event，然后调用：

```python
worker_adapter.batched_submit_retrieve_requests(...)
```

接下来数据路径取决于 transfer mode。

### lmcache_driven

```text
vLLM Worker
    │ RETRIEVE + GPU block IDs + event handle
    ▼
LMCache Server
    │ 等待 vLLM stream 到达安全点
    │ 从 L1 读取 MemoryObj
    │ 通过已注册的 CUDA IPC 映射访问 Worker GPU KV Cache
    │ 在 LMCache CUDA stream 上执行 CPU → GPU copy
    ▼
vLLM Paged KV Cache
```

这里 LMCache Server 主动访问 Worker 的 GPU 内存，因此称为 `lmcache_driven`。

### engine_driven

```text
vLLM Worker                         LMCache Server
    │ PREPARE_RETRIEVE                  │
    ├──────────────────────────────────>│ 从 L1 取出 CPU chunks
    │<──────────────────────────────────┤ 返回 Pickle 数据或 SHM slots
    │ scatter CPU chunks → GPU blocks   │
    │ COMMIT_RETRIEVE                   │
    ├──────────────────────────────────>│ 解锁/完成 session
```

这里 GPU scatter 由 vLLM Worker 执行，所以称为 `engine_driven`。它不依赖 CUDA IPC，可用于 XPU、HPU、CPU 等设备。

当前异步 engine-driven 主要优化 STORE；RETRIEVE 通常仍按 prepare、scatter、commit 顺序完成。

## 十、STORE metadata 是何时生成的

STORE 不能在请求刚到达时生成，因为那时 KV 尚未计算。Scheduler Connector 在每个调度 step 处理中更新：

```text
已分配 block IDs
本 step 新调度 token 数
累计 num_scheduled_tokens
```

然后调用：

```python
LMCacheMPRequestMetadata.GetStoreMetadata(...)
```

它计算三个上限：

```text
请求已有 token 数
GPU block 实际覆盖的 token 数
已经完成或即将完成本 step 计算的 token 数
```

取最小值，再减去 `num_stored_tokens`，最后按 LMCache chunk size 向下取整。只有形成至少一个完整 chunk，才会生成 STORE metadata。

```text
num_chunks = staging_tokens // lmcache_tokens_per_chunk
```

因此 LMCache 默认不会保存不足一个 chunk 的尾部。

混合 cache group 场景还要保证各 group 都有足够的 block 覆盖目标 token 区间。若某种 group 的“物理容量”不等于线性 token 覆盖范围，就不能直接使用普通计算方式，这正是 CircularBuffer 类模型容易出现兼容问题的地方。

## 十一、Worker 如何执行 STORE

模型完成 forward 后，vLLM 调用：

```python
wait_for_save()
```

Connector 从 metadata 中收集 `direction == "STORE"` 的操作，记录当前模型计算 stream 的 event，并提交：

```python
worker_adapter.batched_submit_store_requests(...)
```

### lmcache_driven STORE

```text
vLLM forward 完成
    │ 记录 CUDA event
    ▼
LMCache Server 收到 STORE
    │ 等待 event，确保 KV 已写完
    │ 通过 CUDA IPC 打开 Worker KV Tensor
    │ 根据 block IDs gather 对应 KV
    │ 写入 L1 MemoryObj
    │ StoreController 异步推送到 L2
    ▼
返回完成状态
```

LMCache Server 掌握 GPU Tensor IPC handle，因此数据不需要通过 ZMQ 序列化。

### engine_driven STORE

```text
vLLM Worker                         LMCache Server
    │ PREPARE_STORE                     │
    ├──────────────────────────────────>│ 在 L1 预留对象/SHM slot
    │ gather GPU blocks → CPU chunks    │
    │ 写 SHM 或序列化 Pickle            │
    │ COMMIT_STORE                      │
    ├──────────────────────────────────>│ 接收并写入 L1
    │                                   │ StoreController → L2
```

若设备支持独立 stream、event 和 pinned memory，可以使用 `AsyncEngineDrivenTransferContext`，把 prepare、gather、commit 放入后台线程池，降低对模型执行线程的阻塞。

## 十二、控制面与数据面分别传什么

| 路径 | 主要内容 |
| --- | --- |
| Scheduler → Server LOOKUP | token IDs、模型名、world size、request ID、cache salt |
| Scheduler → Worker metadata | STORE/RETRIEVE 方向、token 区间、各 group block IDs |
| Worker → Server 注册 | Tensor layout、IPC handle 或 engine-driven context、instance ID |
| lmcache-driven 数据面 | Server 通过 CUDA IPC 直接访问 Worker GPU KV |
| engine-driven 数据面 | Worker gather/scatter，数据通过 SHM 或 Pickle 交换 |
| Worker → Scheduler completion | 每个 request 的 STORE 完成计数 |

ZMQ 主要承载小型控制消息。大块 KV 数据在 lmcache-driven 模式下走 CUDA IPC，在 engine-driven 模式下走共享内存或序列化数据，不应把“使用 ZMQ”理解为所有 KV Tensor 都通过 ZMQ socket 传输。

## 十三、L1 与 L2 在链路中的位置

Server 的 `distributed.StorageManager` 把缓存分成：

```text
L1：本节点热缓存
    CPU pinned DRAM / Device-DAX / GDS slab

L2：外部或持久化缓存
    NIXL / POSIX / S3 / Redis / Valkey / 其他 adapter
```

STORE 通常先进入 L1：

```text
GPU → L1 → StoreController → L2
```

LOOKUP/RETRIEVE 则可能是：

```text
查询 L1
  ├── 命中：直接准备 retrieve
  └── 未命中：PrefetchController 从 L2 拉回 L1
                                  ↓
                            L1 → GPU retrieve
```

所以 L2 不直接对应 vLLM GPU block。LMCache 会先把外部对象恢复到统一的 L1 `MemoryObj`，再通过 transfer context 搬回 GPU。

## 十四、完成通知与 block 生命周期

STORE 通常异步完成，但 vLLM 不能过早复用或释放仍在读取的 GPU block。

Worker 侧会维护 store futures。完成后生成 `LMCacheMPWorkerMetadata`：

```python
completed_store_requests: dict[str, int]
```

TP 多 Worker 场景中，每个 Worker 对请求贡献一次完成计数。Scheduler 聚合各 Worker metadata，只有计数达到 `world_size`，才把该请求视为完整 STORE 完成。

这条反向链路是：

```text
LMCache Server 完成 STORE
    ↓
Worker future 完成
    ↓
WorkerMetadata
    ↓
vLLM Scheduler 聚合
    ↓
请求/block 可以安全进入后续生命周期
```

若开启 lazy offload，Connector 还会记录 GPU block hash，并暂时保留 block。真正提交 STORE 前再次比较 hash，确保该 block 没有被 vLLM 复用成其他请求的数据。

## 十五、多 Server 与并行切分

Connector 支持配置多个 LMCache Server。vLLM `world_size` 必须能被 Server 数整除。每个 Worker 根据并行 rank 映射到对应 Server。

多 Server 场景下，Scheduler lookup 会向相关 Server 提交请求，并维护：

```text
pending lookups
未确认 lookup
每个 Server 的命中数
最终一致命中结果
```

若不同 Server 返回的 hit chunk 数不同，只能使用各 shard 都安全具备的公共前缀，并释放多余尾部锁。否则某些 TP shard 有数据、另一些没有，模型无法形成完整 KV 状态。

## 十六、故障与降级路径

### Server 没有注册上下文

若 lookup 时 `LayoutDescRegistry` 找不到 `(model_name, world_size)`，Server 无法知道对象布局，会返回零命中并记录错误。这通常意味着：

- Worker 尚未注册；
- 注册状态被健康检查清理；
- 模型名或并行配置不一致。

### Retrieve 失败

请求会进入 `BYPASS_LMCACHE`，由 vLLM 正常重新计算 KV，而不是让整个推理失败。

### Preemption

Connector 会清理或重建 request tracker。当前代码对被抢占请求的 KV 重新加载仍存在限制，这是 MP 与 vLLM scheduler 深度耦合的复杂点之一。

### Heartbeat

Scheduler/Worker adapter 定期向 Server 发送 heartbeat。Server 根据实例活跃时间清理失联上下文，避免保留失效 IPC handle 和锁。

## 十七、一条完整的命中请求时序

```text
1. vLLM 收到请求 B，token 前缀与请求 A 相同
2. Scheduler Connector 创建 RequestTracker
3. Scheduler Adapter 向 LMCache 发 LOOKUP
4. Server 对 token 分 chunk 并计算 hash
5. StorageManager 在 L1 查询；缺失部分可从 L2 prefetch
6. Scheduler 得到 LMCache 命中 token 数
7. vLLM 为请求 B 分配 GPU block
8. Connector 生成 RETRIEVE metadata
9. metadata 随 SchedulerOutput 到达 GPU Worker
10. forward 前 start_load_kv() 提交 RETRIEVE
11. LMCache-driven：Server 通过 CUDA IPC 写入 GPU blocks
    或 Engine-driven：Worker 从 CPU chunks scatter 到 GPU blocks
12. vLLM 使用已恢复的 KV，从未命中位置继续 prefill/decode
13. 新计算形成完整 LMCache chunk 后，Scheduler 生成 STORE metadata
14. forward 后 wait_for_save() 提交 STORE
15. KV 从 GPU 写入 L1，并可异步进入 L2
16. Worker 报告 STORE 完成，Scheduler 聚合所有 TP Worker 状态
```

## 十八、如何理解这套分层

vLLM 和 LMCache 的边界可以概括为：

```text
vLLM 决定：
- 请求何时调度
- GPU block 分配给谁
- 哪些 token 已经计算
- 模型何时可以执行

LMCache 决定：
- token 前缀对应哪些缓存对象
- 缓存位于 L1 还是 L2
- 如何预取、淘汰和持久化
- 如何在进程间搬运 KV
```

`LMCacheMPConnector` 的核心作用，就是把 vLLM 的“token + block + 调度 step”翻译成 LMCache 的“chunk key + STORE/RETRIEVE + transfer context”，再把完成状态翻译回 vLLM 能理解的请求生命周期事件。

## 总结

vLLM 与 LMCache MP 的对接不是一个简单的 `get/put` API，而是一套与调度器和 Paged KV Cache 深度结合的协议：

```text
注册阶段：暴露 GPU KV layout 和传输能力
LOOKUP 阶段：按 token chunk 查询 L1/L2
分配阶段：等待 vLLM 获得目标 GPU block
RETRIEVE 阶段：把命中 KV 写回指定 blocks
执行阶段：vLLM 继续模型计算
STORE 阶段：把新生成的完整 chunk 写入 L1/L2
完成阶段：跨 Worker 聚合异步状态并释放资源
```

其中，Scheduler Connector 负责控制决策，Worker Connector 负责数据搬运，LMCache MP Server 负责缓存对象和存储层。`lmcache_driven` 用 CUDA IPC 让 Server 主动搬运 GPU KV，`engine_driven` 则让 Worker 通过 CPU staging、SHM 或 Pickle 完成 gather/scatter。两条路径共享同一套 token key、L1/L2 和请求状态模型，但适配不同设备能力。
