---
title: "FoundationDB 中有 Undo 和 Redo 吗？从 TLog、MVCC 到故障恢复"
date: 2026-08-14T10:00:00+08:00
draft: false
tags: ["FoundationDB", "数据库", "事务", "Undo", "Redo", "MVCC", "分布式系统"]
categories: ["技术"]
---

传统数据库通过 Undo Log 和 Redo Log 保证事务的原子性与持久性：Undo 撤销未提交的修改，Redo 恢复已经提交但尚未写入数据文件的修改。那么，在采用分布式事务架构的 FoundationDB 中，是否也存在这两类日志？

简短回答是：

> **FoundationDB 有承担类似 Redo 职责的 TLog，但通常没有传统意义上的 Undo Log。**

这并不意味着 FoundationDB 不支持事务回滚或 MVCC，而是因为它对未提交数据、提交日志和历史版本的组织方式与传统数据库不同。

## 一、先回顾传统数据库为什么需要 Undo 和 Redo

传统数据库通常允许事务直接修改缓冲池中的共享数据页。数据页何时写入磁盘，与事务何时提交并不完全同步。

因此会出现两种状态。

### 1. 事务未提交，数据页却已经落盘

例如事务把账户余额从 `100` 改成 `80`，但随后事务失败：

```sql
BEGIN;

UPDATE account
SET balance = 80
WHERE id = 'A';

ROLLBACK;
```

如果包含 `80` 的脏页已经写入磁盘，数据库就必须知道原值是 `100`，才能撤销这次修改。这是 Undo Log 的主要职责。

### 2. 事务已经提交，数据页却尚未落盘

另一个事务已经执行 `COMMIT`，但修改可能仍然只存在于内存中的脏页里。如果服务器此时断电，已提交的数据就会丢失。

因此数据库先持久化 Redo Log，再返回提交成功。重启后，即使数据页没有及时落盘，也可以通过 Redo 重新应用修改。

可以把二者概括为：

```text
Undo：事务不应该生效，但修改可能已经进入数据文件。
Redo：事务应该生效，但修改可能还没有进入数据文件。
```

## 二、FoundationDB 的事务提交流程

FoundationDB 并不是把客户端事务中的每次 `set` 或 `clear` 立即写入 Storage Server。事务提交前，这些操作首先保存在客户端的事务对象中。

一次提交可以简化为：

```text
客户端积累读写集合和 mutations
        ↓
向事务系统发起提交
        ↓
Commit Proxy 分配提交版本并组织提交
        ↓
Resolver 检查读写冲突
        ↓
mutations 写入多个 TLog 副本
        ↓
满足持久化与复制条件
        ↓
向客户端返回提交成功
        ↓
Storage Server 异步获取并应用 mutations
```

这里最重要的区别是：

> 在事务成功提交以前，它的修改不会作为正式的数据库版本发布给 Storage Server 和其他事务。

如果事务因为冲突、超时、进程故障或用户主动取消而失败，FoundationDB 通常只需要丢弃客户端尚未提交的 mutations，而不需要从共享数据页中恢复旧值。

## 三、TLog 就是 FoundationDB 的 Redo Log 吗？

FoundationDB 的 Transaction Log，简称 **TLog**，承担了与 Redo Log 非常相似的职责。

假设事务在版本 `100` 提交：

```text
version 100
SET account/A = 80
```

当事务返回提交成功时，这条 mutation 已经按照集群的冗余策略写入 TLog，但相关 Storage Server 可能还没有将它应用到本地存储引擎。

如果 Storage Server 此时重启，它可以根据自己最后持久化的版本，继续从日志系统获取缺失的 mutations：

```text
Storage Server 最后持久化版本：95

重新获取并应用：
version 96
version 97
version 98
version 99
version 100
```

这与传统 Redo 的思想一致：

```text
传统数据库：
Redo 已持久化，数据页可以稍后刷盘。

FoundationDB：
TLog 已持久化，Storage Server 可以稍后应用 mutation。
```

因此可以近似理解为：

```text
FDB TLog ≈ 分布式复制的 Redo Log
```

但二者并不完全等价。传统 Redo Log 通常是单个存储引擎内部的数据页恢复日志；TLog 则是 FoundationDB 分布式事务系统的一部分，同时承担以下职责：

- 持久化已经提交的 mutations；
- 将事务系统与 Storage Server 的异步应用过程解耦；
- 在节点故障后重新提供尚未持久化到 Storage Server 的数据；
- 通过多副本和故障域策略满足集群级持久性要求；
- 参与整个数据库代际切换和故障恢复过程。

所以 TLog 不只是“某个节点的本地 WAL”，而是分布式提交路径上的核心组件。

## 四、FoundationDB 为什么通常不需要传统 Undo Log？

传统 Undo 存在的前提是：

```text
未提交事务可以修改共享数据页，甚至把修改写入磁盘。
```

