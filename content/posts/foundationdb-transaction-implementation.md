---
title: "FoundationDB 事务如何实现？从源码看 GRV、Resolver、TLog 与 Storage Server"
date: 2026-08-14T17:00:00+08:00
draft: false
tags: ["FoundationDB", "数据库", "事务", "MVCC", "Resolver", "TLog", "源码分析"]
categories: ["技术"]
---

FoundationDB 支持跨任意 key range 的 ACID 事务，并向应用提供严格可串行化语义。但它既没有让每个 Storage Server 参与经典的两阶段提交，也不是传统单机数据库中“共享缓冲池 + Undo/Redo”的事务实现。

它的核心方案可以概括为：

> **全局版本号 + MVCC 快照读 + 客户端暂存修改 + Resolver 乐观冲突检测 + 多副本 TLog 持久化。**

本文结合 FoundationDB 官方源码，沿着一笔事务的完整路径，解释它如何从读取、提交、冲突检测，一直走到日志持久化和 Storage Server 应用。

本文分析的源码版本为：

```text
commit: eab21ffe07c20575d5bac9ab0745375b8f7f8357
date:   2026-08-12
```

源码中的具体行号会随版本变化，阅读时应以函数和类型名为主要定位依据。

## 一、整体架构

FoundationDB 的事务涉及以下核心角色：

| 角色 | 主要职责 |
|---|---|
| Client | 保存事务 mutations、读冲突范围和写冲突范围 |
| GRV Proxy | 分配 Read Version |
| Commit Proxy | 批量接收事务、分配 Commit Version、组织提交流水线 |
| Resolver | 检测乐观事务冲突 |
| TLog | 复制并持久化已提交 mutations |
| Storage Server | 提供版本化读取，异步应用 TLog mutations |
| Sequencer | 为事务系统提供版本推进基础 |

完整流程可以简化为：

```text
Client
  │
  ├── 获取 Read Version
  │
  ├── 从 Storage Server 读取该版本
  │
  ├── 在客户端积累 mutations 和 conflict ranges
  │
  └── 提交
        │
        ▼
   Commit Proxy
        │
        ├── 组成 Commit Batch
        ├── 分配 Commit Version
        └── 按 key range 发给 Resolver
        │
        ▼
     Resolver
        │
        ├── 检查 read conflict ranges
        ├── 登记 write conflict ranges
        └── 返回 committed / conflict / too old
        │
        ▼
   Commit Proxy
        │
        ├── 丢弃冲突事务
        ├── 为 mutation 添加 Storage Server tag
        └── 将成功事务写入 TLog
        │
        ▼
       TLog
        │
        ├── 按冗余策略复制和持久化
        └── 返回 durable
        │
        ▼
   Commit Proxy
        │
        └── 向 Client 返回 Commit Version
        │
        ▼
 Storage Server
        ├── 异步读取自己的 tagged mutations
        ├── 应用到内存中的版本化数据
        └── 后台持久化到 Storage Engine
```

最关键的解耦是：

```text
事务提交完成
    不等于
Storage Server 已经将数据写入本地文件
```

客户端收到成功时，事务已经可靠进入复制的 TLog；Storage Server 可以在之后异步追赶。

## 二、第一阶段：获得 Read Version

FoundationDB 事务不会随意读取各个 Storage Server 的“当前值”。事务首先从 GRV Proxy 获取一个全局逻辑读版本：

```text
Read Version = RV
```

客户端相关逻辑主要位于：

```text
fdbclient/NativeAPI.actor.cpp
```

服务端 GRV 处理位于：

```text
fdbserver/grvproxy/GrvProxyServer.cpp
```

GRV 是 Get Read Version 的缩写。获得版本后，事务对不同 Storage Server 的读取都携带相同的版本：

```text
Storage Server A：读取 version 100
Storage Server B：读取 version 100
Storage Server C：读取 version 100
```

这样，即使数据分布在多个节点上，事务看到的仍然是数据库在同一个逻辑版本上的一致快照。

