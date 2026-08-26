---
title: "NVIDIA Dynamo 源码阅读（一）：数据中心级推理栈的整体架构"
date: 2026-08-26T13:50:00+08:00
draft: false
tags: ["NVIDIA Dynamo", "LLM Serving", "分布式推理", "源码阅读"]
categories: ["大模型推理"]
description: "从请求路径、控制面、数据面和后端边界出发，理解 NVIDIA Dynamo 为什么是推理引擎之上的分布式编排层。"
---

NVIDIA Dynamo 不是另一个 vLLM 或 TensorRT-LLM。它不负责重新实现 Attention、Continuous Batching 或模型执行，而是把多个推理引擎实例组织成一个可路由、可拆分、可扩缩的服务。

这一区分决定了阅读源码的入口：不要从 CUDA kernel 开始，而应先找到请求如何跨过 Frontend、Router、Prefill Worker 和 Decode Worker，以及服务发现、事件传播、KV 传输分别位于哪一层。

本文基于 Dynamo 提交 `27f09d5`，仓库版本为 `1.5.0`。

## 一、Dynamo 解决的不是单卡执行问题

单个推理引擎已经能完成一次生成：接收 token、执行 Prefill、循环 Decode、管理本机 KV Cache。规模扩大后，系统出现新的问题：

- 同一前缀可能被多个副本重复 Prefill。
- Prefill 与 Decode 的算力和延迟特征不同，固定配比容易浪费 GPU。
- Worker 上下线后，路由器必须快速获得最新拓扑。
- KV Cache 不仅存在于显存，还可能分布在主存、SSD 或其他节点。
- 不同后端有不同请求格式与 KV 传输协议，但上层服务不应被绑定。

Dynamo 把这些问题放在推理引擎之上：

```text
                         控制面
              Discovery / Planner / Metrics
                         |
                         v
Client -> Frontend -> Router -> Inference Worker
             |          |              |
             |          +---- KV Events+
             |
             +------ 流式响应 ---------->

                 KV 数据面（按需）
        Prefill Worker ======> Decode Worker
                    NIXL / backend connector
```

请求平面回答“请求发给谁”；KV 数据面回答“缓存如何移动”；控制面回答“哪些实例存在、负载怎样、是否扩缩容”。三者分开，是 Dynamo 架构最重要的设计。

## 二、一次请求穿过哪些组件

官方架构文档把解耦部署的一次请求拆成九步，源码中可归纳成五个阶段。

### 1. Frontend 完成协议适配

Frontend 对外暴露 OpenAI 兼容接口，完成聊天模板、tokenize、参数校验与流式响应封装。此时请求从 HTTP 对象转成 Dynamo 内部可路由的请求。

Frontend 不是简单反向代理。KV 路由需要 token 序列，而 HTTP 请求通常只有文本；因此预处理必须发生在路由之前。

### 2. Router 选择执行实例

聚合部署中，Router 直接选择一个同时执行 Prefill 和 Decode 的 Worker。解耦部署中，`PrefillRouter` 先选择 Prefill Worker，再把 Prefill 返回的传输元数据注入 Decode 请求。

选择依据并非只有排队长度。KV-aware 路由同时考虑：

- 请求前缀与各 Worker 已缓存块的重叠；
- Worker 当前活动序列与预计工作量；
- Worker 类型、数据并行 rank 和路由分区；
- 自定义过滤、打分与选择策略。

### 3. Prefill Worker 计算 KV Cache

Prefill Worker 消费完整输入，建立 KV Cache，并返回后续传输需要的 backend-specific metadata。元数据描述“如何取缓存”，实际 KV 张量不经过 Frontend 或 Router。

### 4. Decode Worker 拉取 KV 并生成

Decode Worker 根据元数据与 Prefill Worker 建立传输，通过 NIXL 或后端连接器直接搬运 KV。随后进入逐 token Decode，并把输出流式返回。

### 5. KV 事件更新全局视图

Worker 在缓存块写入、移除或清空时发布事件。Router 的索引器消费这些事件，维护“某段 token 前缀目前在哪些 Worker 上”的近实时视图，为下一次调度提供依据。

因此 Dynamo 的主循环不是一次简单 RPC，而是两条相互配合的链：

```text
请求链：HTTP -> preprocess -> select -> execute -> stream
状态链：KV store/remove -> event -> index -> next selection
```