FoundationDB 避免了这个前提。事务提交以前，修改主要保存在客户端事务对象中，不会作为一个正式版本进入 Storage Server。

如果提交失败：

```text
丢弃客户端 mutations
```

而不是：

```text
找到所有被修改的数据
根据旧值逐项恢复
```

例如客户端执行：

```python
transaction["account/A"] = "80"
```

这次赋值在提交前只是事务中的一个 mutation。若冲突检测失败，数据库并没有一个已经对外可见的 `account/A = 80` 需要撤销。

从设计思路上看，它接近一种 **未提交修改不进入正式存储状态** 的策略：

```text
传统数据库：
先修改共享缓冲页，失败后通过 Undo 恢复。

FoundationDB：
提交前不发布修改，失败后直接丢弃。
```

这也是 FoundationDB 不需要为分布式事务维护传统 Undo Log 的根本原因。

## 五、没有 Undo，事务还能回滚吗？

可以，但要区分提交前和提交后。

### 1. 提交前回滚

事务尚未提交时，可以取消、重置或丢弃事务对象。因为 mutations 还没有成为数据库正式版本，所以不需要执行反向写入。

这相当于：

```text
取消一份尚未生效的修改计划
```

而不是：

```text
撤销一批已经写入数据库的数据
```

### 2. 提交后撤销

事务一旦成功提交，就已经成为全局版本历史的一部分，不能再对原事务执行传统意义上的 `ROLLBACK`。

如果业务需要撤销，只能再提交一个补偿事务：

```text
version 100：account/A = 80
version 101：account/A = 100
```

版本 `101` 没有删除版本 `100` 曾经发生过的事实，而是产生了一个新的数据库状态。

这一点与关系型数据库相同：`COMMIT` 成功后，也只能通过新的反向事务补偿，不能重新回滚已经提交的事务。

## 六、FoundationDB 的 MVCC 是否依赖 Undo？

FoundationDB 支持 MVCC，但它不是典型的 InnoDB Undo 版本链模式。

在 InnoDB 中，一条记录的当前版本可以通过回滚指针找到 Undo 中的旧版本：

```text
当前记录：80
    ↓ roll pointer
旧版本：100
    ↓
更旧版本
```

FoundationDB 的事务开始读取时，会获取一个 **Read Version**。之后，事务请求 Storage Server 读取该版本对应的数据快照：

```text
事务读取版本：100

Storage Server：
返回 key 在 version 100 时的值
```

Storage Server 按提交版本接收和应用 mutations，并在有限的历史窗口中维护服务旧版本读取所需的状态。概念上可以表示为：

```text
version 100：account/A = 100
version 101：account/A = 80
version 102：account/A = 60
```

读取版本 `100` 的事务看到 `100`，读取版本 `102` 的事务看到 `60`。

因此：

> FoundationDB 有 MVCC 历史版本，但这些历史版本不能简单称为 Undo Log。

二者都能帮助读取旧数据，但目的和组织方式不同：

| 机制 | 主要用途 |
|---|---|
| 传统 Undo Log | 撤销未提交修改，并可能为 MVCC 构造旧版本 |
| FDB 版本化状态 | 为指定 Read Version 提供一致性快照 |

FoundationDB 对旧版本的保留是有限的。事务运行过久，可能因为所需版本已经超出可读取窗口而失败。因此，FDB 应用通常应保持事务短小，并在出现可重试错误时重新执行事务。

## 七、Storage Engine 自己会不会有 WAL？

这里必须区分两个层次。

### 1. FoundationDB 分布式事务层

这一层包含：

- Commit Proxy；
- Resolver；
- TLog；
- Storage Server；
- 全局提交版本；
- mutation 分发与恢复。

在这一层，TLog 最接近 Redo，而传统 Undo 通常不是必要组件。

### 2. Storage Server 本地存储引擎

Storage Server 最终还要把数据写入本地存储引擎。不同引擎可能采用不同的持久化方式，例如：

- Write-Ahead Log；
- Copy-on-Write；
- 原子页面提交；
- 检查点；
- 版本化 B-tree；
- 引擎内部的恢复日志。

这些机制属于本地 Storage Engine 的实现细节。即使某个引擎内部使用 WAL，也不能把它与 FoundationDB 的 TLog 混为一谈。

可以把两层关系理解为：

```text
FoundationDB TLog
    负责集群级已提交 mutation 的持久化与分发

Storage Engine 本地日志
    负责单个 Storage Server 本地文件状态的一致性
```

它们解决的问题层次不同。

## 八、故障恢复时具体会发生什么？

假设数据库中存在三个事务：

```text
T1：已提交，Storage Server 尚未应用
T2：正在客户端执行，尚未提交
T3：已提交，Storage Server 已经持久化
```

此时集群发生故障。

### 对 T1