Read Version 是递增的数据库逻辑版本，而不是 Unix 时间戳。源码中版本通常使用 `Version` 表示，本质上是一个整数类型。

事务提交结构中的 `read_snapshot` 保存了这个版本：

```cpp
struct CommitTransactionRef {
    Version read_snapshot;
    VectorRef<MutationRef> mutations;
    VectorRef<KeyRangeRef> read_conflict_ranges;
    VectorRef<KeyRangeRef> write_conflict_ranges;
    // ...
};
```

定义位于：

```text
fdbclient/include/fdbclient/CommitTransaction.h
```

`read_snapshot` 不只是读取数据时使用，也是 Resolver 判断事务是否与后续写入冲突的时间起点。

## 三、第二阶段：修改首先保存在客户端

FoundationDB 客户端事务主要积累三类信息：

```text
1. mutations
2. read conflict ranges
3. write conflict ranges
```

### 1. Mutations

例如客户端执行：

```python
tr[b"account/A"] = b"80"
tr.clear(b"temporary/key")
tr.add(b"counter", encode_int64(1))
```

这些操作会被整理成 `MutationRef`，常见 mutation 类型包括：

```text
SetValue
ClearRange
AddValue
And
Or
Xor
AppendIfFits
CompareAndClear
```

重要的是，执行一次 `set` 并不意味着客户端立即将数据写入 Storage Server。提交前，它更像是在事务对象中追加一条指令：

```text
待提交 mutation：SET account/A = 80
```

如果事务最终取消或冲突，这些未发布的 mutations 可以直接丢弃。这也是 FoundationDB 通常不需要传统事务 Undo Log 的原因。

### 2. Read Conflict Range

事务普通读取一个 key 时，会登记相应的读冲突范围。概念上，读取：

```python
value = tr[b"account/A"]
```

会形成：

```text
["account/A", keyAfter("account/A"))
```

它表达的是：

> 如果从我的 Read Version 到提交之间，有其他成功事务写过这个范围，那么我的读取前提已经失效，事务不能提交。

范围读取也会登记对应范围：

```python
rows = tr.get_range(b"user/0000", b"user/9999")
```

可以形成：

```text
read conflict range = ["user/0000", "user/9999")
```

### 3. Write Conflict Range

写入一个 key 时，客户端会登记写冲突范围：

```text
mutation:             SET account/A = 80
write conflict range: [account/A, keyAfter(account/A))
```

范围删除则会登记整个删除范围：

```python
tr.clear_range(b"tmp/", b"tmp0")
```

```text
write conflict range = ["tmp/", "tmp0")
```

它表示：

> 如果本事务提交，它会使依赖该范围旧状态的并发事务冲突。

## 四、FDB 为什么提交冲突范围，而不是事务代码？

FoundationDB 不会把应用的事务函数发送到服务器重新执行。

例如应用代码可能是：

```python
balance = decode(await tr[b"account/A"])
tr[b"account/A"] = encode(balance - 20)
```

服务器并不知道“读取余额再减 20”这段业务逻辑。客户端发送的是已经整理好的确定性事务描述：

```text
Read Version:        100
Read Conflict Range: account/A
Write Conflict Range: account/A
Mutation:            SET account/A = 80
```

提交请求类型是：

```cpp
struct CommitTransactionRequest : TimedRequest {
    Arena arena;
    SpanContext spanContext;
    CommitTransactionRef transaction;
    ReplyPromise<CommitID> reply;
    uint32_t flags;
    Optional<UID> debugID;
    Optional<ClientTrCommitCostEstimation> commitCostEstimation;
    Optional<TagSet> tagSet;
    IdempotencyIdRef idempotencyId;
};
```

定义位于：

```text
fdbclient/include/fdbclient/CommitProxyInterface.h
```

这种设计把业务计算留在客户端，把服务端提交过程收敛为两个问题：

```text
1. 事务读取的前提是否仍然成立？
2. 如果成立，如何将确定的 mutations 可靠地纳入全局历史？
```

## 五、第三阶段：客户端发起提交

原生事务 API 的提交入口位于：