## 三、仓库如何映射到架构

根目录 `Cargo.toml` 展示了 Dynamo 的模块边界。

| 目录 | 职责 |
|---|---|
| `lib/runtime` | 分布式运行时、Endpoint、服务发现、网络传输与流水线 |
| `lib/llm` | LLM 协议、Frontend、模型管理和较高层 KV 路由编排 |
| `lib/kv-router` | KV 索引、调度策略、Worker 负载与路由协议 |
| `lib/kv-hashing` | token block 的稳定哈希 |
| `lib/kvbm-*` | 多层 KV Block Manager |
| `lib/sidecar/*` | vLLM、SGLang、TensorRT-LLM 的 Sidecar 集成 |
| `lib/bindings/python` | Rust 能力的 Python 绑定与高层组件 |
| `deploy` | Kubernetes Operator、Helm、Inference Gateway 与观测组件 |
| `examples/backends` | 三类推理后端的启动与部署示例 |

这是一套明显的“Rust 内核 + Python 编排”结构。请求传输、并发状态和索引等性能敏感部分放在 Rust；后端启动、配置组合与用户扩展保留在 Python。

## 四、控制面与数据面为何要分离

`lib/runtime` 同时支持服务发现和请求传输，但二者不是同一条通道。

服务发现负责持久状态：

```text
namespace / component / endpoint / instance
```

裸机环境可使用 etcd 或文件系统，Kubernetes 环境默认使用 CRD 与 EndpointSlice。请求平面默认使用直接 TCP，也可选择 HTTP 或 NATS。

这种拆分避免了两个常见问题：

1. 不让注册中心承载高频 token 流量；
2. 不让点对点数据连接承担成员关系的一致性。

NATS 在当前架构中也不是所有请求必经的总线。它更多承担可选的 KV 事件传播；请求平面默认已经转向直接 TCP。阅读早期文章时若把 Dynamo 简化成“NATS + etcd 的 RPC 框架”，会错过现在的实现重点。

## 五、后端适配边界在哪里

Dynamo 支持 SGLang、TensorRT-LLM 和 vLLM，但不会强迫它们共享同一内部调度器。统一的是外围契约：

- 接收标准化请求；
- 暴露可发现的 Endpoint；
- 上报负载和 KV 事件；
- 在解耦模式下交换 KV 传输元数据；
- 以流方式返回生成结果。

真正涉及 paged KV、block ID、bootstrap handle 或 opaque state 的部分留在各后端连接器中。这样，Dynamo 可以统一集群调度，而不抹平引擎差异。

Sidecar 模式进一步降低侵入性：后端进程保留自身生命周期与核心逻辑，Sidecar 负责把其事件和能力接入 Dynamo 数据面。

## 六、为什么这个架构适合数据中心规模

Dynamo 的价值随规模增长而增长。单 GPU 服务引入它通常没有必要；多个副本、多个节点和长上下文负载下，系统级优化才可能覆盖额外复杂度。

其收益来自组合，而非单个功能：

- KV-aware routing 减少重复 Prefill；
- Prefill/Decode 分离允许独立扩缩；
- 多层 KV 存储提高缓存容量；
- Planner 根据 SLO 调整资源；
- 统一运行时屏蔽部署和传输差异。

代价同样明确：组件数量、状态传播、故障模式和观测需求都会增加。是否采用 Dynamo，应该以端到端吞吐、TTFT、ITL 与成本为依据，而不是仅比较单引擎 benchmark。

## 七、后续阅读路线

本系列接下来沿源码继续深入：

1. `lib/runtime` 如何用 Namespace、Component、Endpoint 组合分布式流水线；
2. KV Router 如何把 token 前缀变成 Worker 选择；
3. Prefill/Decode 分离如何完成两次调度与一次直接 KV 传输。

建立这张全景图后，Dynamo 的大量目录不再是平铺功能，而是围绕三条平面形成的组合：请求、状态与数据。

## 参考源码

- `Cargo.toml`
- `lib/runtime/src/distributed.rs`
- `lib/runtime/src/component/endpoint.rs`
- `lib/kv-router/src/lib.rs`
- `docs/fern/pages/developer-guide/knowledge-base/concepts/system-architecture/architecture-flow.md`
- `docs/fern/pages/developer-guide/knowledge-base/concepts/system-architecture/distributed-runtime.md`
