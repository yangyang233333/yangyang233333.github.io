---
title: "Mooncake Store 源码阅读（一）：从对象存储接口到控制面与数据面"
date: 2026-08-24T10:10:00+08:00
draft: false
tags: ["Mooncake", "KV Cache", "分布式存储", "源码阅读"]
categories: ["分布式系统"]
description: "从源码目录、进程角色和一次请求的路径入手，解释 Mooncake Store 如何把元数据控制与大对象传输分离。"
---

Mooncake Transfer Engine 解决的是“怎样快速搬数据”，Mooncake Store 解决的则是更上层的问题：一个对象叫什么、放在哪些节点、有哪些副本、何时可读、空间不足时淘汰谁，以及 Master 重启后怎样恢复这些事实。

本文使用 Mooncake 仓库提交 `777cc77` 作为源码基线。这个版本的 Store 已经远超早期原型，包含多租户配额、内存与本地盘分层、动态副本、批量淘汰、快照和热备等能力。

## 一、先看整体分层

Mooncake Store 可以拆成四层。

第一层是上层调用者。vLLM、SGLang 或其他推理系统通过 Python、C++、Rust、Go 等接口访问对象。

第二层是 Client。`RealClient` 负责本地缓冲区、Master RPC、数据传输、重试和资源清理。

第三层是 Master。`MasterService` 保存对象元数据，管理 Segment、分配副本、维护租户配额，并驱动淘汰和复制。

第四层是数据资源。DRAM、VRAM、本地 SSD 或其他后端提供真正保存字节的空间，Transfer Engine 执行跨节点传输。

数据不会先发送到 Master，再由 Master 转发。Master 只告诉 Client 应该访问哪些副本，真正的大块数据直接在 Client 与目标内存之间移动。

## 二、源码目录怎样阅读

建议先抓住以下文件。

- `mooncake-store/include/real_client.h`：Client 的主要状态和接口。
- `mooncake-store/src/real_client.cpp`：Put、Get、Remove 与初始化流程。
- `mooncake-store/include/master_service.h`：Master 的核心数据结构。
- `mooncake-store/src/master_service.cpp`：元数据状态机和控制逻辑。
- `mooncake-store/include/replica.h`：副本描述与状态。
- `mooncake-store/include/segment.h`：可分配存储资源。
- `mooncake-store/src/allocation_strategy.cpp`：副本放置策略。
- `mooncake-store/src/allocator.cpp`：具体空间分配。
- `mooncake-store/src/transfer_task.cpp`：Store 到 Transfer Engine 的任务封装。

不要一开始就顺序阅读体量巨大的 `master_service.cpp`。更有效的方法是从 RPC 接口出发，沿 `PutStart`、`PutEnd`、`GetReplicaList` 和 `Remove` 四条链路向下追踪。

## 三、Client 不是一层简单包装

`RealClient::setup_real` 会组装 Store 客户端运行所需的大部分组件：Transfer Engine、Master Client、本地缓冲区、Client Service、SSD offload 以及可选的 HTTP 服务。

它同时向 Master 注册自身可用的 Segment。Master 只有知道某个节点提供了多大的空间、属于什么介质，才能把对象副本分配到该节点。

`ResourceTracker` 也值得注意。它持有 Client 实例的弱引用，并通过信号处理线程在异常退出时清理资源。这说明 Store 的 Client 并非无状态 RPC stub，而是持有已注册内存、后台线程和服务端点的长期运行组件。

## 四、Segment 是资源管理边界

Transfer Engine 中的 Segment 描述“哪些内存可以被远程访问”。Store 在此基础上又加入容量和分配语义。

Master 挂载 Segment 时，会建立对应的分配器和资源记录。后续 Put 不直接选择某个裸地址，而是先选择 Segment，再从 Segment 中分配一段 offset 和 length。

一个副本因而至少包含以下信息：

- 位于哪个 Segment。
- 从什么 offset 开始。
- 长度是多少。
- 当前处于什么状态。
- 使用内存、磁盘还是其他介质。

这层抽象把“对象副本”与“机器上的一段可寻址空间”连接起来。

## 五、Master 管理的是状态机

`MasterService` 的核心不是 RPC，而是对象元数据状态机。

一个对象从写入开始到可读，大致经历以下过程。

```text
PutStart
  -> 创建对象元数据
  -> 选择并预留副本
  -> Client 传输数据
  -> PutEnd
  -> 副本变为可读
```

如果传输失败，预留空间必须回收；如果 Client 在中途失联，后台清理也必须识别未完成状态。于是元数据不能只有“key 到地址”的静态映射，还要记录写入者、租约、时间戳和副本状态。

## 六、控制面与数据面如何配合

一次典型 Put 包含两类通信。

控制面通信通过 Master RPC 完成。Client 请求创建对象并获得目标副本列表。

数据面通信通过 Transfer Engine 完成。Client 将本地 Buffer 直接写入目标 Segment。

传输完成后，Client 再调用 Master，提交写入结果。只有提交成功的副本才应被 Get 返回。

这种设计让 Master 不承担对象字节流，避免中心节点成为带宽瓶颈；同时所有对象状态仍由一个明确的控制面裁决。

## 七、Store 与传统对象存储的差异

Mooncake Store 的对象通常是推理过程中的 KV Cache 或张量数据。它们有几个特点：

- 对吞吐和尾延迟极其敏感。
- 对象可能位于 GPU 显存。
- 生命周期通常较短。
- 缓存副本可以重建，不一定要求传统存储级持久性。
- 大对象适合切片并行传输。

因此 Store 优先优化高速缓存池，而不是把所有语义都建立在慢速持久化存储之上。

## 八、一次 Get 的最短路径

Get 的核心路径可以概括为：

```text
Client 请求副本列表
  -> Master 筛选可读副本
  -> Client 选择来源
  -> Transfer Engine 拉取数据
  -> 返回本地 Buffer
```

Master 不参与数据复制。若对象有多个副本，Client 和控制面还可以结合 locality、介质类型及可用性选择更合适的来源。

## 九、为什么 Store 仍需要中心化 Master

完全去中心化看起来更有扩展性，但对象写入原子性、副本分配、租户配额和统一淘汰都会变复杂。

Mooncake Store 选择把元数据集中管理，把大流量数据面分散出去。这是许多高性能存储系统常用的折中：中心控制面负责做决定，分布式数据面负责跑带宽。

随着规模扩大，源码又通过 metadata shard、批处理、oplog 和后台任务降低 Master 压力，而不是放弃统一状态机。

## 十、后续文章

下一篇沿一条完整 Put 链路阅读 `RealClient`、`PutStart`、副本分配、Transfer Engine 任务和 `PutEnd`，重点解释对象写入原子性从哪里来。

## 参考源码

- `docs/source/design/architecture.md`
- `mooncake-store/include/real_client.h`
- `mooncake-store/src/real_client.cpp`
- `mooncake-store/include/master_service.h`
- `mooncake-store/src/master_service.cpp`
- `mooncake-store/include/segment.h`
- `mooncake-store/include/replica.h`