```text
fdbclient/NativeAPI.actor.cpp
```

公开入口是：

```cpp
Future<Void> Transaction::commit() {
    return commitAndWatch(this);
}
```

随后进入：

```cpp
Transaction::commitMutations()
```

客户端将当前事务的读版本、mutations 和冲突范围组装为 `CommitTransactionRequest`，再进入：

```cpp
tryCommit(...)
```

`tryCommit` 的主要工作包括：

1. 找到当前数据库代际的 Commit Proxy；
2. 发送提交请求；
3. 等待 `CommitID`；
4. 区分成功、冲突或提交结果未知；
5. 保存提交版本和事务批次编号。

Commit Proxy 的回复类型是：

```cpp
struct CommitID {
    Version version;
    uint16_t txnBatchId;
    Optional<Value> metadataVersion;
    Optional<Standalone<VectorRef<int>>> conflictingKRIndices;
};
```

其中最关键的是：

```cpp
Version version;
```

通常可以理解为：

```text
version != invalidVersion：事务提交成功
version == invalidVersion：事务发生冲突
```

## 六、Commit Proxy 为什么批量处理事务？

Commit Proxy 不会为每个客户端事务单独完成一遍冲突检测和日志持久化，而是将多个请求组成 Commit Batch：

```text
T1 ─┐
T2 ─┤
T3 ─┼── Commit Batch ── Resolver ── TLog
T4 ─┤
T5 ─┘
```

主体实现位于：

```text
fdbserver/commitproxy/CommitProxyServer.cpp
```

批处理可以：

- 减少 Resolver RPC 次数；
- 合并 TLog 消息和持久化操作；
- 摊薄同步写入成本；
- 统一决定同一批事务的顺序；
- 提高网络和 mutation 编码效率。

一个 batch 会获得 Commit Version：

```text
commitVersion = V
```

同一版本可以包含多个事务，所以 `CommitID` 还带有：

```cpp
uint16_t txnBatchId;
```

事务的逻辑提交位置可以理解为：

```text
(commitVersion, txnBatchId)
```

## 七、第四阶段：Commit Proxy 将事务发送给 Resolver

Resolver 接收的请求类型是：

```cpp
struct ResolveTransactionBatchRequest : TimedRequest {
    Version prevVersion;
    Version version;
    Version lastReceivedVersion;
    VectorRef<CommitTransactionRef> transactions;
    // ...
};
```

定义位于：

```text
fdbserver/core/include/fdbserver/core/ResolverInterface.h
```

Commit Proxy 会按照 conflict range 所在的 key 空间，将事务发送给相关 Resolver。

假设集群有多个 Resolver：

```text
Resolver 0：负责范围 A
Resolver 1：负责范围 B
Resolver 2：负责范围 C
```

如果事务同时访问范围 A 和 C，它可能需要经过 Resolver 0 与 Resolver 2：

```text
最终结果 = Resolver 0 结果 AND Resolver 2 结果
```

任何一个 Resolver 判断有冲突，整个事务都不能提交。

## 八、Resolver 如何检测冲突？

Resolver 的主处理函数位于：

```text
fdbserver/resolver/Resolver.cpp
```

入口是：

```cpp
Future<Void> resolveBatch(
    Reference<Resolver> self,
    ResolveTransactionBatchRequest req)
```

处理时会创建 `ConflictBatch`：

```cpp
ConflictBatch conflictBatch(
    self->conflictSet,
    &reply.conflictingKeyRangeMap,
    &reply.arena);
```

然后加入各事务的读写冲突范围，最后执行：

```cpp
conflictBatch.detectConflicts(
    req.version,
    newOldestVersion,
    commitList,
    &tooOldList);
```

事务主要有三种结果：

```text
TransactionCommitted
TransactionConflict
TransactionTooOld
```

### 核心冲突规则

对于事务 `T`：

```text
Read Version          = RV
Read Conflict Ranges  = R
Write Conflict Ranges = W
```

如果存在一个已经提交的事务 `U`，满足：

```text
U.commitVersion > T.readVersion
```

并且：

