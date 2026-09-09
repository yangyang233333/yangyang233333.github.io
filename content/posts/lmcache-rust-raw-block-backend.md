---
title: "LMCache RawBlockBackend 源码解析：读写链路与优化边界"
date: 2026-09-09T10:10:00+08:00
draft: false
tags: ["LMCache", "Rust", "io_uring", "NVMe", "KV Cache"]
categories: ["大模型推理"]
summary: "沿 Python Core、PyO3 与 Rust worker 追踪 KV Cache 的落盘和回读，分析固定槽位、条件零拷贝、批量 I/O、NVMe passthrough 与 FDP 的收益及限制。"
---

LMCache 的 Rust Raw Block 后端，核心不是“把 Python 换成 Rust”，而是：**用固定槽位管理 KV Cache，以调用方内存作为 I/O buffer，再通过直接 I/O 和批量提交减少文件管理、数据复制与提交开销。** 不过，名字带 `batched` 不代表跨对象写入已经充分并行，启用 `io_uring` 也不代表每次请求都走零拷贝。

本文分析截至 **2026 年 9 月 9 日**检查的公开源码快照 `08fac932010ca1c3a65ed9a2507fff71b0279282`，不声称它是最新版本；研究工作区中尚未提交的修改不纳入正文。本文是源码分析，没有执行裸盘压测，也不提供未经测量的性能倍数。文末给出固定版本的源码入口。

## 1. Rust 管 I/O，Python 管缓存语义

通常所说的 RawBlockBackend，在非多进程路径里的类名是 `RustRawBlockBackend`。多进程模式使用 `RawBlockL2Adapter`；两者共用 `RawBlockCore` 和 Rust 扩展 `lmcache_rust_raw_block_io`。

```text
非 MP：RustRawBlockBackend       MP：RawBlockL2Adapter
          |                            |
          +----------+-----------------+
                     v
                RawBlockCore
       key 索引 / slot / checkpoint / 锁
                     |
                     v
          PyO3 → Rust RawBlockDevice
      buffer / 对齐 / I/O 提交 / 完成回收
                     |
          +----------+------------------+
          v          v                  v
        POSIX     io_uring        io_uring_cmd
     pread/pwrite Read/Write       NVMe passthrough
```

`RawBlockCore` 维护内存索引、空闲槽位、inflight 状态与恢复信息。Rust 持有设备描述符，处理 buffer 指针、bounce buffer、系统调用和 io_uring 队列。**它不是一个用 Rust 实现的完整 KV 数据库，也没有使用 Tokio runtime。** 所分析的 crate 直接依赖 PyO3、libc 和 io-uring，使用专用线程驱动 ring。

MP adapter 另有 store、lookup、load 三类线程池，通过任务结果与 eventfd 对接上层控制器。这里要分清三层并发：控制器提交任务、Python worker 执行同步 Core 方法、Rust worker 异步驱动设备。这几层的任务数量并不等于设备队列深度。

## 2. 设备布局：用 slot 换掉每个对象一个文件

设备前部保留 metadata 区，内部有两份轮换 checkpoint；后面是固定大小的数据槽位。

```text
设备起始
  [ checkpoint A ][ checkpoint B ][ 对齐余量（如有） ]
  [ slot 0: header | payload | 未使用空间 ]
  [ slot 1: header | payload | 未使用空间 ]
  [ slot 2: header | payload | 未使用空间 ]
```

槽位和 payload 的位置由算术计算，而不是路径查找：

```text
slot_offset    = data_base_offset + slot_id × slot_bytes
payload_offset = slot_offset + header_bytes
payload_capacity = slot_bytes - header_bytes
```

slot header 的有效字段包含 8 字节 magic、8 字节 identity 和 8 字节 payload 长度；预留的 `header_bytes` 还要满足块对齐。完整 key、shape、dtype 等信息保存在内存索引和 checkpoint，而非全部塞进这 24 字节。

这种布局免去逐对象文件创建和路径管理。**只有目标是真正的裸块设备时，才避开对应文件系统的数据路径；用普通文件模拟 raw block，文件系统仍然存在。** `O_DIRECT` 解决的是 page cache 路径，并不把普通文件变成裸盘。

