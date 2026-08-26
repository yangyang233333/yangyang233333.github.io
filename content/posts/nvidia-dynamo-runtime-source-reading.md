---
title: "NVIDIA Dynamo 源码阅读（二）：分布式运行时如何组织服务与请求"
date: 2026-08-26T13:50:00+08:00
draft: false
tags: ["NVIDIA Dynamo", "Rust", "分布式运行时", "源码阅读"]
categories: ["大模型推理"]
description: "深入 lib/runtime，分析 Namespace、Component、Endpoint、服务发现和网络流水线如何组成 Dynamo 的请求平面。"
---

Dynamo 的推理组件能够跨进程、跨节点组合，核心依赖 `lib/runtime`。这一层不理解模型，也不关心 token 的语义；它提供的是一套分布式组件模型：服务如何命名、实例如何注册、客户端如何发现、请求如何流式传输。

本文基于 Dynamo 提交 `27f09d5`。

## 一、运行时的核心抽象

源码从 `DistributedRuntime` 开始。它持有运行时配置、服务发现实现、网络管理器和关闭信号，并向上提供 Namespace。

对象关系可以简化为：

```text
DistributedRuntime
└── Namespace("dynamo")
    ├── Component("frontend")
    │   └── Endpoint("generate")
    ├── Component("router")
    │   └── Endpoint("generate")
    └── Component("worker")
        └── Endpoint("generate") × N instances
```

这四层各有不同职责：

- Runtime：进程级资源和生命周期；
- Namespace：隔离一组服务；
- Component：表示逻辑服务角色；
- Endpoint：表示可调用接口及其多个实例。

Endpoint 名字相同不代表只有一个服务器。每个 Worker 可注册自己的 instance，客户端通过 discovery 得到动态实例集合。

## 二、服务发现保存什么

`lib/runtime/src/discovery` 定义了发现层。一个 Endpoint 注册时，系统不仅记录地址，还附带实例、组件和元数据。客户端查询的关键维度是：

```text
(namespace, component, endpoint) -> [instance...]
```

在裸机模式中，注册与租约可由 etcd 支持；Kubernetes 模式使用 `DynamoWorkerMetadata` CRD 和 EndpointSlice。运行时把发现后端抽象掉，上层 Router 不需要分别实现 etcd watcher 与 Kubernetes watcher。

发现层还必须处理动态变化：Worker 启动、退出、租约失效或滚动升级。`Component::list_instances` 从 discovery 拉取 Endpoint 实例并排序，给更高层建立稳定视图。

服务发现解决“现在有哪些目标”，不负责传输每个生成 token。

## 三、Endpoint 不只是一个地址

`component/endpoint.rs` 将逻辑 Endpoint 与本地服务、远程客户端和指标注册关联起来。调用方可通过同一个 Endpoint：

- 注册本地 handler；
- 建立远程 client；
- 观察实例变化；
- 选择单个实例或进行负载分发；
- 收集 Endpoint 维度指标。

这种设计使组件代码不依赖具体传输方式。上层看到的是异步请求流，下层可以选择直接 TCP、NATS 或其他实现。

值得注意的是，生成请求天然是流式 RPC：输入通常是单个请求，输出是一串增量 token 或事件。因此 Runtime 的 pipeline 抽象围绕 stream 而非普通 request-response 展开。

## 四、Pipeline 如何把本地与远程统一

`lib/runtime/src/pipeline` 把处理过程表示成可连接节点。典型路径如下：

```text
Ingress -> Deserialize -> Handler -> Serialize -> Egress
```

本地组件可以直接连接内存节点；跨进程时，网络 ingress/egress 将同样的流编码并传输。上层逻辑不需要因部署拓扑改变而重写。

网络目录分成几个层次：

| 模块 | 作用 |
|---|---|
| `network/manager.rs` | 管理监听器、连接和共享网络资源 |
| `network/ingress` | 接收远端请求并转入本地 pipeline |
| `network/egress` | 把请求发送到选定实例 |
| `network/codec` | 编解码消息和流帧 |
| `network/tcp` | TCP 客户端与服务端实现 |

默认请求平面使用 TCP，避免所有 token 都绕经中心消息系统。NATS 仍可作为可配置传输和事件通道，但已不是理解主请求链的唯一入口。

## 五、流式响应如何保持上下文

LLM 请求的特殊之处在于响应会持续较长时间。Dynamo 的 pipeline context 随流传播，用于携带请求标识、取消信号和元数据。

这使几个行为成为可能：

1. 客户端断开时向下游传播取消；
2. Router 在转发时保留请求上下文；
3. Worker 逐块返回结果，而不等待完整生成；
4. 指标能够把排队、首 token 和生成阶段关联到同一请求。

如果只把 Runtime 看成 RPC 库，就难以解释其大量 context、stream 与 cancellation 代码。它实际针对的是长生命周期、可取消、持续回传的推理任务。

## 六、控制路径为什么没有侵入模型执行

Runtime 只处理“组件之间如何通信”，模型执行仍由后端完成。一个自定义 Worker 只需：

```text
创建 Runtime
  -> 取得 Namespace
  -> 创建 Component 与 Endpoint
  -> 注册异步 handler
  -> 将 Endpoint 暴露到 discovery
```

Frontend 和 Router 通过同一套发现与 client API 调用它。后端可以是 Python 推理框架，也可以是 Rust mocker；运行时无须知道张量布局。

这一边界也解释了 Dynamo 的双语言设计。Rust 提供稳定、高并发的数据通路，Python 负责把不同推理框架的启动参数和生命周期接到 Endpoint 上。

## 七、故障语义来自两层

运行时故障大致分成两类：

- 发现层故障：实例列表过期、租约失效、注册中心不可用；
- 请求层故障：连接失败、流中断、目标退出或响应超时。

二者不能混为一谈。一个实例仍出现在 discovery 中，不代表 TCP 连接一定可用；一次连接失败，也不意味着应立刻从全局注册表删除实例。

因此生产环境需要同时观测：

- Endpoint 实例数量与变更；
- 连接建立和重连；
- 请求排队、处理与取消；
- 每个组件的健康状态；
- Runtime 与后端各自的错误。

## 八、源码阅读结论

`lib/runtime` 的价值不是提供新颖的 RPC 语法，而是给推理系统建立统一的组件生命周期和流式请求模型。它让同一套 Frontend、Router 和 Worker 既能在单机进程中组合，也能在 Kubernetes 上拆成多个服务。

下一篇进入 `lib/kv-router`：当 Runtime 已经知道“有哪些 Worker”后，Router 如何知道“哪个 Worker 最适合当前 token 前缀”。

## 参考源码

- `lib/runtime/src/distributed.rs`
- `lib/runtime/src/component/component.rs`
- `lib/runtime/src/component/endpoint.rs`
- `lib/runtime/src/discovery/mod.rs`
- `lib/runtime/src/pipeline.rs`
- `lib/runtime/src/pipeline/network/manager.rs`
- `lib/runtime/src/pipeline/network/ingress/unified_server.rs`
- `lib/runtime/src/pipeline/network/egress/unified_client.rs`
