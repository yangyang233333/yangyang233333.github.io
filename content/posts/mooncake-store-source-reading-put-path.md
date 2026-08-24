---
title: "Mooncake Store 源码阅读（二）：Put 写入链路与对象原子性"
date: 2026-08-24T10:20:00+08:00
draft: false
tags: ["Mooncake", "KV Cache", "对象存储", "源码阅读"]
categories: ["分布式系统"]
description: "沿 RealClient、PutStart、Transfer Engine 和 PutEnd 阅读 Mooncake Store 的完整写入链路。"
---

Mooncake Store 的 Put 不是一次 RPC。它是一段跨越 Client、Master、分配器和 Transfer Engine 的两阶段流程。

本文继续使用提交 `777cc77`。阅读重点是 `real_client.cpp` 与 `master_service.cpp` 中的写入路径。

## 一、为什么 Put 必须拆成两段

如果 Master 收到 Put 请求后立刻把对象标记为可读，其他 Client 可能读到尚未传完的数据。

如果等所有数据都发送给 Master，再由 Master 落到目标节点，控制面又会成为数据面瓶颈。

Mooncake Store 的做法是：Master 先预留副本，Client 直接传输，最后由 Master 提交状态。

```text
PutStart
  -> 返回目标副本
  -> 直接传输对象
  -> PutEnd
```

这相当于围绕一次数据面操作建立轻量级提交协议。

## 二、Client 侧的准备工作

Put 开始前，`RealClient` 需要确定源 Buffer 的位置和长度。源数据可能来自普通主机内存，也可能来自已经注册的加速器内存。

Client 会把大对象切成适合传输的任务。每个任务包含本地地址、远端 Segment、远端 offset 和长度，然后交给 Transfer Engine。

Store 不重复实现 RDMA、TCP 或 GPU Direct 细节。它只把“对象副本”翻译成 Transfer Engine 能执行的传输描述。

## 三、进入 `MasterService::PutStart`

`PutStart` 位于 `mooncake-store/src/master_service.cpp`。它承担的职责远多于生成几个地址。

典型检查包括：

- key 和对象大小是否合法。
- 租户是否允许继续占用空间。
- 同名对象是否已经存在或正在写入。
- 复制配置是否合法。
- 当前有哪些 Segment 可用。
- 能否为目标副本预留足够空间。

检查通过后，Master 创建 `ObjectMetadata`，并记录本次写入的 Client、租约与副本列表。

此时对象还不应被普通 Get 当作完整对象读取。

## 四、副本怎样被分配

副本放置由 allocation strategy 和 allocator 协作完成。

allocation strategy 决定优先选择哪些 Segment。它需要考虑介质、容量、节点分布和复制要求。

allocator 负责在选中的 Segment 内找到具体空间，也就是 offset 和 length。

两者分开有一个明显好处：放置策略可以演进，而底层空间管理不必跟着重写。

## 五、切片与并行传输

大对象不一定只对应一个连续副本描述。Store 可以把对象切成多个 slice，分布在不同资源上，并由 Transfer Engine 并行执行。

Client 为每个目标片段创建传输任务，大致包含：

```text
本地 Buffer 区间
目标 Segment ID
目标 offset
传输长度
```

Transfer Engine 根据 Segment 元数据选择 RDMA、TCP 或其他传输路径，并等待各任务完成。

Store 的对象层只关心所有必要片段是否成功，不需要知道每个 RDMA work request 的实现细节。

## 六、失败时为什么不能只返回错误

假设 Master 已经预留两份副本，但第二份传输失败。如果 Client 直接向调用者返回错误，Master 中仍会留下预留空间和未完成元数据。

因此写入失败路径必须做补偿：

- 取消或终止未完成任务。
- 告知 Master 哪些副本失败。
- 回收对应空间。
- 清理未提交的对象状态。
- 更新配额和统计值。

源码中大量边界处理正是为了保证“错误返回之后，系统还能继续正确分配资源”。

## 七、`PutEnd` 才是可见性边界

数据传输完成后，Client 调用 `MasterService::PutEnd` 对应的 RPC。

Master 根据 Client 上报的结果更新副本状态。成功副本进入可读集合，失败副本被排除或回收。若复制策略要求的最低条件无法满足，整个写入不能作为成功对象对外可见。

因此对象原子性不是 Transfer Engine 提供的。Transfer Engine 只报告一个个数据任务是否完成；对象级原子性由 Master 的元数据状态转换提供。

## 八、这里的原子性意味着什么

Mooncake 架构文档强调：Get 总会读取一个一致版本，但不一定是最新版本。

源码角度可以这样理解：

- 未完成写入不会以完整对象身份暴露。
- Get 只选择满足可读条件的副本。
- 同一对象的元数据更新由 Master 串行化和持久化机制约束。
- 并发覆盖写不会让读者拼接两个版本的片段。

这不是数据库事务的全部语义，而是面向大对象缓存非常关键的完整性保证。

## 九、租约解决 Client 消失问题

Client 可能在 `PutStart` 后崩溃，永远不会调用 `PutEnd`。

对象元数据中的租约和时间戳让 Master 能识别长期未完成的写入。后台任务可在租约过期后回收副本，避免空间永久泄漏。

租约的意义是把“等待一个进程回复”转换成“等待一个有截止时间的状态”。这是分布式资源管理中很实用的设计。

## 十、写入路径中的配额核算

当前 Store 支持 tenant 维度的容量治理。Put 预留空间时就需要考虑配额，而不能等到传输结束才检查。

否则多个并发 Put 可能同时通过检查，最终一起突破上限。

因此配额通常伴随预留、完成、失败回滚和删除四类状态变化。源码里的 accounting 辅助函数虽然看似繁琐，却决定了系统在并发和失败场景下是否会“越算越多”或“越算越少”。

## 十一、Put 链路的阅读方法

建议按下面顺序跟踪。

1. 从 `RealClient` 的 Put 入口观察参数和 Buffer。
2. 找到 Master Client 发出的 PutStart RPC。
3. 进入 `MasterService::PutStart` 查看元数据创建。
4. 跟进 allocation strategy 和 allocator。
5. 回到 Client 查看 transfer task 的构造和等待。
6. 进入 `MasterService::PutEnd` 查看状态提交。
7. 最后检查所有提前返回分支是否释放空间。

这条顺序比直接阅读整个 Master 文件更容易建立完整心智模型。

## 十二、下一篇

下一篇分析 Get、Remove 和 Eviction，解释 Store 如何选择可读副本，以及“删除元数据”和“回收真实空间”为什么不能简单地视为同一个动作。

## 参考源码

- `mooncake-store/src/real_client.cpp`
- `mooncake-store/src/master_client.cpp`
- `mooncake-store/src/master_service.cpp`
- `mooncake-store/src/allocation_strategy.cpp`
- `mooncake-store/src/allocator.cpp`
- `mooncake-store/src/transfer_task.cpp`
- `mooncake-store/include/replica.h`
