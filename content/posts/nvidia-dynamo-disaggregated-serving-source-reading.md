---
title: "NVIDIA Dynamo 源码阅读（四）：Prefill/Decode 分离与 KV 传输"
date: 2026-08-26T13:50:00+08:00
draft: false
tags: ["NVIDIA Dynamo", "Disaggregated Serving", "NIXL", "源码阅读"]
categories: ["大模型推理"]
description: "分析 Dynamo 如何进行两阶段路由、交换传输元数据，并让 KV Cache 在 Prefill 与 Decode Worker 之间直接移动。"
---

Prefill 与 Decode 使用同一个 Transformer，却具有不同资源特征。Prefill 面向长序列并行计算，关注首 token 延迟；Decode 每轮只处理少量 token，受显存带宽和迭代调度影响。把两者固定绑在同一副本上，资源比例只能随实例一起扩缩。

Dynamo 的 disaggregated serving 将两阶段放到不同 Worker 池，并用直接 KV 传输连接起来。本文基于提交 `27f09d5`。

## 一、为什么要做两次路由

解耦请求包含两个独立选择：

```text
Frontend
   |
   v
选择 Prefill Worker ----> 计算输入 KV
   |                         |
   |<---- transfer metadata--+
   |
   v
选择 Decode Worker <==== 直接搬运 KV
   |
   v
逐 token 生成 -> Frontend -> Client
```

Prefill Worker 的选择偏向输入前缀命中与 Prefill 排队；Decode Worker 的选择偏向持续生成负载、可用 KV 容量和传输拓扑。将二者合成一次普通负载均衡，会丢失阶段差异。

Dynamo 的 `PrefillRouter` 位于 Frontend 与 Worker 之间，负责这段编排，而不是让客户端感知两阶段。

## 二、第一阶段返回的不是 KV 本体

Prefill 完成后不会把庞大的 KV 张量返回 Router。它返回 `disaggregated_params`，其中携带后端完成传输所需的描述信息。

不同后端使用不同语义：

- SGLang 可使用 bootstrap 连接信息；
- TensorRT-LLM 可携带 opaque state；
- vLLM 可围绕 block ID 与 connector metadata 协调。

Router 只负责转交元数据。实际数据路径是 Prefill Worker 到 Decode Worker，避免形成中心带宽瓶颈。

这个设计对应一个经典原则：控制消息经过编排层，大对象走点对点数据面。

## 三、NIXL 在数据面的位置

NIXL，即 NVIDIA Inference Xfer Library，为 GPU、主存和存储之间的数据移动提供统一接口。根据机器拓扑，它可以利用 NVLink、PCIe、InfiniBand/UCX 等路径。

在 Dynamo 中，NIXL 不决定请求发给哪个 Worker。Router 做调度，后端 connector 根据 metadata 发起或配合传输，NIXL 承担数据移动。

```text
Router：谁给谁
Connector：搬哪些 block、何时可用
NIXL：通过哪种能力完成搬运
```

分清这三层，可以避免把 PD 分离误解为一个简单的远程 memcpy。

## 四、PrefillRouter 如何编排请求

`lib/llm/src/kv_router/prefill_router` 包含 activation、admission、query 与 conditional bypass 等模块。其职责可以概括为：

1. 判断请求是否需要远程 Prefill；
2. 从 Prefill Worker 集合中选择目标；
3. 发送 Prefill 请求并等待元数据；
4. 将元数据注入原 Decode 请求；
5. 选择 Decode Worker并继续流式调用；
6. 在异常或策略允许时退化到本地/聚合执行。

Conditional disaggregation 尤其重要。并非所有请求都值得搬运 KV：短 prompt 的传输协调成本可能高于独立 Prefill。策略可以基于输入长度、负载或其他条件决定是否拆分。

## 五、拓扑会改变最优 Worker

假设两个 Decode Worker 都很空闲，但一个与 Prefill Worker 位于同一 NVLink 域，另一个需要跨节点 RDMA。只比较 Decode 队列会做出次优选择。

Dynamo 的 topology-aware KV transfer 将传输代价纳入路由。选择过程需要同时考虑：

- Prefill Worker 已有的缓存重叠；
- Decode Worker 当前负载；
- 两个 Worker 的连接能力与拓扑；
- KV 大小及可用传输路径；
- 目标 Worker 的缓存容量。

