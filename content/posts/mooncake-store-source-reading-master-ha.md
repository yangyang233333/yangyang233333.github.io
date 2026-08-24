---
title: "Mooncake Store 源码阅读（四）：Master 持久化、快照与热备恢复"
date: 2026-08-24T10:40:00+08:00
draft: false
tags: ["Mooncake", "高可用", "元数据", "源码阅读"]
categories: ["分布式系统"]
description: "解读 Mooncake Store Master 的 oplog、snapshot、standby 和租约机制，以及它们如何保护控制面状态。"
---

Mooncake Store 把对象位置和副本状态集中在 Master。这样简化了一致性，但也意味着 Master 的状态不能只存在进程内存中。

本篇基于提交 `777cc77`，阅读 oplog、snapshot、standby controller 和恢复相关源码。

## 一、先澄清高可用目标

Mooncake Store 面向高速缓存池。缓存数据本身可以因为节点故障而丢失，系统并不等价于强持久化对象存储。

但控制面仍需要可靠：Master 重启后必须尽可能恢复对象元数据、Segment、配额和副本状态，否则即使真实字节还在，系统也不知道如何访问和回收它们。

因此需要区分两件事。

- 数据副本是否仍存在。
- Master 是否记得这些副本及其状态。

本篇主要讨论第二件事。

## 二、为什么只做定期快照不够

假设 Master 每十分钟保存一次完整快照。第九分钟发生的大量 Put、Remove 和 Eviction，在崩溃后都会消失。

缩短快照间隔又会频繁扫描和序列化庞大元数据。

常见解决方案是 snapshot 加 oplog：

- Snapshot 保存某个时刻的完整状态。
- OpLog 记录快照之后的增量变化。
- 恢复时先加载 Snapshot，再重放 OpLog。

Mooncake Store 的 Master 也采用了这一思路。

## 三、OpLog 记录什么

不是每个函数调用都值得写日志。需要持久化的是会改变可恢复状态的操作，例如对象元数据建立、提交、删除、副本变化和 Segment 相关更新。

当前 `master_service.cpp` 中可以看到 `AppendOpLogWithDurableFinalize`、批量预留和 durable finalize 等路径。

这里最关键的顺序是：某些不可逆的内存状态变化，要等对应日志达到持久条件后才能最终确认。

否则进程可能在“内存已经释放，但日志还没记住”时崩溃，恢复后重新得到一个指向已释放空间的副本。

## 四、Durable Finalize 的设计含义

名字里的 durable finalize 表明操作被拆成了准备与最终完成。

大致过程如下：

```text
准备元数据变化
  -> 追加 OpLog
  -> 等待达到持久条件
  -> 执行最终回收或确认
```

这和 Put 的两阶段思路相似。系统先建立可回滚或可恢复的中间状态，再跨过明确的持久化边界。

源码中的 finalize 回调负责在日志成功后更新配额、移除副本或释放其他资源。

## 五、为什么批量操作先预留日志槽位

批量淘汰可能一次处理很多对象。如果做到一半才发现 oplog 队列或存储无法接受更多记录，前半部分已经改变、后半部分无法提交，恢复语义会非常困难。

因此源码提供 batch oplog reservation。执行批量元数据修改前，先确认日志系统能容纳这组变化。

这是一种 admission control：不是让所有操作先执行再处理失败，而是在跨越危险边界之前确认后续资源可用。

## 六、Snapshot Manager 做什么

`master_snapshot_manager.cpp` 和 `master_snapshot_repository.cpp` 负责快照生命周期。

快照不仅需要序列化对象 map，还要处理：

- 元数据格式版本。
- 写入临时文件与原子替换。
- 快照生成期间的并发变化。
- 与 oplog 位点的对应关系。
- 旧快照清理。
- 加载失败时的错误处理。

如果快照和 oplog 没有共同的序列边界，恢复时就可能重复应用或漏掉一段操作。

## 七、序列化格式为什么必须谨慎演进

