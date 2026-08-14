---
title: "3FS 如何使用 FoundationDB：元数据模型、事务与并发控制"
date: 2026-08-14T19:10:00+08:00
draft: false
tags: ["3FS", "FoundationDB", "分布式文件系统", "元数据", "事务"]
categories: ["技术"]
---

3FS 是面向 AI 训练和推理负载设计的分布式文件系统。它没有把 FoundationDB 当成普通的持久化 KV 使用，而是围绕 FoundationDB 的有序键空间、乐观事务、冲突检测和 Versionstamp，构建了一套强一致的文件系统元数据服务。

本文从源码出发，梳理 3FS 如何接入 FoundationDB，如何编码 inode 和目录项，以及 create、rename、remove、list 等文件系统操作怎样映射为 FoundationDB 事务。

本文分析的源码位于 3FS 仓库中的以下目录：

```text
src/fdb/             FoundationDB C API 封装
src/common/kv/       通用 KV 和事务接口
src/meta/store/      inode、目录项和元数据操作
src/meta/components/ ID 分配、服务分布和 GC 等组件
src/meta/service/    Meta Service RPC 入口
```

## 一、3FS 中 FoundationDB 的定位

3FS 将数据面和元数据面分开：

- 文件的 chunk 数据由 Storage Service 保存。
- 文件系统元数据由 Meta Service 管理，并持久化到 FoundationDB。

FoundationDB 中保存的主要内容包括：

- inode；
- directory entry，也就是目录项；
- 文件打开会话；
- RPC 幂等记录；
- Meta Server 分布信息；
- 用户、配置和其他全局状态。

整体调用链可以简化为：

```text
FUSE / Client
      │ RPC
      ▼
MetaOperator
      │
OperationDriver：事务、提交、重试、幂等
      │
MetaStore Operation：open / rename / remove / list
      │
Inode / DirEntry / FileSession：键和值的编码
      │
IKVEngine / IReadWriteTransaction
      │
FDBKVEngine / FDBTransaction
      │
FoundationDB C API
```

这里最重要的一点是：**上层元数据代码不直接依赖 FoundationDB C API**。3FS 先抽象出通用 KV 接口，再由 FDB 实现这些接口。这既隔离了数据库细节，也允许单元测试使用内存 KV。

## 二、FoundationDB 接入层

### 1. FDBContext：管理数据库运行环境

`FDBContext` 负责 FoundationDB 客户端的全局初始化和销毁，主要工作包括：

1. 选择 FoundationDB API Version；
2. 配置 external client 和多版本客户端；
3. 调用 `fdb_setup_network()`；
4. 启动独立的 `fdb_net` 线程执行 `fdb_run_network()`；
5. 根据 cluster file 创建数据库连接；
6. 进程退出时停止网络线程。

因此，3FS 的 coroutine 并没有替代 FoundationDB 的网络模型。FDB 网络线程负责推进 `FDBFuture`，3FS coroutine 则异步等待 Future 完成，再把结果转换为自己的 `CoTryTask`。

### 2. HybridKvEngine：统一 FDB 和内存 KV

3FS 定义了三个核心接口：

```cpp
IKVEngine
IReadOnlyTransaction
IReadWriteTransaction
```

生产环境通常使用 `FDBKVEngine`，测试时则可以使用 `MemKVEngine`。两者通过 `HybridKvEngine` 统一创建。

当开启 FoundationDB multiple client 配置时，`HybridKvEngine` 会创建多个数据库 handle，并在创建事务时随机选择一个：

```text
Meta request
     │
     ▼
HybridKvEngine::pick()
     │
     ├── FDB client 0
     ├── FDB client 1
     └── FDB client N
```

这不是把数据分片到多个 FDB 集群，而是让同一个 FDB 集群可以利用更多客户端网络线程和连接资源。

### 3. FDBTransaction：把 C API 转换为 coroutine

`FDBTransaction` 对外提供的主要接口包括：

```cpp
snapshotGet(key)
get(key)
snapshotGetRange(begin, end)
getRange(begin, end)
addReadConflict(key)
addReadConflictRange(begin, end)
set(key, value)
clear(key)
setVersionstampedValue(...)
commit()
reset()
```

这些接口最终分别调用 `fdb_transaction_get`、`fdb_transaction_get_range`、`fdb_transaction_set`、`fdb_transaction_commit` 等 FoundationDB C API。