代价是内部碎片与容量约束：`slot_bytes` 包含 header，不是纯 payload 容量。假设 payload 正好是 1 MiB，header 为 4 KiB，那么 slot 至少需要 1 MiB + 4 KiB。分配器优先复用空闲槽位，再递增 `_next_slot`；没有槽位时该 key 写入失败，Core 不会自行执行 LRU，回收由上层淘汰控制器或显式删除驱动。

## 3. 写链路：先占槽位，完成后才进入索引

非 MP 路径从 `batched_submit_put_task()` 开始：过滤已有或正在提交的 key，增加 `MemoryObj` 引用计数，然后把工作交给后台协程。协程用 `asyncio.to_thread()` 调用同步的 `RawBlockCore.put_many()`，在 `finally` 中释放引用并清理任务记录。

MP 的入口是 `submit_store_task()`，由 store pool 调用 `_run_store_task()`，最终同样进入 `put_many()`；启用 FDP 时，还会在 adapter 层给对象分配 placement ID。

### 3.1 单对象写入的状态转换

`put_many()` 在这个快照里逐对象执行以下过程：

1. 持 Core 锁检查索引和 inflight 表，分配一个 slot，并登记 `_inflight[key]`。
2. 释放锁，进入 `_write_one()`，生成 slot header，准备 payload buffer 及对齐后的实际 I/O 长度。
3. `_write_buffers()` 提交 header 和 payload，等待相应 I/O 完成。
4. 重新加锁。成功且未取消，才把 key 放进 `_index` 并更新 dirty 计数；失败或被取消，则回收槽位。

**索引可见性的边界是 I/O 完成，而不是提交完成。** 大块 I/O 不在 Core 索引锁内执行，因此不同调用可以在设备等待阶段重叠。上层对内存对象的引用保留，则避免异步写尚未结束时内存被回收复用。

### 3.2 “批量写”到底批在哪里？

这段结构最容易被函数名误导。用简化伪代码表达当前控制流：

```python
for key, obj in zip(keys, objs):
    offset = allocate_slot(key)
    success = write_one_and_wait(key, obj, offset)
    publish_or_reclaim(key, offset, success)
```

也就是说，**同一次 `put_many()` 并没有先积攒所有对象，再统一提交它们的 payload**。它通常等当前对象的 header 与 payload 完成，才处理下一个对象。

真正已有的批量能力位于更低层：普通 io_uring 在条件满足时，将一个对象的 header 和 payload 交给 `batched_write()`；uring_cmd 可以将大 payload 拆成多个命令一批提交；Rust worker 还可以汇集不同并发调用入队的请求。

因此，非 MP 的 `_submit_put_many()` 可以减少上层调度次数，却不能据此认定拥有“对象数大小”的设备并发。单个 store worker、普通 io_uring、每对象两个 I/O 的场景下，即使 ring 很深，也可能没有足够待提交请求填满它。

## 4. 读链路：调用方分配，设备直接填充

非 MP 的 `_batched_get_prefix()` 先通过 `get_metadata_prefix()` 查找连续命中前缀，并对需要的 key 加锁；然后依据 shape、dtype 和 format，从 `LocalCPUBackend` 分配目标 `MemoryObj`，交给 `load_many_into()`。

MP 则把 lookup-and-lock 与 load 拆成独立任务，load 阶段接收上层已经提供的目标对象。Core 的公共路径一致：

1. 在锁内取得索引项快照，增加读 I/O 活跃计数。
2. 为所有命中项收集 offset、目标 buffer、payload 长度和实际传输长度。
3. 一次调用 `_read_buffers()`，得到逐对象成功结果。
4. 对成功对象恢复 `cached_positions`，最后减少活跃计数。

与写路径不同，**读取确实先收集多个对象，再尝试批量下发。** 普通 io_uring 快路径要求所有条目的传输长度条件和 buffer 对齐条件成立；否则 `_read_buffers()` 回退为逐项 `read_uring()` 并等待，不能把这一分支也称为跨对象批量读。

成功结果在两个入口有不同语义：Core 返回完整布尔数组，MP 将其转换为 bitmap；非 MP 只返回从头开始连续成功的前缀，并释放失败位置及之后已经分配的对象。单次 I/O 失败不必抹掉其他条目的成功信息，但上层前缀语义可能限制最终可利用的范围。

## 5. 零拷贝不是一个开关，而是一组条件