因此 PD 分离并不自动带来收益。若网络慢、输入短或调度忽略拓扑，额外传输可能恶化 TTFT。

## 六、Sidecar 为什么适合后端集成

Dynamo 为 vLLM、SGLang 和 TensorRT-LLM 提供 Sidecar crate。Sidecar 负责连接后端事件、传输控制和 Dynamo Runtime，而模型执行仍留在原后端。

这种方式有三点优势：

- 后端升级时不必把完整执行循环 fork 到 Dynamo；
- Rust Sidecar 可稳定处理高频事件和协议转换；
- 各后端保留自己的 paged KV 与调度实现。

代价是集成边界更复杂：后端版本、connector 协议、block size 与生命周期必须匹配。生产部署应把 Dynamo、Sidecar 和推理引擎视为一个经过联调的版本集合。

## 七、多层 KVBM 如何扩展缓存容量

PD 分离解决 Worker 间 KV 移动，KVBM 进一步解决“KV 放在哪里”。其设计把缓存层次扩展为：

```text
GPU HBM -> Host DRAM -> Local SSD / external storage
```

KVBM 由多个 Rust crate 组成：

| crate | 作用 |
|---|---|
| `kvbm-logical` | 逻辑块和地址空间 |
| `kvbm-physical` | 物理存储与分配 |
| `kvbm-engine` | 搬运和操作编排 |
| `kvbm-kernels` | 与数据移动相关的底层能力 |
| `kvbm-consolidator` | 块整合与状态处理 |
| `kvbm-config` | 配置模型 |

后端 scheduler 构造 onboard/offload metadata，Worker 在 forward pass 边界执行异步搬运。事件平面再把 Store/Remove 生命周期广播给 Router 或外部 storage advisor。

这里形成了两个相互独立但协作的优化：Router 决定请求靠近哪份缓存，KVBM 决定缓存驻留在哪个层级。

## 八、故障与回退是实现重点

解耦路径比聚合路径多出多个失败点：

- Prefill Worker 在返回 metadata 前退出；
- KV 传输建立失败或超时；
- Decode Worker 在接收后不可用；
- Router 视图滞后，选择了错误的缓存位置；
- 两端 block layout 或版本不兼容。

因此系统需要明确的 admission、取消、超时、重试和 bypass 语义。盲目重试尤其危险：Prefill 可能已产生状态，Decode 也可能已开始返回 token。

源码中的 cancellation guard、request guard、recovery 和 conditional bypass，说明 Dynamo 把 PD 分离当成一个分布式事务式流程，而非两个独立 HTTP 调用。

## 九、如何判断是否值得部署

评估 PD 分离至少要同时测量：

- TTFT：Prefill 排队、计算和 KV 传输总和；
- ITL：Decode 池在目标并发下的稳定性；
- GPU 利用率：两类 Worker 是否分别达到合理饱和度；
- KV 传输带宽与尾延迟；
- Prefill/Decode 池扩缩时的缓存损失；
- 聚合模式与解耦模式的单位 token 成本。

长 prompt、长输出、阶段资源比例差异明显时，解耦通常更有价值。短请求或小规模部署中，聚合模式更简单，也可能更快。

## 十、系列总结

从四篇源码阅读可以看到，Dynamo 的核心不是单个算法，而是一组边界清晰的协作机制：

```text
Runtime 组织分布式组件
Router 维护 KV 与负载视图
PrefillRouter 编排两阶段执行
Connector + NIXL 移动 KV
KVBM 扩展缓存层次
Inference Engine 专注模型执行
```

这种分层让 Dynamo 能站在多个推理引擎之上，但也意味着生产价值必须通过系统级 benchmark 证明。最合理的阅读和落地顺序，是先跑通聚合模式，再启用 KV-aware routing，最后评估 PD 分离与多层缓存。

## 参考源码

- `lib/llm/src/kv_router/prefill_router`
- `lib/llm/src/kv_router/routing_host`
- `lib/sidecar/vllm`
- `lib/sidecar/sglang`
- `lib/sidecar/trtllm`
- `lib/kvbm-engine`
- `lib/kvbm-logical`
- `lib/kvbm-physical`
- `docs/fern/pages/developer-guide/knowledge-base/concepts/system-architecture/disaggregated-serving.md`
- `docs/fern/pages/developer-guide/knowledge-base/modular-components/kvbm/kvbm-design.md`