`master_service.cpp` 中存在 `ObjectMetadata` 的显式序列化和反序列化逻辑，并检查数组长度、UUID、时间范围和可选字段类型。

显式校验看起来啰嗦，但它保护了两个重要场景：

- 旧版本快照被新版本程序加载。
- 损坏或截断的数据不会悄悄变成错误对象。

源码还明确处理某些不恢复的瞬时字段。例如 soft pin 属于运行时策略状态，不一定应该跨重启原样继承。

这提醒我们：持久化状态不应简单等于“把整个 C++ 对象内存 dump 下来”。

## 八、Standby 如何接管

`ha/standby_controller.cpp`、`standby_state_machine.cpp` 和 `hot_standby_service.cpp` 组成热备相关逻辑。

Standby 需要持续获得主节点的状态变化，并维护足以接管的元数据视图。发生切换时，还要避免旧 Master 与新 Master 同时对外做决定。

因此高可用不仅是复制日志，还涉及 leader 身份、租约和状态机阶段。

## 九、Kubernetes Lease 解决什么

源码包含 `k8s_lease_helper`。在 Kubernetes 环境中，Lease 可以作为 leader 活性与续约机制的一部分。

它的目标不是保存对象数据，而是帮助系统判断谁当前有权作为主节点服务。

如果没有 fencing，仅靠“我联系不到旧主，所以我成为新主”是不安全的。网络分区可能让两个 Master 都认为自己有效。

生产系统仍需要结合部署配置，确保旧主失去租约后无法继续修改共享控制面状态。

## 十、恢复后为什么还要重新校验现实世界

Snapshot 和 oplog 描述的是 Master 最后知道的状态，但节点可能在 Master 停机期间变化。

例如：

- 某个 Segment 所在进程已经退出。
- Client 注册的内存已经失效。
- 本地 SSD 文件被运维清理。
- 节点重启后获得新的实例身份。

因此恢复不能盲目信任历史地址。Master 还需要等待资源重新注册、检查 lease，并清理无法再证明有效的副本。

持久化让控制面“不失忆”，重新校验让控制面“不活在过去”。

## 十一、HA 与数据复制是两套机制

Master 热备保护元数据服务的连续性。

对象多副本保护数据读取路径，并提高并发带宽。

两者缺一不可，但不能互相替代：即使有两个 Master，某个对象唯一的数据副本损坏后仍无法读取；即使对象有三份副本，唯一 Master 的元数据彻底丢失也难以找到它们。

## 十二、源码中的工程化信号

当前 Store 已包含以下能力：

- OpLog 批处理与预留。
- Snapshot repository。
- Standby 状态机。
- Kubernetes Lease 辅助。
- 配额与副本变更的 durable finalize。
- 元数据格式严格校验。
- 失败路径指标与后台清理。

这些代码说明 Mooncake Store 已经从“一个 KV Cache Demo”演进成需要认真处理控制面恢复的分布式系统。

## 十三、整个系列的阅读主线

四篇文章可以归纳为四个问题。

1. 架构篇回答谁负责元数据，谁负责搬数据。
2. Put 篇回答对象何时从不可见变成可读。
3. Get 与 Eviction 篇回答副本怎样被选择和回收。
4. HA 篇回答 Master 崩溃后如何重建状态。

理解这四条主线后，再阅读动态复制、多租户、本地 SSD、NoF 和 Engram 等扩展模块会容易很多。

## 参考源码

- `mooncake-store/src/master_service.cpp`
- `mooncake-store/src/master_snapshot_manager.cpp`
- `mooncake-store/src/master_snapshot_repository.cpp`
- `mooncake-store/src/ha/standby_controller.cpp`
- `mooncake-store/src/standby_state_machine.cpp`
- `mooncake-store/src/hot_standby_service.cpp`
- `mooncake-store/src/k8s_lease_helper.cpp`
- `mooncake-store/include/ha/ha_types.h`