T1 已经写入 TLog 并满足提交条件，因此它不能丢失。恢复系统需要确认已提交边界，并让 Storage Server 重新获取、应用相关 mutations。

效果类似于 Redo：

```text
重新应用 T1
```

### 对 T2

T2 尚未成功提交，它的 mutations 仍属于客户端事务状态。客户端连接中断后，这些修改直接消失，不需要数据库执行 Undo。

```text
丢弃 T2
```

### 对 T3

T3 已经被 Storage Server 持久化，不需要再次产生业务效果。系统通过版本和持久化进度判断从哪里继续处理，而不是重新运行原始应用逻辑。

恢复完成后：

```text
T1：保留
T2：不生效
T3：保留
```

这与传统数据库崩溃恢复想要达到的最终结果一致，只是实现路径不同。

## 九、TLog 重放不是重新执行事务代码

需要特别注意，恢复 mutations 不等于重新运行用户事务。

假设原事务逻辑是：

```text
读取余额
余额减 20
写入新余额
```

FoundationDB 不会在恢复时再次执行“读取后减 20”，否则每次重放都可能再次扣款。

TLog 保存的是事务提交后确定的 mutations，并带有对应的提交版本。Storage Server 按版本应用这些确定的变更。

可以将其理解为：

```text
不安全的重放：
balance = balance - 20

确定性的 mutation：
在提交版本 100 写入事务已经确定的结果
```

版本化和持久化进度也使 Storage Server 能判断哪些 mutations 已经处理、哪些仍然缺失。

## 十、与传统数据库的对照

| 能力 | 传统数据库 | FoundationDB |
|---|---|---|
| 提交前修改位置 | 共享缓冲池中的数据页 | 主要在客户端事务对象中 |
| 未提交数据是否可能进入正式存储 | 可能 | 通常不会成为正式版本 |
| 撤销未提交事务 | Undo Log | 丢弃未提交 mutations |
| 已提交修改持久化 | Redo Log / WAL | 多副本 TLog |
| 后台应用修改 | 刷新脏页 | Storage Server 获取并应用 mutations |
| MVCC 读取 | 常通过 Undo 版本链构造旧版本 | 按 Read Version 读取版本化状态 |
| 提交后撤销 | 新建补偿事务 | 新建补偿事务 |
| 恢复进度标识 | LSN、页面版本等 | 全局提交版本和持久化版本 |

这个对照表里最关键的一行是：

```text
传统 Undo：修改已经进入共享状态，所以需要反向恢复。
FDB：修改尚未发布，所以提交失败时直接丢弃。
```

## 十一、这种设计带来了什么？

### 1. 简化未提交事务的恢复

提交失败不需要在多个 Storage Server 上协调反向操作。事务对正式数据库状态要么以某个版本提交，要么根本没有出现。

### 2. 提交路径与存储路径解耦

事务不必等待所有相关 Storage Server 完成本地持久化后才能返回成功。只要 mutations 在事务日志系统中满足持久化要求，Storage Server 就可以异步追赶。

### 3. 适合分布式故障恢复

TLog 不是单机日志，而是具备复制和故障域约束的分布式组件。节点故障时，系统可以从存活副本确认已提交历史并恢复数据流。

### 4. 对事务时长提出限制

FoundationDB 不会无限保留任意旧版本。长事务不仅更容易产生冲突，也可能超出 MVCC 版本窗口。应用应尽量缩短事务，并通过重试循环处理可重试错误。

## 十二、总结

回答“FoundationDB 中有没有 Undo 和 Redo”，不能只看组件名称，而要看它们解决的问题。

FoundationDB 中：

```text
TLog ≈ 分布式、复制的 Redo Log
MVCC 历史版本 ≠ 传统 Undo Log
未提交 mutations 直接丢弃，因此通常不需要传统 Undo
```

它与传统数据库的核心区别是：

```text
传统数据库：
允许未提交修改进入共享数据页，失败后再通过 Undo 恢复。

FoundationDB：
提交前不发布修改，失败后直接丢弃事务状态。
```

而对于已经提交的事务：

```text
传统数据库依靠 Redo 保证不丢失；
FoundationDB 依靠多副本 TLog 保证 mutations 可恢复。
```

所以，从事务语义看，两者都要实现“失败的事务不生效、成功的事务不丢失”；从内部实现看，FoundationDB 用版本化 mutations、TLog 和 Storage Server 的异步应用机制，替代了经典的 Undo/Redo 组合。

## 参考资料

- [FoundationDB Transaction Processing](https://apple.github.io/foundationdb/transaction-processing.html)
- [FoundationDB Key-Value Store Architecture](https://apple.github.io/foundationdb/kv-architecture.html)
- [FoundationDB Recovery Internals](https://github.com/apple/foundationdb/blob/main/design/recovery-internals.md)
- [FoundationDB Configuration](https://apple.github.io/foundationdb/configuration.html)
