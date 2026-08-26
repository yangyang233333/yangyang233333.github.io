---
title: "NVIDIA Dynamo 源码阅读（三）：KV-aware Router 如何选择 Worker"
date: 2026-08-26T13:50:00+08:00
draft: false
tags: ["NVIDIA Dynamo", "KV Cache", "路由算法", "源码阅读"]
categories: ["大模型推理"]
description: "从 token 分块哈希、KV 事件、Radix Tree、负载估计和选择策略出发，拆解 Dynamo KV-aware Router。"
---

普通负载均衡只关心哪个 Worker 更空闲。LLM 推理还需要回答另一个问题：哪个 Worker 已经缓存了当前请求的最长前缀？如果忽略缓存局部性，相同 system prompt、文档或多轮会话会在不同 GPU 上反复 Prefill。

Dynamo 的 KV-aware Router 将缓存命中与运行负载放进同一次决策。本文基于提交 `27f09d5`，重点阅读 `lib/kv-router` 与 `lib/llm/src/kv_router`。

## 一、路由问题如何形式化

设请求 token 被按固定 block size 切成若干块：

```text
[t0 ... t15] [t16 ... t31] [t32 ... t47] [剩余 token]
      B0            B1            B2
```

每个完整块计算链式哈希。Router 维护如下关系：

```text
B0 -> {worker 1, worker 3}
B1 -> {worker 1}
B2 -> {worker 1}
```

如果新请求具有相同前三块，Worker 1 的重叠长度最大。但它可能已经非常繁忙，因此最终决策不是简单的最长前缀匹配，而是 locality 与 load 的权衡。

## 二、为什么使用链式 block hash

`compute_block_hash_for_seq` 不会孤立地哈希每个 token 块，而是把前一个块的 hash 纳入下一个块。这使相同内容出现在不同前缀后时不会被错误视为同一缓存位置。

概念上可写成：

```text
H0 = hash(tokens[0:block])
H1 = hash(H0, tokens[block:2*block])
H2 = hash(H1, tokens[2*block:3*block])
```

这种结构天然对应前缀树。查找时从根沿请求块依次前进，直到某个块不存在；走过的深度就是可复用的缓存前缀长度。

不完整尾块通常不能作为稳定共享单元，因为后续 token 到来后其内容仍会变化。

## 三、Router 如何知道 Worker 的缓存状态

推理后端在 block 存储、移除或全部清空时产生 KV event。事件经 Sidecar 或后端集成转换成统一的 `RouterEvent`，其中包含：

- Worker ID 与 data-parallel rank；
- event ID；
- block hash 列表；
- store、remove 或 clear 类型；
- 存储层级等附加信息。

`KvEventSender` 把事件送入索引器的 FIFO mutation queue。索引器串行应用会改变逻辑状态的操作，同时允许查询通过独立通道进入。这个边界很重要：如果增删事件乱序，Router 可能把请求发送到实际上已经驱逐缓存的 Worker。

源码还提供事件确认、Worker reset、恢复查询和近似 LRU 等机制，用来处理重启、丢事件与大规模状态维护。

## 四、Radix Tree 中保存什么

`RadixTree` 的节点代表一段连续 block 前缀，节点关联拥有该前缀的 Worker 集合。压缩版本会合并只有单分支的路径，降低长上下文下的节点开销。

一次查询返回的不只是“是否命中”，而是各候选 Worker 的 overlap score。对于多层缓存，还可得到 device、host 或外部存储层的命中信息。

```text
请求块： A -> B -> C -> D

Worker 1：A -> B -> C       overlap = 3
Worker 2：A -> B            overlap = 2
Worker 3：无                overlap = 0
```

Indexer 与 Scheduler 分开：Indexer 回答缓存事实，Scheduler 决定如何使用事实。这样可替换数据结构或远程索引，而不把路由策略写死在树中。

## 五、负载模型补上缓存视角的盲区

最长命中不一定是最佳选择。假设 Worker 1 命中三个块但排队很深，Worker 2 只命中两个块却立即可执行，把请求发给 Worker 2 可能更早完成。

Dynamo 维护活动序列和 Worker 负载投影，估计请求加入后带来的 Prefill 与 Decode 工作量。调度输入大致包括：

```text
Worker cache overlap
+ active sequence load
+ queued request load
+ worker availability
+ request-specific constraints
```

`PrefillLoadEstimator`、`SchedulerQueue` 和 `WorkerLoadProjection` 分别承担负载估计、等待队列和前瞻更新。路由决策后立即把潜在负载计入视图，而不是等后端稍后上报，否则高并发下多个请求可能同时涌向同一 Worker。

## 六、选择过程被拆成 Filter、Score、Pick

新版 Router 将策略抽象为三段：

1. Filter：排除不满足条件的 Worker；
2. Score：根据 KV 命中、负载或自定义指标评分；
3. Pick：从候选集中选出最终 Worker。

`DefaultWorkerSelector` 提供内置行为，`WorkerSelectionPolicy` 允许插件化组合。仓库中的 `lib/router-plugins` 和 `examples/router/custom-policy-example` 展示了自定义策略入口。

这比一个巨大的 `select_worker()` 更利于演进。例如：

- LoRA 请求可先过滤没有对应 adapter 的 Worker；
- 多数据中心环境可先过滤不可接受的区域；
- PD 分离可按 WorkerType 区分 Prefill 与 Decode；
- 业务可把缓存分数与排队时间按自身 SLO 重新加权。

## 七、路由状态为什么只能近实时

KV Cache 变化速度很快。若每次路由都同步查询每张 GPU，查询成本会抵消收益；若完全依赖异步事件，视图又可能短暂落后。

Dynamo 选择事件驱动的近实时副本，并配套恢复机制。这意味着 Router 的目标不是获得强一致全局真相，而是在可控成本下做高质量决策。

因此代码中会看到以下工程措施：

- event ID 检测缺口与乱序；
- Worker 注册和移除时清理索引；
- recovery query 重建 Worker 状态；
- dedup 与 batching 降低事件开销；
- pruning 和近似 LRU 控制索引体积。

KV-aware routing 的难点并非 Radix Tree 查找本身，而是持续维护可信、可恢复且足够便宜的缓存视图。

## 八、什么时候 KV-aware routing 最有效

它最适合存在明显前缀复用的负载：

- 固定 system prompt；
- RAG 中重复文档块；
- 多轮会话；
- agent 工作流的共享上下文；
- 大量请求使用相同 few-shot 示例。

如果 prompt 几乎随机且很短，KV 事件、索引和路由计算可能没有足够回报。评估时应同时观察 prefix hit rate、TTFT、Router 开销和负载倾斜，而不能只看命中率。

## 九、源码阅读结论

Dynamo Router 的核心不是“按 hash 分片”，而是一个闭环：

```text
Worker 产生 KV 事件
    -> Indexer 更新前缀位置
    -> 请求查询 overlap
    -> Scheduler 结合负载选择
    -> 预测负载立即回写
    -> Worker 执行并产生新事件
```

下一篇将进入解耦服务：为何一次请求需要先选 Prefill Worker、再选 Decode Worker，以及 Router 如何让 KV 绕过自身直接传输。

## 参考源码

- `lib/kv-hashing`
- `lib/kv-router/src/protocols.rs`
- `lib/kv-router/src/indexer/kv_indexer.rs`
- `lib/kv-router/src/indexer/radix_tree.rs`
- `lib/kv-router/src/scheduling/mod.rs`
- `lib/kv-router/src/scheduling/selector`
- `lib/kv-router/src/sequences`
- `lib/llm/src/kv_router/publisher`