优化的起点是让 Python 的 `MemoryObj.byte_array` / `memoryview` 经 buffer protocol 暴露地址，Rust 直接使用该地址，而不是先把整个 KV payload 转成 Rust `Vec`。

这里的“零拷贝”主要指**避免这段 CPU 内存到存储链路里的额外用户态 payload 复制**。它不意味着 GPU 到 CPU 没有传输，也不意味着设备不做 DMA，更不等于 GPUDirect Storage。

### 5.1 对齐同时约束地址、偏移和长度

直接 I/O 不只检查 offset。buffer 起始地址和实际传输长度也需要满足配置的 alignment，目标内存容量必须覆盖即将写入的范围。

`_build_direct_odirect_view()` 只有在 `use_odirect`、`enable_zero_copy` 等条件成立时，才尝试从 `MemoryObj.data_ptr` 构造直接视图；视图不允许越过已知 buffer 长度。写入 padding 时，如果可见容量足够，可以原地清零尾部；否则要进入补齐路径。

POSIX 的 `pwrite_from_buffer()` / `pread_into()` 有三种重要情况：

| 情况 | 执行方式 | 复制成本 |
| --- | --- | --- |
| 地址对齐，长度与可用容量满足要求 | 直接 pread / pwrite | 不增加用户态 payload 复制 |
| 地址对齐，但尾部需要补齐或读目标装不下补齐部分 | 对齐前缀直读写，尾部用 bounce buffer | 只复制尾部有效数据 |
| 直接 I/O 地址不对齐 | 分配对齐 bounce buffer | 可能复制整个 payload |

例如 payload 是 4100 字节、alignment 是 4096、实际 I/O 是 8192 字节：POSIX 混合路径直接处理前 4096 字节，最后 4 字节通过一个对齐尾块处理。它减少的是大块 memcpy，**不是把实际设备传输量缩回 4100 字节**；而且前缀与尾块会带来分开的 I/O。

### 5.2 不要把 POSIX 的尾部优化推广到所有引擎

Rust 的 `read_uring()` 遇到未对齐地址，或目标容量小于 `total_len` 时，可以为整次传输分配 bounce buffer，然后复制回有效内容。uring_cmd 的 Python 层也会在容量不足时准备完整对齐临时区，再拆分命令；Rust 批量读还会按条目检查地址并决定是否 bounce。

因此，同一块 KV 在不同引擎、不同分配器和不同长度下，可能走完全不同的复制路径。评估零拷贝应观察**实际 fast-path 命中率和 bounce 字节数**，不能只看配置名或 README 的总览图。

### 5.3 fixed buffer 优化的是注册与映射，不是零拷贝的唯一入口

Core 尝试通过 allocator 的 `get_paged_buffers()` 取得长期存在的内存页，调用 Rust 的 `register_fixed_buffers()` 注册到 ring。普通 io_uring 请求命中注册映射时，可以使用 `ReadFixed` / `WriteFixed`；uring_cmd 分支也有传递 buffer index 的代码。

需要注意，这个快照的映射查找使用**请求起始地址精确匹配**，不是“地址落在任意已注册区间内”查找。一个大 allocation 已注册，不代表它内部所有偏移切片都能命中，尤其是拆分后的子块。

CUDA pinned memory 与 io_uring registered buffer 也不是同一项注册操作。前者适合 GPU 传输，不自动意味着后者已完成；反过来，即便没有 fixed buffer，地址满足条件的普通直接 I/O 仍然可以避免额外用户态 memcpy。

## 6. Rust worker 怎样组织 io_uring

`RawBlockDevice` 内部有一个带 Mutex 的生产者队列、专用 worker、inflight 表，以及按 batch ID 隔离的完成和 buffer 保活记录。它不是无锁实现。

```text
Python 调用线程
  准备 buffer → IoSubmission → 生产者队列 → producer eventfd
                                      |
                                      v
                               Rust worker
                        填 SQE → submit → 收 CQE
                                      |
                       拷回 bounce（如需）并记结果
                                      |
                       更新全局 / batch 活跃计数
                                      v
                         wait_iouring(batch_id)
```

worker 从队列取请求，按 SQ 可用空间构建提交项，剩余请求留待后续处理。`user_data` 将 CQE 关联回对应请求；普通 I/O 还有短传输后续处理逻辑，NVMe command completion 则按其状态语义判断，不能当作字节计数处理。