上层 MetaStore 因此只需要面对 coroutine 和统一的 `Result`，不需要直接处理 `FDBFuture`、回调和错误码。

## 三、元数据如何映射到有序键空间

3FS 没有使用 FoundationDB Directory Layer，而是自行定义固定长度的键前缀。

主要前缀如下：

| 前缀 | 含义 |
| --- | --- |
| `INOD` | inode |
| `DENT` | directory entry |
| `INOS` | inode/file session |
| `IDEM` | 请求幂等记录 |
| `META` | Meta Server 分布信息 |
| `USER` | 用户信息 |
| `SING` | 全局单键 |
| `CONF` | 配置 |

固定前缀把不同类型的数据隔离到不同子空间，同时保留 FoundationDB 的字典序排列能力。

### 1. Inode

inode 的键可以抽象为：

```text
INOD + encoded_inode_id
```

值是序列化后的 `Inode` 对象，包含：

```text
INOD/<inode-id> -> {
    type,
    acl,
    timestamps,
    nlink,
    file-layout | directory-info | symlink-info,
    ...
}
```

每个 inode 可以通过 inode ID 做一次精确 KV 查询。文件、目录和符号链接共用同一套 inode 键空间，通过 inode 中的类型和 variant 数据区分。

### 2. Directory Entry

目录项的键可以抽象为：

```text
DENT + parent_inode_id + filename
```

假设 inode 100 是一个目录，它的键空间可能是：

```text
DENT/<inode-100>/a.txt -> inode-101
DENT/<inode-100>/b.txt -> inode-102
DENT/<inode-100>/dir   -> inode-103
```

同一个目录下的全部目录项会连续排列。因此列目录只需扫描：

```text
[DENT/<parent>/, prefixEnd(DENT/<parent>/))
```

这种设计同时支持：

- 根据 `parent + filename` 精确查找；
- 按目录执行范围扫描；
- 使用 `prev + limit` 做分页；
- 读取一条记录判断目录是否为空；
- 让不同文件名的冲突落到不同键上。

这正是文件系统目录结构与 FoundationDB 有序 KV 最自然的结合点。

### 3. FileSession

文件打开会话的键可以抽象为：

```text
INOS + inode_id + session_uuid
```

把 inode ID 放在 session UUID 前面，可以通过范围查询列出某个 inode 的所有打开会话，用于 close、prune session 和 GC 等操作。

### 4. 幂等记录

幂等记录的键可以抽象为：

```text
IDEM + request_uuid + client_uuid
```

值中保存：

- client ID；
- request ID；
- 执行时间；
- 第一次请求的返回结果。

代码特意将 request ID 放在 client ID 前面，使不同请求更均匀地散布在键空间中，避免大量客户端请求集中到相邻键范围。

## 四、Snapshot Read 与显式冲突控制

FoundationDB 的普通读和 Snapshot Read 有一个关键区别：

| 读取方式 | 是否加入读冲突集合 | 典型用途 |
| --- | --- | --- |
| `get()` / `getRange()` | 是 | 读取后依据结果写入 |
| `snapshotGet()` / `snapshotGetRange()` | 否 | stat、list、路径遍历 |

如果路径解析过程中读取的每一个祖先 inode 和目录项都进入读冲突集合，那么任意一个祖先目录发生变化，都可能导致当前写事务提交失败。

3FS 的优化方式是：

```text
使用 snapshot read 解析路径
            │
            ▼
完成权限检查和对象定位
            │
            ▼
只为真正影响正确性的键添加 read conflict
            │
            ▼
写入并提交
```

例如创建文件时，路径遍历可以使用 Snapshot Read，但提交前会显式保护：

- parent inode；
- 目标 `parent + filename` 对应的 dentry 键。

如果另一个事务同时修改父目录，或者创建了同名目录项，本事务就会在 commit 阶段发生冲突。

这种做法将“读取数据”和“参与并发控制”拆开，使事务冲突范围保持在最小必要集合。

## 五、一次 Meta Operation 如何运行

所有元数据操作都由 `OperationDriver` 驱动。可以将它的核心逻辑简化为：