```text
overlap(U.writeConflictRanges, T.readConflictRanges)
```

那么 `T` 冲突。

写成公式是：

```text
conflict(T) =
    存在事务 U：
        U.commitVersion > T.readVersion
        并且 U.writeRanges 与 T.readRanges 相交
```

它判断的重点不是“两个事务是否都写了同一个 key”，而是：

> **其他事务是否修改了本事务曾经依赖的读取结果。**

## 九、转账事务如何发生冲突？

假设初始数据为：

```text
A = 100
```

两个事务都获得：

```text
Read Version = 100
```

事务 T1：

```text
读取 A = 100
写入 A = 80
```

事务 T2：

```text
读取 A = 100
写入 A = 70
```

它们提交的信息分别是：

```text
T1:
  read snapshot  = 100
  read conflict  = A
  write conflict = A
  mutation       = SET A=80

T2:
  read snapshot  = 100
  read conflict  = A
  write conflict = A
  mutation       = SET A=70
```

假设 T1 先在版本 `101` 提交，Resolver 已知：

```text
A 最近被写入的版本 = 101
```

检查 T2 时发现：

```text
T2 read version = 100
A last write version = 101
101 > 100
```

因此 T2 的读取前提已经过期：

```text
T2 = TransactionConflict
```

客户端通常在收到 `not_committed` 后重新执行整个事务，而不是只重新发送原来的 `SET A=70`。重新执行才能基于最新余额重新计算正确结果。

## 十、为什么纯写事务可能不冲突？

考虑两个事务不读取旧值，直接写入：

```text
T1: SET A = 80
T2: SET A = 70
```

如果它们没有显式添加读冲突范围，两者可能都成功，并按照提交版本形成顺序：

```text
version 101: SET A=80
version 102: SET A=70
```

最终结果是：

```text
A = 70
```

这并不违反串行化，因为两个事务都没有声明自己的写入依赖 A 的旧值。串行执行 T1、T2 也会得到相同结果。

如果业务需要 compare-and-set 或 read-modify-write 语义，就必须先执行普通读取，或者显式添加相应的读冲突范围。

因此 FoundationDB 一个非常重要的概念是：

> **事务冲突语义来自 conflict ranges，而不只是 mutations。**

## 十一、Atomic Operation 为什么能减少冲突？

普通计数器递增可能写成：

```python
value = await tr[b"counter"]
tr[b"counter"] = encode(decode(value) + 1)
```

由于读取会产生读冲突范围，多个事务并发执行时通常只有一个成功，其他事务需要重试。

使用原子 mutation：

```python
tr.add(b"counter", encode_int64(1))
```

客户端无需先读旧值，可以只提交：

```text
Mutation: ADD counter, 1
Write Conflict Range: counter
```

多个加法操作可以按提交版本依次应用：

```text
version 101: ADD counter, 1
version 102: ADD counter, 1
version 103: ADD counter, 1
```

所以 Atomic Operation 的价值不仅是减少一次读取，还包括：

- 避免不必要的读冲突；
- 提高热点计数器吞吐；
- 将可交换操作交给数据库按提交顺序应用。

## 十二、为什么会出现 TransactionTooOld？

Resolver 不可能永久保存数据库从诞生以来的全部写冲突历史。它只维护一个有限版本窗口中的 conflict set。

假设：

```text
Resolver 最早保留版本 = 1000
事务 Read Version     = 500
```

Resolver 已经无法确定版本 `(500, 1000]` 之间是否有事务写过当前事务的读取范围。此时不能冒险允许提交，只能返回：

```text
TransactionTooOld
```

源码中会将对应事务标记为：

```cpp
reply.committed[index] =
    ConflictBatchStatus::TransactionTooOld;
```

这也是 FDB 要求事务保持短小的原因之一。长事务会带来：

- MVCC 历史版本过期；
- Resolver 冲突历史过期；
- 冲突概率增大；
- 失败后的重试成本增加。

## 十三、同一个 Commit Batch 内也需要冲突检测

Resolver 不仅检查当前 batch 与历史成功事务之间的冲突，还要处理同一个 batch 内部的事务关系。