唤醒机制由两个 eventfd 和 epoll 组成：生产者入队触发一个 eventfd，内核产生 CQE 触发另一个。这样 worker 在没有待提交工作时可以阻塞，既能被新请求唤醒，也能被已提交请求的完成唤醒，而非始终忙轮询。

`wait_iouring()` 在释放 GIL 后等待指定 batch，而不是等待整个设备清空，最终返回成功 bitmap 和稀疏错误列表。不过，这个快照里的 batch 等待还使用了 **10 微秒超时的 condition-variable 循环**；所以“worker 是事件驱动”并不等于“所有等待层都完全没有周期唤醒”。

buffer 生命周期同样属于性能设计的一部分。批量接口保留 Python 对象引用，bounce 内存由 Rust 的引用计数对象保持到完成处理；等待结束再清理该批资源。但保活不能替代调用方契约：**I/O 期间仍不能重分配底层内存、提前复用目标，或并发修改正在写出的内容。** Rust 的类型系统不会自动证明这些跨 Python 裸指针约束。

## 7. NVMe passthrough 与 FDP：进一步控制设备路径

### 7.1 io_uring_cmd 并不是 SPDK

`io_engine="io_uring"` 配合 `use_uring_cmd=True` 时，后端使用 NVMe namespace 字符设备，例如 `/dev/ng0n1`，而不是普通块设备节点 `/dev/nvme0n1`。它构造 NVMe 读写命令，通过 `UringCmd80` 交给内核 NVMe 驱动。

**这仍然经过内核，不是用户态驱动，也不是 kernel bypass。** 此模式会忽略针对普通文件描述符路径的 `use_odirect`，但这绝不表示 NVMe 命令的 LBA、传输长度或 buffer 约束消失了。

### 7.2 拆分大请求，先解决合法性，再提供并发机会

Core 的 `_resolve_max_data_transfer_size()` 优先采用显式配置；自动推导时结合设备队列的 `max_hw_sectors_kb` 和 `max_segments × page_size`，再满足块对齐。不能只考虑总字节上限而忽略 scatter-gather 段数。

写路径把 header / payload 拆成有界子块，批量下发并检查全部结果。读路径还记录每个子块属于哪个逻辑对象，只有该对象的全部子块都成功，才将这个对象标记为成功；需要临时目标区时，再复制回调用方。

拆分既能规避超过设备限制的失败，也能让一份大 payload 形成多条待处理命令。但更小的块会增加 SQE/CQE、内存视图和完成跟踪开销，并非越小越快。

### 7.3 FDP 处理的是放置与回收，不是提高链路带宽

这版 adapter 支持依据 cache salt 前缀，以及前缀结合 rank 的策略分配 FDP placement ID。Core 将 ID 传到 NVMe 写命令，也可以为 metadata checkpoint 指定独立 ID；同一 slot 的 header 与 payload 使用相同的数据 placement ID。

可选 slot affinity 会优先复用同一 placement ID 的空闲槽位，避免软件槽位复用完全打乱放置意图。这些槽位 affinity 记录是运行时状态，并非跨重启的完整持久化放置策略。

其设计目标是让不同生命周期的数据更容易分开回收，从而减少 SSD 内部搬迁和写放大。**这是需要稳态实测验证的目标，不是源码证明的收益。** cache salt / rank 只是策略代理，不天然等于真实寿命；控制器支持情况、数据更新模式和设备填充率都会影响结果。

`None` 表示不显式设置 directive；Core 拒绝显式 placement ID 0，因为默认写已经使用对应映射。ID 池耗尽等情况下，adapter 可以回退到无 directive 写入，不能把回退解释成“仍有独立放置隔离”。

## 8. checkpoint：减少热路径负担，不提供数据库式事务

每次对象变化只更新 dirty 计数。周期 checkpoint 在有脏元数据、没有活跃 I/O 且满足静默窗口时，序列化状态并轮换写入两份 metadata container；关闭流程也有强制 checkpoint 路径。

每份 checkpoint 有序号、长度和 CRC。恢复时选取可读取且校验通过的最高序号副本；可选的 slot header 验证进一步比较 identity 与 payload 长度，删除不匹配条目。