```cpp
while (true) {
    result = operation.run(txn);

    if (result.ok()) {
        txn.commit();
        return result;
    }

    if (!retryable(result)) {
        return result;
    }

    txn.reset();
    backoff();
}
```

实际实现还会处理：

- 请求 deadline；
- Meta Service readonly 模式；
- FoundationDB GRV cache；
- `commit_unknown_result`；
- 幂等记录；
- operation 的 retry 回调；
- 指标、日志和 trace。

这里有一个非常重要的原则：

> 事务失败后，3FS 重新执行完整的 Meta Operation，而不是只重新调用 commit。

事务 reset 后，原来的读版本和读取结果已经不能继续使用。路径解析、权限判断和目录项状态都必须基于最新数据库版本重新执行。

## 六、典型文件系统操作

### 1. 创建文件

创建一个新文件时，同一个 FoundationDB 事务中大致完成：

1. 解析父目录；
2. 检查访问权限；
3. 分配 inode ID；
4. 为 parent inode 添加读冲突；
5. 为目标 dentry 添加读冲突；
6. 写入新 dentry；
7. 写入新 inode；
8. 写入 file session；
9. 提交事务。

假设两个客户端同时创建 `/dir/a`：

```text
T1 read conflict: DENT/dir/a
T2 read conflict: DENT/dir/a

T1 commit success
T2 commit -> not_committed
T2 reset and retry
T2 sees a already exists
```

整个过程不需要额外的分布式锁。FoundationDB 的乐观事务负责识别并发写入，3FS 的重试逻辑负责重新执行操作。

### 2. Rename

rename 是 FoundationDB 事务能力最有价值的场景之一。跨目录 rename 需要同时修改多个逻辑对象：

- 源目录的 dentry；
- 目标目录的 dentry；
- 源 parent inode；
- 目标 parent inode；
- 被移动 inode；
- 必要时被覆盖的目标 inode。

3FS 在一个事务中完成：

```text
检查源路径和目标路径
        │
        ├── 检查目标目录是否为空
        ├── 删除源 dentry
        ├── 必要时删除或更新目标 inode
        ├── 创建目标 dentry
        └── 更新 inode 和父目录关系
                     │
                     ▼
                  commit
```

事务提交要么全部生效，要么全部不生效。因此跨目录 rename 不需要自行实现日志、两阶段提交或跨节点锁协议。

### 3. 删除空目录

删除目录前必须保证目录为空。3FS 对目标目录的 dentry 前缀执行范围读取，并限制最多读取一条：

```text
[DENT/<directory>/, prefixEnd(DENT/<directory>/))
```

如果范围为空，则继续删除该目录对应的 dentry 和 inode。

因为这里使用的是非 Snapshot Range Read，读取范围会进入事务的 read conflict ranges。假设检查完成后，另一个事务向目录插入新文件，那么删除事务在 commit 时会发生冲突，不会把一个已经变成非空的目录删除掉。

### 4. 递归删除

递归删除可能涉及大量 inode 和 dentry，不适合放进一个 FoundationDB 事务：

- 事务可能持续过久；
- 读写集合可能过大；
- 与前台元数据操作冲突的概率会快速上升。

3FS 因此不会试图用一个超大事务删除整棵目录树，而是先原子地移动或标记待删除对象，再交给 GC Manager 分批清理。

这体现了使用 FoundationDB 时的一个重要边界：**事务适合短小、确定的元数据原子操作，不适合承载无限扩张的后台任务。**

### 5. List

列目录主要是只读操作，3FS 使用 Snapshot Range Read：

```text
snapshot resolve directory inode
            │
            ▼
snapshot range scan DENT/<directory>/
            │
            ▼
按需并发加载每个 entry 对应的 inode
```

这样即使目录很大，list 也不会因为读取大量 dentry 而给其他写事务制造读写冲突。

## 七、事务重试与 Maybe Committed

`FDBRetryStrategy` 统一处理 FoundationDB 事务错误。处理过程大致是：

1. 判断错误是否来自 FoundationDB 事务；
2. 判断错误是否可重试；
3. 根据 operation 配置判断是否允许重试 maybe-committed；
4. 调用 FoundationDB `onError()` 或 reset 事务；
5. 使用带随机抖动的退避；
6. 重新执行完整 operation。

FoundationDB 事务提交时可能遇到 `commit_unknown_result`：客户端不知道事务是否已经提交成功。