例如：

```text
T1 读取 A，写入 B
T2 读取 B，写入 C
```

如果逻辑顺序中 T1 在 T2 前面，那么 T2 读取的 B 已经被 T1 修改，不能再基于旧快照无条件提交。

`ConflictBatch` 会同时处理：

```text
历史提交产生的冲突
+
当前批次内部产生的冲突
```

通过检测的事务会进入 `commitList`，并被标记为：

```cpp
ConflictBatchStatus::TransactionCommitted
```

因此 Resolver 不只是维护一张简单的“key 最近写入版本表”，还需要构造批次内部确定的提交结果。

## 十四、Commit Proxy 如何合并多个 Resolver 的结果？

一个事务可能被多个 Resolver 检查。Commit Proxy 收到全部回复后，会合并结果：

```text
T1:
  Resolver A = committed
  Resolver B = committed
  最终 = committed

T2:
  Resolver A = committed
  Resolver B = conflict
  最终 = conflict
```

只有最终状态为 `TransactionCommitted` 的事务，其 mutations 才会进入后续日志编码。

Commit Proxy 随后还要根据 key 到 shard 的映射，为 mutation 添加对应的 Storage Server tag。

## 十五、Tag 如何把日志分发给 Storage Server？

TLog 不需要让每个 Storage Server 读取整个数据库的全部 mutation。Commit Proxy 根据 key range 的副本分布，为 mutation 添加 tag：

```text
Mutation: SET account/A = 80
Tags:
  Storage Server 1
  Storage Server 4
  Storage Server 7
```

Storage Server 从 TLog 读取时，只获取带有自己 tag 的消息：

```text
Storage Server 1：读取 SS1 tag 对应的 mutations
Storage Server 4：读取 SS4 tag 对应的 mutations
```

这样既保留了统一的事务日志顺序，又避免所有 Storage Server 接收全部写入。

## 十六、第五阶段：将成功事务写入 TLog

Commit Proxy 完成冲突判定和 mutation 路由后，会调用日志系统：

```cpp
pProxyCommitData->logSystem->push(
    versionSet,
    self->toCommit,
    span.context,
    self->debugID,
    tpcvMap);
```

相关位置：

```text
fdbserver/commitproxy/CommitProxyServer.cpp
fdbserver/logsystem/LogSystem.cpp
```

`LogSystem::push()` 会按照日志系统的复制配置，将消息发送到 TLog。核心请求类型是：

```cpp
struct TLogCommitRequest : TimedRequest {
    Version prevVersion;
    Version version;
    Version knownCommittedVersion;
    Version minKnownCommittedVersion;
    Version seqPrevVersion;

    StringRef messages;
    ReplyPromise<TLogCommitReply> reply;

    uint16_t tLogCount;
    std::vector<uint16_t> tLogLocIds;
};
```

定义位于：

```text
fdbserver/core/include/fdbserver/core/TLogInterface.h
```

其中：

| 字段 | 作用 |
|---|---|
| `prevVersion` | 前一个相关版本 |
| `version` | 当前提交版本 |
| `knownCommittedVersion` | 已知的提交进度 |
| `messages` | 编码后的 tagged mutations |
| `tLogCount` | 相关 TLog 数量 |
| `tLogLocIds` | 日志位置或副本标识 |

## 十七、什么时候才算事务提交成功？

Resolver 返回无冲突时，事务还没有真正完成提交。

Resolver 只证明：

```text
该事务可以进入串行化历史
```

Commit Proxy 还必须等待成功事务的 mutations 按集群冗余策略写入 TLog。满足持久化条件后，才向客户端返回带有效版本的 `CommitID`。

因此：

```text
冲突检查完成
    ≠
事务提交完成
```

更准确的提交边界是：

```text
成功事务的 mutations 已经可靠进入复制日志系统
```

此时 Storage Server 可以尚未写入本地数据文件：

```text
Client 收到 commit success
    ↓
TLog 已可靠持久化
    ↓
Storage Server 可能仍在追赶
```

