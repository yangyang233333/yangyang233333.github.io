---
title: "Mooncake Store 源码阅读（三）：Get、副本选择与淘汰回收"
date: 2026-08-24T10:30:00+08:00
draft: false
tags: ["Mooncake", "KV Cache", "缓存淘汰", "源码阅读"]
categories: ["分布式系统"]
description: "从 GetReplicaList、Replica 状态和批量 Eviction 分析 Mooncake Store 的读取与空间回收。"
---

写入建立对象，读取和淘汰决定缓存系统能否长期稳定运行。本篇沿 Get、Remove 与后台 Eviction 三条路径，观察 Mooncake Store 如何维护副本可读性和容量平衡。

源码基线仍为 `777cc77`。

## 一、Get 首先读取的是元数据

Client 不知道对象当前在哪台机器，也不应缓存一份永远不变的地址。

读取开始时，Client 向 Master 请求副本列表。核心入口之一是 `MasterService::GetReplicaList`。

Master 查找 key 对应的 `ObjectMetadata`，然后筛选当前允许读取的副本。返回结果包含 Segment、offset、长度和介质等信息。

随后 Client 才通过 Transfer Engine 把对象复制到本地 Buffer。

## 二、不是列表里的每个副本都可读

一个对象可能同时存在以下副本：

- 已完成并可读的内存副本。
- 正在复制的动态副本。
- 写入失败、等待清理的副本。
- 位于本地盘的副本。
- 所在 Segment 正在卸载的副本。

因此 Get 不能只检查 `replicas` 是否非空。源码中的 `HasReadableReplica` 以及各种状态判断，负责把控制面中的暂态副本排除掉。

读取正确性的一个重要来源，就是“只从已提交的副本集合中选择”。

## 三、副本选择优化什么

拥有多个可读副本后，系统还要选择来源。

理想选择通常考虑：

- 是否位于本机或同一故障域。
- 介质是 DRAM、VRAM 还是磁盘。
- 传输协议和拓扑成本。
- 副本是否正在被回收。
- 是否能满足本次 Buffer 类型。

Mooncake 将元数据筛选与实际传输分开，使选择策略可以持续演进。Master 保证候选集合合法，Client 和 Transfer Engine 完成具体数据路径。

## 四、读取失败不等于对象不存在

控制面返回副本后，数据面仍可能失败。例如节点刚好退出、网络断开，或 Segment 已经不可访问。

客户端可以在多个候选副本之间重试。只要还有另一份完整副本，对象就可能继续读取。

这也是复制的价值之一：它既提升热点读取带宽，也为短暂的数据面故障提供备用路径。

## 五、Remove 的第一目标是停止可见

删除操作首先需要让后续 Get 不再把对象当作可读对象返回，然后再释放各副本占用的资源。

如果先释放内存，再更新元数据，并发 Get 可能拿到已经失效的地址。

如果只删除元数据，不回收 allocator 中的空间，容量会持续泄漏。

因此 Remove 同样是一段状态转换，而不是对 map 调一次 `erase`。

`MasterService::Remove`、`RemoveByRegex` 和 `RemoveAll` 展示了单对象、模式匹配和全量清理的不同入口。

## 六、淘汰与显式删除有什么不同

Remove 是调用者明确要求删除对象。

Eviction 是系统为了释放容量，自主选择可以牺牲的缓存对象。它必须尊重更多约束：

- 硬 pin 对象不能淘汰。
- 未过期租约可能阻止淘汰。
- 正在写入或复制的对象需要谨慎处理。
- 某些策略要求至少保留一份副本。
- 本地盘副本和内存副本的回收代价不同。
- 配额账本必须与真实回收保持一致。

## 七、Eviction Strategy 与执行分离

`eviction_strategy.h` 描述“谁应优先被淘汰”。真正的副本移除、allocator 释放、元数据更新和 oplog 持久化则由 Master 执行。

这是策略与机制分离：策略给出候选，机制保证删除过程安全。

Store 中还包含 Count-Min Sketch 等数据结构，可用于近似访问频率。近似统计比维护每个 key 的精确全局计数更适合大规模缓存。

## 八、为什么需要批量淘汰

每淘汰一个对象都可能涉及锁、元数据修改、日志和配额更新。逐对象执行会让固定成本非常高。

源码中的 BatchEvict 和 NoF BatchEvict 路径尝试把多个候选一起处理，并预留批量 oplog 空间。

批量化提高吞吐，但也增加一致性难度：任何中途失败都不能让日志、元数据、allocator 与 quota 出现四套不同答案。

## 九、Pin 与 Lease 的角色不同

Pin 表达策略意图：这个对象暂时不希望被缓存淘汰。

Lease 表达时间约束：某个进行中的操作或持有关系在截止时间前仍然有效。

硬 pin 通常是强约束。软 pin 可以结合系统压力降级。租约过期则帮助系统回收失联 Client 遗留的暂态资源。

把两者混为一个布尔字段，会很难同时处理业务优先级和故障恢复。

## 十、Segment 卸载怎样影响对象

节点下线或资源池缩容时，Master 通过 `UnmountSegment` 处理 Segment。

它必须找到受影响对象，移除或迁移位于该 Segment 的副本，并重新判断对象是否还有可读副本。

这说明对象元数据既需要从 key 找副本，也需要能够从 Segment 反向找到受影响对象。资源拓扑变化是 Store 控制面的常态，而不是罕见异常。

## 十一、Local SSD 是缓存层而非简单文件输出

当前源码包含 `local_ssd`、`file_storage`、`nvme_kv_backend` 等实现。它们让 Store 能在 DRAM 之外使用更大但更慢的本地介质。

分层存储会引入新的决策：

- 什么时候从内存下沉到 SSD。
- Get 是否先查本地热缓存。
- SSD 副本是否计入可读副本。
- 删除时如何协调内存和磁盘空间。
- 重启后哪些磁盘状态可以恢复。

因此 SSD 不是 Transfer Engine 多一个协议那么简单，而是对象生命周期的一部分。

## 十二、读路径与回收路径的共同原则

Get、Remove 和 Eviction 看似是三个功能，实际共享一个原则：只有 Master 才能改变“哪些副本仍然有效”这一事实。

Client 可以报告传输结果，后台线程可以提出淘汰候选，allocator 可以释放地址，但对象可见性最终由元数据状态机统一决定。

## 十三、下一篇

最后一篇讨论 Master 自身的可靠性：oplog、snapshot、standby、lease 与故障恢复。重点不是宣称缓存永不丢失，而是解释控制面怎样避免重启后失去对资源的认知。

## 参考源码

- `mooncake-store/src/real_client.cpp`
- `mooncake-store/src/master_service.cpp`
- `mooncake-store/include/replica.h`
- `mooncake-store/include/eviction_strategy.h`
- `mooncake-store/include/count_min_sketch.h`
- `mooncake-store/src/local_hot_cache.cpp`
- `mooncake-store/src/local_ssd/manager.cpp`