如果客户端直接重试，一个 remove、create 或其他非天然幂等操作可能被执行两次。3FS 使用幂等记录解决这个问题。

需要幂等保护的操作按以下顺序执行：

```text
读取 IDEM 记录
      │
      ├── 已存在：返回第一次执行结果
      │
      └── 不存在
             │
             ▼
       执行业务修改
             │
             ▼
       写入 IDEM 结果
             │
             ▼
       同一个 FDB commit
```

业务修改和幂等结果在同一个事务中提交，因此不会出现以下中间状态：

```text
业务修改已经提交，但幂等记录尚未保存
```

如果第一次事务实际已经提交，只是客户端收到了未知结果，那么第二次执行会读到幂等记录，并直接返回第一次的结果。

## 八、Versionstamp 的用途

FoundationDB Versionstamp 是与事务提交绑定的 10 字节版本值：

- 前 8 字节来自数据库提交版本；
- 后 2 字节用于区分同一事务中的顺序。

3FS 使用 Versionstamp 保存全局元数据版本，以及 Meta Server 的状态版本。它的优势是：

- 版本和事务原子绑定；
- 全局单调递增；
- 不需要读取全局计数器再加一；
- 不会让所有事务竞争同一个计数器键。

Meta Server 可以据此判断服务注册信息或元数据映射是否发生变化。

如果改用普通全局计数器：

```text
get(version)
set(version + 1)
```

所有更新都会在同一个键上产生冲突，形成明显热点。Versionstamp 更符合 FoundationDB 的设计方式。

## 九、Inode ID 如何避免热点

inode ID 分配同样存在全局热点问题。如果每创建一个文件都更新一次 FoundationDB 计数器，高并发 create 会全部竞争同一个键。

3FS 使用分段分配：

```text
FoundationDB 分配高位号段
             │
             ▼
Meta Server 在本地生成低 12 位
             │
             └── 一个 FDB 分配结果生成 4096 个 inode ID
```

生成 4096 个 inode ID 才需要再次访问 FoundationDB，大幅降低全局分配键的更新频率。

这个设计体现了另一个重要原则：

> FoundationDB 适合协调粗粒度的全局号段，不适合让每个前台请求都更新同一个全局计数器。

## 十、为什么这种设计适合 FoundationDB

### 1. 目录结构天然适合有序 KV

`DENT + parent_inode + filename` 同时支持精确查找、范围扫描、分页、判空和细粒度冲突控制，不需要维护额外的目录索引。

### 2. 用事务替代分布式锁

create、rename、unlink 等操作主要依赖读写冲突和事务重试，而不是显式分布式锁。服务节点宕机时也不需要处理锁租约和锁所有者恢复。

### 3. Snapshot Read 控制冲突放大

路径解析可能读取大量祖先对象。3FS 先用 Snapshot Read 获得数据，再只为真正参与写入判断的键添加冲突，从而降低无关事务互相 abort 的概率。

### 4. 幂等结果与业务修改原子提交

这使 Meta Service 能正确处理 RPC 重试、网络超时和 FoundationDB 的未知提交结果。

### 5. 主动限制事务边界

前台操作保持短小，而递归删除和 GC 被拆成后台批次。3FS 并没有因为 FoundationDB 支持事务，就把任意规模的工作都塞进一个事务。

## 十一、总结

3FS 使用 FoundationDB 的核心方式可以概括为：

> 将文件系统命名空间编码成有序 KV，把一次 POSIX 元数据变更封装为一个短小的 Serializable 事务；通过 Snapshot Read、显式最小冲突范围、事务重试、幂等记录和 Versionstamp，在不引入分布式锁的情况下实现强一致元数据服务。

它并不是简单地把 inode 保存到 FoundationDB，而是围绕 FoundationDB 的事务模型重新设计了：

- inode 和 dentry 的键布局；
- 路径解析方式；
- 并发控制范围；
- commit unknown 的处理；
- inode ID 分配；
- Meta Server 状态版本；
- 大规模删除和 GC 的事务边界。

从这个角度看，3FS 的 Meta Service 是一个很典型的 FoundationDB 上层系统：FoundationDB 提供强一致、有序 KV 和乐观事务，3FS 则负责把文件系统语义准确地翻译成键、范围和冲突集合。