这正是 TLog 类似分布式 Redo Log 的地方。

## 十八、TLog 如何完成本地持久化？

TLog 的主要实现位于：

```text
fdbserver/tlog/TLogServer.cpp
```

收到 `TLogCommitRequest` 后，TLog 将消息加入持久化队列，并调用类似：

```cpp
self->persistentQueue->commit();
```

只有日志达到相应持久化进度，才返回：

```cpp
struct TLogCommitReply {
    Version version;
};
```

单个 TLog 节点故障不必然导致事务丢失，因为 LogSystem 会根据集群的冗余模式，将日志放入多个故障域。

概念上，一个三副本日志系统可能是：

```text
TLog A：zone 1
TLog B：zone 2
TLog C：zone 3
```

具体成功条件由日志复制策略决定，而不是简单固定为“所有 TLog 都返回成功”。

## 十九、第六阶段：Storage Server 异步读取日志

Storage Server 通常不参与普通事务的冲突判定，也不是客户端提交 RPC 中的 prepare participant。

它持续从 TLog peek 与自己 tag 对应的消息。主要更新逻辑位于：

```text
fdbserver/storageserver/storageserver.cpp
```

获取 mutation 后，会按版本应用：

```cpp
updater.applyMutation(
    data,
    msg,
    ver,
    false);
```

Storage Server 维护多个重要版本进度：

```text
version/currentVersion：已经从 TLog 接收并应用到内存的版本
durableVersion：已经持久化到本地 Storage Engine 的版本
```

所以系统中可能正常出现：

```text
TLog durable version   = 1200
Storage Server version = 1198
Storage durableVersion = 1180
```

这些进度不相等并不意味着数据不一致，而是说明写入处于流水线中的不同阶段。

## 二十、Storage Server 如何回答指定版本的读取？

假设客户端获得：

```text
Read Version = 1195
```

负责该 key 的 Storage Server 必须至少追赶到版本 `1195`，才能正确回答请求。

如果它当前只应用到：

```text
version = 1190
```

就需要等待从 TLog 获取后续 mutations：

```text
wait until version >= 1195
```

然后从版本化数据中返回版本 `1195` 的快照。

因此读取正确性依赖两个条件：

```text
1. 客户端请求携带明确的 Read Version
2. Storage Server 回答前已经应用到该版本
```

Storage Server 不会用版本 `1190` 的旧状态假装回答版本 `1195` 的请求。

## 二十一、Storage Server 如何写入本地存储？

Storage Server 的后台持久化循环是：

```cpp
Future<Void> updateStorage(StorageServer* data)
```

位于：

```text
fdbserver/storageserver/storageserver.cpp
```

它会将内存中的版本化 mutations 批量提交到本地 Storage Engine，然后推进 `durableVersion`：

```text
从 TLog 获取 mutation
    ↓
应用到内存版本
    ↓
批量提交本地 Storage Engine
    ↓
推进 durableVersion
    ↓
允许回收旧状态和对应日志
```

客户端事务提交不需要同步等待这一过程。若 Storage Server 在本地持久化前崩溃，可以从自己的 durableVersion 之后继续读取 TLog，恢复缺失的 mutations。

## 二十二、MVCC 如何提供历史版本？

Storage Server 保存版本化状态，使客户端可以按 Read Version 读取。

假设提交历史是：

```text
version 100: A = 100
version 105: A = 80
version 110: A = 60
```

不同事务会看到：

| Read Version | A 的值 |
|---:|---:|
| 102 | 100 |
| 107 | 80 |
| 115 | 60 |

FoundationDB 不需要为每个版本完整复制一份数据库。Storage Server 使用当前数据和有限的版本化变更，构造目标版本的读取结果。

当系统确认没有事务再需要某些旧版本时，可以推进最老可见版本并清理历史状态。这也是事务无法无限运行的另一个原因。

## 二十三、FDB 如何保证严格可串行化？

FoundationDB 主要通过四个机制建立严格可串行化语义。

### 1. 全局提交顺序

每个成功事务获得一个逻辑提交位置：