这把元数据序列化和写入从每次 put 的同步热路径中移开，代价是恢复点可能落后于最近成功的对象写入。**CRC 针对 metadata payload，不是 KV payload 的端到端校验；检查 header 也不等于检查完整内容。**

此外，所分析的写链路没有显式 `fsync` / `fdatasync` 或 NVMe FUA/flush 协议。checkpoint 将 payload 与 header 按该顺序传入 `_write_buffers()`，但不能把列表顺序当成 io_uring 的严格持久化顺序，也不能把 I/O completion 当成掉电后的数据保证。

所以更准确的定位是：**为可重建缓存提供 checkpoint 恢复机制，而不是提供 crash-atomic 的对象事务。** 双副本、CRC 和 slot 验证降低部分恢复风险，并不能据此推导出任意时刻断电都无损。

## 9. 怎样验证这些优化是否真正起作用

源码能证明分支存在，不能证明某台机器上谁更快。建议围绕下面几组变量做受控对比，而不是只跑一组最大吞吐：

| 对比维度 | 需要回答的问题 |
| --- | --- |
| 文件系统后端与真正 raw device，统一 direct-I/O 条件 | 收益来自文件管理，还是 page cache 条件不一致？ |
| POSIX、普通 io_uring、uring_cmd | 提交与命令路径改变后，CPU 开销和尾延迟是否改善？ |
| 对齐 / 非对齐 buffer，整块 / 尾块 payload | 快路径命中率、bounce 字节数和有效带宽是多少？ |
| store/load worker 数、逻辑 batch、ring depth、子块大小 | 上游有没有提供足够并发？更多线程是否只是增加争用？ |
| FDP 开关与分组策略 | 设备预填充并持续更新后，写放大和稳态尾延迟是否改善？ |

除了有效 payload 带宽和 IOPS，还应记录 p50/p95/p99 延迟、CPU 时间、用户态队列等待、实际设备请求尺寸、失败率及恢复校验结果。后端请求数、拆分命令数与设备最终执行的命令数不是同一个指标；ring depth 也不直接等于硬件 outstanding 深度。

仓库已有 `benchmarks/storage_backend_io/` 作为非 MP 后端微基准入口，但 MP 的控制器、线程池与 L1 分配器需要单独验证。**裸设备写入会覆盖原有内容，只能使用专用、未挂载、可销毁的数据设备；本文没有执行这些操作。**

从这个快照看，值得继续验证的方向是跨对象写入聚合、更多布局下的批量读、注册 buffer 的区间匹配，以及等待与 bounce 开销的观测。这些是后续优化方向，不是已经实现的能力。

## 小结与源码入口

这套后端的收益来自一条完整链路：**固定 slot 减少对象存储管理成本，对齐 buffer 减少复制，io_uring 摊薄提交开销，uring_cmd 与 FDP 提供更细的 NVMe 控制。** 实际收益则取决于上游并发、分配器布局、对象大小和设备行为，不能归结成“Rust 更快”。

最值得亲自核对的是 `put_many()` 与 `load_many_into()` 的不对称，再沿 `_write_buffers()` / `_read_buffers()` 进入 Rust。所有链接固定到本文快照，避免后续代码演进改变阅读结论：

- [RawBlockCore：布局、读写、对齐与 checkpoint](https://github.com/LMCache/LMCache/blob/08fac932010ca1c3a65ed9a2507fff71b0279282/lmcache/v1/storage_backend/raw_block/core.py)
- [Rust RawBlockDevice：PyO3、POSIX、io_uring worker 与 NVMe 命令](https://github.com/LMCache/LMCache/blob/08fac932010ca1c3a65ed9a2507fff71b0279282/rust/raw_block/src/lib.rs)
- [非 MP 的 RustRawBlockBackend](https://github.com/LMCache/LMCache/blob/08fac932010ca1c3a65ed9a2507fff71b0279282/lmcache/v1/storage_backend/plugins/rust_raw_block_backend.py)
- [MP 的 RawBlockL2Adapter 与 FDP 策略](https://github.com/LMCache/LMCache/blob/08fac932010ca1c3a65ed9a2507fff71b0279282/lmcache/v1/distributed/l2_adapters/raw_block_l2_adapter.py)
- [Storage Backend I/O 微基准](https://github.com/LMCache/LMCache/tree/08fac932010ca1c3a65ed9a2507fff71b0279282/benchmarks/storage_backend_io)