```text
(commitVersion, txnBatchId)
```

成功事务因此可以放入一个确定的全局顺序。

### 2. 一致性快照读

事务中的普通读取基于同一个 Read Version。

### 3. Resolver 冲突检测

如果事务读取的内容在 Read Version 之后被修改，事务不能提交。

### 4. 成功回复前日志持久化

只有 mutation 达到 TLog 持久化要求，客户端才收到提交成功。

因此，一个成功事务可以被逻辑地串行化在自己的 Commit Version 上。

## 二十四、一笔转账的完整路径

初始状态：

```text
version 100:
A = 100
B = 50
```

应用事务：

```python
@fdb.transactional
def transfer(tr):
    a = decode(tr[b"A"].wait())
    b = decode(tr[b"B"].wait())

    tr[b"A"] = encode(a - 20)
    tr[b"B"] = encode(b + 20)
```

### 客户端阶段

事务获取：

```text
Read Version = 100
```

读取：

```text
A@100 = 100
B@100 = 50
```

本地事务状态变为：

```text
mutations:
  SET A = 80
  SET B = 70

read conflict ranges:
  A
  B

write conflict ranges:
  A
  B
```

### Commit Proxy 阶段

Commit Proxy 将事务放入 batch，并为 batch 分配：

```text
Commit Version = 110
```

### Resolver 阶段

Resolver 检查：

```text
在版本 (100, 110] 内，是否有成功事务写过 A 或 B？
```

如果没有：

```text
TransactionCommitted
```

如果有人在版本 `105` 写过 A：

```text
TransactionConflict
```

### TLog 阶段

通过冲突检测后写入：

```text
version 110:
  SET A = 80
  SET B = 70
```

TLog 达到持久化要求后，客户端收到：

```text
Commit Version = 110
```

### Storage Server 阶段

负责 A、B 的 Storage Server 异步获取并应用：

```text
A = 80
B = 70
```

即使 Storage Server 在应用前崩溃，版本 `110` 的 mutations 仍然可以从 TLog 恢复。

## 二十五、为什么 commit_unknown_result 特别危险？

客户端发送事务后，可能发生：

```text
事务已经成功写入 TLog
但成功回复在网络中丢失
```

客户端此时无法确定事务到底成功还是失败，只能得到：

```text
commit_unknown_result
```

这与明确的 `not_committed` 不同：

```text
not_committed：
确定事务没有提交，可以重新执行。

commit_unknown_result：
事务可能已经提交，直接重试可能重复产生业务效果。
```

例如转账已经成功，但客户端因超时再次执行一次，就可能重复扣款。

提交请求中包含：

```cpp
IdempotencyIdRef idempotencyId;
```

新版 FoundationDB 可以利用相关机制改进幂等提交处理，但应用仍应理解“结果未知”与“确定失败”的差异，并为关键业务设计幂等标识或结果确认流程。

## 二十六、为什么它不是经典两阶段提交？

FDB 的流程容易被误认为 2PC：

```text
Resolver 检查
TLog 提交
```

但经典两阶段提交通常是：

```text
Coordinator
  ├── Participant A: prepare
  ├── Participant B: prepare
  └── Participant C: prepare

全部 prepare 后再发送 commit
```

FoundationDB 普通事务中：

- Storage Server 不是 prepare participant；
- Resolver 只检查冲突，不保存每笔业务的待定数据；
- TLog 持久化已经确定的 mutation 流；
- Storage Server 之后按版本异步应用；
- 不需要为每个事务在所有 shard 上维护 prepare 状态。

因此更准确的描述是：

> **事务经过集中排序和冲突检测后，确定的 mutations 被原子地纳入复制日志。**

## 二十七、这种事务方案的优势和代价

### 优势

- 跨 shard 事务不需要所有 Storage Server 参与 2PC；
- 客户端提交内容确定，适合编码为 mutation 日志；
- 冲突检测、日志持久化与数据存储彼此解耦；
- Commit Batch 可以提高 Resolver 和 TLog 吞吐；
- Storage Server 可以异步持久化，缩短提交关键路径；
- 全局版本统一了 MVCC、冲突判断和故障恢复；
- 节点故障不会改变已经确定的事务提交顺序。

### 代价

- 所有事务依赖全局版本体系；
- Resolver 承担集中式冲突检测压力；
- 热 key 上的 read-modify-write 容易频繁冲突；
- 事务时长受 MVCC 和 conflict set 历史窗口限制；
- `commit_unknown_result` 要求应用考虑幂等性；
- 大事务会增加 Client、Commit Proxy、Resolver 和 TLog 压力；
- 涉及大量 key range 的事务可能需要多个 Resolver 共同检查。

## 二十八、总结

FoundationDB 的事务主路径可以压缩为五步：

```text
1. GRV Proxy 为事务分配 Read Version
2. 客户端按该版本读取，并积累 mutations 与 conflict ranges
3. Commit Proxy 组成批次、分配 Commit Version，并发送给 Resolver
4. Resolver 检查 Read Version 之后是否存在相交写入
5. 成功事务写入多副本 TLog，持久化后回复客户端
```

随后由 Storage Server 异步完成：

```text
6. 从 TLog 获取自己的 tagged mutations
7. 按版本应用到 MVCC 内存状态
8. 后台写入本地 Storage Engine
```

整个方案最核心的职责划分是：

```text
Client：       保存未提交修改和事务依赖
GRV Proxy：    给出一致性快照版本
Resolver：     判断读取前提是否仍然成立
Commit Proxy： 决定批次、版本和全局提交顺序
TLog：         保证已经提交的 mutations 不丢失
Storage Server：异步生成可查询、可持久化的数据状态
```

因此，FoundationDB 既不是传统“数据页 + Undo/Redo”事务方案，也不是标准参与者式 2PC，而是：

> **以版本化 mutation 流为核心，通过乐观冲突检测和复制日志实现严格可串行化事务。**

## 参考源码

- [`fdbclient/NativeAPI.actor.cpp`](https://github.com/apple/foundationdb/blob/main/fdbclient/NativeAPI.actor.cpp)：客户端读版本、事务组装和提交流程
- [`fdbclient/include/fdbclient/CommitTransaction.h`](https://github.com/apple/foundationdb/blob/main/fdbclient/include/fdbclient/CommitTransaction.h)：`CommitTransactionRef` 等事务数据结构
- [`fdbclient/include/fdbclient/CommitProxyInterface.h`](https://github.com/apple/foundationdb/blob/main/fdbclient/include/fdbclient/CommitProxyInterface.h)：`CommitTransactionRequest` 与 `CommitID`
- [`fdbserver/grvproxy/GrvProxyServer.cpp`](https://github.com/apple/foundationdb/blob/main/fdbserver/grvproxy/GrvProxyServer.cpp)：Read Version 分配
- [`fdbserver/commitproxy/CommitProxyServer.cpp`](https://github.com/apple/foundationdb/blob/main/fdbserver/commitproxy/CommitProxyServer.cpp)：Commit Batch、Resolver 汇总与日志提交
- [`fdbserver/resolver/Resolver.cpp`](https://github.com/apple/foundationdb/blob/main/fdbserver/resolver/Resolver.cpp)：Resolver 主流程
- [`fdbserver/core/include/fdbserver/core/ConflictBatch.h`](https://github.com/apple/foundationdb/blob/main/fdbserver/core/include/fdbserver/core/ConflictBatch.h)：批量冲突检测
- [`fdbserver/logsystem/LogSystem.cpp`](https://github.com/apple/foundationdb/blob/main/fdbserver/logsystem/LogSystem.cpp)：日志复制与 `push()`
- [`fdbserver/tlog/TLogServer.cpp`](https://github.com/apple/foundationdb/blob/main/fdbserver/tlog/TLogServer.cpp)：TLog 消息持久化
- [`fdbserver/storageserver/storageserver.cpp`](https://github.com/apple/foundationdb/blob/main/fdbserver/storageserver/storageserver.cpp)：mutation 获取、MVCC 应用和本地持久化
