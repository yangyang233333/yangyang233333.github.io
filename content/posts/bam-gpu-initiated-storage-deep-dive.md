---
title: "BaM 深入解析：GPU 如何自主驱动 NVMe，并把 SSD 变成按需访问的后备内存"
date: 2026-09-12T21:17:00+08:00
draft: false
tags: ["BaM", "GPU I/O", "NVMe", "CUDA", "源码阅读", "存储系统"]
categories: ["技术"]
description: "沿源码拆解 BaM 的数组抽象、软件页缓存、并发 NVMe 队列、PCIe 可见性协议与性能边界，并推导细粒度访问背后的元数据和并发成本。"
---

BaM 最重要的改变，不是让 SSD 的数据直接进入 GPU 显存，而是让 **GPU kernel 中发现数据需求的线程，自己完成缓存查询、NVMe 命令提交和完成等待**。CPU 负责建立访问条件，却不再逐条参与这条请求路径。

它用三层机制实现这个目标：接近数组的编程接口、运行在显存里的软件页缓存，以及能够被大量 GPU 线程并发访问的 NVMe 队列。真正困难的部分，是如何让“很多线程同时缺页”既不会重复读盘、错误淘汰，也不会在一个全局锁前排成长队。[S1][S2][S3]

本文基于 **2026 年 9 月 12 日获取的仓库默认分支快照 `315fadfc5c5c018a64596157bfac94ecbb7d87a2`**，对应最新提交日期为 2025 年 11 月 23 日；论文采用 ASPLOS 2023 工作的 arXiv v3。源码链接固定到该提交，避免后续代码变化使讨论失效。

以下区分三种证据：源码可以直接确认的实现、论文报告的实验结果，以及根据实现作出的推导。**没有在满足 BaM 条件的 GPU/NVMe 实机上运行性能实验**；文中的吞吐、加速比不是本次实测。硬件前提和复现限制见最后两节。[S1][P]

## 一、问题不是“怎么搬数据”，而是“谁知道下一次该搬什么”

考虑图遍历。GPU 处理当前 frontier 中的顶点后，才知道下一步需要读取哪些邻接表。如果 CPU 负责 I/O，请求路径在逻辑上会变成：

```text
GPU 发现下一批数据需求
          │
          ▼
CPU 获得需求、组织读取、安排传输
          │
          ▼
GPU 等待所需数据，再继续计算
```

当数据访问模式可预知时，这种分工可以用大块预取和流水线摊薄成本；但对细粒度、数据依赖的访问，下一次读取的位置本身就是上一步 GPU 计算的结果。扩大预取范围会多读，缩小预取范围又会放大请求协调成本。这正是 BaM 论文讨论的场景，而不是“所有应用都应该把 CPU 从 I/O 中移除”。[P]

这里必须区分两个维度：

| 维度 | 要回答的问题 | BaM 的选择 |
|---|---|---|
| 数据面 | 数据是否必须经由 CPU 内存中转？ | NVMe SSD 与 GPU 显存间进行 PCIe P2P DMA |
| 请求控制路径 | 谁决定读什么、构造请求、通知设备并等待完成？ | GPU 线程执行常规数据 I/O 路径 |

因此，**direct DMA 不等于 GPU-initiated I/O**。本文对 CPU 发起型路径的讨论，指论文所对比的执行模型；不把 2023 年的比较扩大成对所有后续 GDS 产品或接口的断言。[P][S4]

## 二、架构地图：CPU 建立资源，GPU 消费资源

从源码组织看，BaM 的分层如下：

```text
                    CPU 初始化控制面
       Controller / QueuePair / page_cache_t / range_t
           │ 创建队列、注册显存、映射 BAR、准备 PRP
           ▼
┌──────────────────────── GPU ─────────────────────────┐
│ 应用 kernel：array_d_t / bam_ptr / bafs_ptr           │
│                       │                             │
│ 软件页缓存：合并、引用计数、装入、淘汰、写回          │
│                       │ cache miss                  │
│ NVMe I/O：构造命令 → SQ → doorbell → CQ               │
│                       │                             │
│ HBM：数据页、SQ/CQ、软件元数据                       │
└───────────────────────┼─────────────────────────────┘
                        │ PCIe P2P
                        ▼
                 NVMe SSD controller
```

阅读时可以按这张源码地图推进：

| 层次 | 主要入口 | 负责什么 |
|---|---|---|
| 应用 | `benchmarks/bfs/main.cu` | 在真正的数据依赖循环里访问后备数组 |
| 数组与指针 | `array_d_t`、`bam_ptr`、`bafs_ptr` | 元素索引转换、页内访问、引用复用 |
| 缓存 | `range_d_t::acquire_page`、`page_cache_d_t::find_slot` | 缺页合并、固定页面、淘汰、写回 |
| 设备 I/O | `read_data`、`write_data` | 构造 NVMe 请求并管理完成 |
| 并发队列 | `sq_enqueue`、`cq_poll`、`cq_dequeue` | 多线程提交、轮询和批量推进队列 |
| 主机运行时 | `Controller`、`QueuePair`、`buffer.h` | 创建和映射 GPU 将使用的资源 |
| 内核辅助模块 | `module/pci.c`、`module/map.c` | BAR 映射、GPU 页固定与 DMA 地址建立 |

这些不是彼此独立的优化：数组层决定访问粒度，缓存层决定实际产生多少 I/O，队列层决定这些 I/O 能多快进入设备。[S2][S3][S4][S5][S6][S7][S8]

### 2.1 为什么仍然需要 CPU 和内核模块

`Controller` 在主机侧打开控制器设备，建立管理队列，识别控制器和 namespace，协商队列数量，再创建 `QueuePair`。控制器寄存器区域通过 `cudaHostRegisterIoMemory` 注册；队列初始化进一步取得 doorbell 的 GPU 可访问指针。[S4][S5]

内核模块的 `mmap_registers` 使用非缓存属性映射设备 BAR；`map_gpu_memory` 则通过 `nvidia_p2p_get_pages` 固定 GPU 页，再调用 `nvidia_p2p_dma_map_pages` 建立 NVMe 设备使用的 DMA 映射。[S6][S7]

因此，“CPU 不在逐请求路径上”不代表“不需要 CPU、操作系统或权限管理”。这些初始化动作是后续 GPU 能安全使用地址和设备资源的前提；至于多租户隔离、故障恢复是否充分，则是另一个问题，不能由“能映射”直接推导出来。

### 2.2 三种地址不能混在一起

沿初始化流程至少会遇到三类地址：

- GPU 线程用于 load/store 的设备虚拟地址。
- NVMe 控制器进行 DMA 时使用的 I/O 地址。
- GPU 用于通知设备的 MMIO doorbell 地址。

`nvm_dma_t` 同时保存 `vaddr` 和 `ioaddrs[]`，正是在区分前两者。缓存初始化把数据槽对应的 DMA 地址整理为 PRP 信息；读请求使用这些地址告诉 SSD 应把数据放进哪个 HBM 槽位。**把 `cudaMalloc` 返回的指针直接塞进 NVMe 命令，并不等价于完成了这条映射链。**[S8][S9]

还要区分四种“页”：应用的缓存页、NVMe namespace 的逻辑块、控制器 PRP 使用的内存页、GPU peer-memory 固定与对齐粒度。它们可能不同。源码通过 `n_blocks_per_page = cache_page_size / lba_data_size` 把一个缓存页转换成若干逻辑块；并另外准备 PRP，不能把所有 `page_size` 都理解为 4 KiB。[S8][S9]

## 三、一次元素读取，如何变成一条 NVMe 命令

仓库 BFS 的一个 frontier kernel 先从 `vertexList` 取得顶点的邻接区间，再在循环中调用 `da->seq_read(i)` 读取边。这个落点很重要：BaM 接管的是算法已经确定索引后的数据访问，而不是在 CPU 上重演一次图遍历来预测请求。[S10]

标准数组读路径可以概括为：

```text
array_d_t::operator[] / seq_read
  → find_range
  → get_page / get_subindex
  → coalesce_page
  → range_d_t::acquire_page
      命中：返回 cache slot
      未命中：find_slot → read_data
                            → get_cid
                            → sq_enqueue
                            → cq_poll / cq_dequeue
                            → enqueue_second
                            → put_cid
  → 从 HBM cache slot 读取目标元素
  → release_page
```

这是同步的设备侧访问接口：调用线程需要得到值后才能继续。它的高吞吐不依赖把一次读取变成零延迟，而依赖**其他独立线程能够同时提交请求或执行计算**。[S2][S3]

### 3.1 数组抽象不是硬件虚拟内存

`range_d_t` 保存逻辑数组区间、后备页起点、页面状态数组和分布方式。读取时，大致进行以下地址计算；公式为源码逻辑的改写：

```text
byte_offset = (element_index - range_start) × sizeof(T) + page_start_offset
logical_page = byte_offset / cache_page_size
offset_in_page = byte_offset % cache_page_size
```

对于二次幂页大小，实现使用移位和掩码。找到缓存槽之后，目标地址才成为 `cache_base + slot × cache_page_size + offset_in_page`。[S2]

也就是说，SSD 并没有被 GPU MMU 透明地变成普通显存；访问必须经过 BaM 的软件接口。论文常用的 `bam::array<T>` 是概念性接口名称，当前仓库的主要实现名则是 `array_t<T>`、`array_d_t<T>` 以及若干指针包装，照搬论文片段不等于得到可编译的仓库示例。[S1][S2][S11][P]

### 3.2 先在 warp 内合并，再进入全局缓存

`coalesce_page` 对活跃线程按逻辑页地址分组，还把数组对象身份纳入匹配。每组选择一个 leader，由它调用 `acquire_page`，然后用 shuffle 把缓存槽广播给组内线程。引用数量使用组内线程数，而不是简单地固定为 1。[S2]

例如，假设同一 warp 的 32 个线程读取同一个冷缓存页上的不同元素：

- 页级获取由一个 leader 执行，而不是 32 次分别争夺状态字。
- 引用计数一次增加 32，使页面在这组线程使用期间不能被淘汰。
- 数据装入之后，线程各自读取页内位置，再由 leader 成组释放引用。

如果 32 个线程命中 32 个不同页面，则不存在这种合并收益。**warp 合并消除的是同页元数据操作和请求重复，不会把任意随机索引自动变成顺序 I/O。**[S2]

## 四、软件缓存：它首先是并发请求协调器

BaM 缓存不只是“让下一次访问更快”。即使没有长期重用，它仍要处理同一时刻大量线程读同一冷页的问题：如果每个线程都独立提交 I/O，SSD 会重复读取相同数据，显存也可能被装入多份副本。[S2]

### 4.1 两张表，各自代表什么

源码把逻辑页和物理缓存槽分开管理：

| 对象 | 索引方式 | 核心信息 |
|---|---|---|
| `data_page_t` | 后备数组的逻辑页号 | 状态字、映射到的缓存槽 `offset` |
| `cache_page_t` | HBM 缓存槽号 | 槽位锁、当前承载的逻辑页全局标识 |

这让命中查询能够从逻辑页直接找到缓存槽，同时让淘汰器从槽位反查旧页面。[S2]

不要把它理解成一个只为驻留页分配元数据的稀疏哈希缓存：当前 `range_t` 初始化会为 range 中的所有逻辑页分配 `data_page_t`。这一点决定了后面讨论的显存容量边界。[S12]

### 4.2 一个状态字同时编码有效性、忙碌、脏和引用数

当前实现的 32 位 `state` 布局如下：

```text
bit 31       bit 30      bit 29       bits 28..0
VALID        BUSY        DIRTY        reference count
```

这里的 `BUSY` 不是“有线程正在普通读取”，而是页面正在经历需要独占协调的变化，例如装入或淘汰；普通使用者由引用计数表示。[S2]

忽略计数和脏位后的主要状态转换是：

```text
INVALID ──取得装入权──► INVALID|BUSY
                             │
                     找槽、提交读、完成可见性处理
                             │
                             ▼
                           VALID
                             │
                   引用数为 0，取得淘汰权
                             ▼
                         VALID|BUSY
                             │
                        必要时写回
                             ▼
                          INVALID
```

`acquire_page` 一开始就原子增加引用数，再根据状态行动。冷页上的竞争者通过 `fetch_or(BUSY)` 争夺装入权；只有成功取得权利、且确认页面仍无效的线程才找槽并发起读取，其他线程等待状态改变。[S2]

装入者在 `read_data` 返回后写入 `offset`，再用 release 操作切换到有效状态；其他线程通过 acquire 观察状态后读取槽位映射。这里用状态字发布此前建立的映射关系，避免把普通 `offset` 写入误当成独立完成的页面发布。[S2]

这套 acquire/release 处理的是 GPU 线程之间的元数据发布。**它不自动解决外部 PCIe DMA 写入的可见性**；后者还需要第七节的额外协议。

### 4.3 为什么拿到 BUSY 后还要重新检查

一个容易忽略的竞争窗口是：线程先读到 INVALID，但在它执行 `fetch_or(BUSY)` 之前，别的线程已完成装入并把页面改成 VALID。

当前代码检查原子操作返回的最新状态；如果发现页面其实已有效，就清除这次取得的 BUSY 并重试，而不是再次装入。同页重复装入的历史修复 `11cbf91` 正是围绕这个问题。**“第一次检查无效”不代表“取得锁后仍然无效”。**[S2][H1]

### 4.4 淘汰并非严格 LRU

`find_slot` 用全局递增 `page_ticket` 对缓存槽数取模，分散不同线程尝试的槽位。空闲槽可以直接领取；已占用槽则需要取得槽位锁，查到旧逻辑页，并确认旧页不忙且引用数为零。[S13]

论文把它归类为 clock replacement。就这个快照的代码而言，更精确的理解是：**循环探测候选槽，跳过忙碌或被引用的页**。这条实现里没有严格 LRU 的最近访问链，也没有经典 second-chance 算法中单独的近期访问位；不要凭名称补出不存在的策略。[S13][P]

淘汰器先看到引用数为零之后，还必须在设置旧页 BUSY 的原子操作返回值里再次检查引用数。否则，一个读者可能恰好在两步之间获得引用。检查失败时，淘汰器撤销 BUSY，继续找其他槽。[S13]

一个更细的情况是，读者在淘汰器取得独占权之后才增加引用。此时它会看到 BUSY 并等待；旧页失效操作保留计数位，等待者后续可以重新竞争装入，而不是让这个引用悄悄丢失。[S2][S13]

可以用两条不变量概括它要保护的关系：

1. 已允许访问 HBM 数据的读者持有引用时，槽位不能被改装成另一页。
2. 同一个逻辑页在一次冷页装入竞争中，只允许一个线程成为装入者。

前者靠引用计数、BUSY 和复查协作；后者靠逻辑页状态上的原子竞争，而不只是槽位锁。槽位锁只能阻止两个线程抢同一个槽，无法单独阻止一个逻辑页被装入两个不同槽。

## 五、NVMe 队列：预约可以乱序，发布必须连续

缓存 miss 最终到达 `read_data`：分配 command identifier，构造带 namespace、LBA、块数和 PRP 的 NVMe READ，加入 SQ，等待对应 CQE，最后回收标识。[S2][S3]

难点不是 64 字节命令如何填写，而是成千上万 GPU 线程如何共享有限长度的硬件队列。

### 5.1 为什么不能锁住整个提交过程

如果把“取得槽位、写命令、敲 doorbell、等待完成”全部放在一个锁内，那么等待 SSD 的线程会阻塞其他提交者。即使不把完成等待放进去，仅仅串行化所有命令写入也会限制 GPU 并发。[P]

BaM 拆分了三件事：

- **预约**：原子 ticket 分配逻辑队列位置。
- **准备**：不同线程在不同物理槽位并行填命令。
- **发布**：一个短临界区将连续准备好的前缀交给硬件。

它不是严格意义上完全无锁的队列，而是把锁缩小到共享 head/tail 的推进过程；SQ/CQ 和一致性辅助队列都能看到显式锁。[S3][S14]

### 5.2 ticket 与 turn counter 解决槽位复用

设物理队列长度为 `Q`，且满足实现使用掩码索引所需的二次幂前提。当前源码的核心关系为：

```text
ticket = atomic_increment(submission_ticket)
slot = ticket mod Q
expected_turn = 2 × floor(ticket / Q)
```

线程等待 `tickets[slot]` 等于自己的 `expected_turn`，再写入命令。提交阶段结束和硬件消费后的回收阶段分别递增该槽的计数，合起来推进到下一轮使用者需要的偶数。[S3]

比如 `Q=8`，ticket 1 和 ticket 9 都映射到槽 1，但期望的 turn 分别是 0 和 2。后者不能因为看见槽地址相同就覆盖前者；它必须等待上一轮提交与回收都完成。

这个机制也解释了两个不能混淆的数字：**软件可以预约很多请求，不代表硬件队列当下可以容纳这么多命令**；而槽位可复用与 command ID 可复用，又是两件分别管理的事。[S3]

### 5.3 为什么必须是“连续准备好的前缀”

各线程写完命令后设置 `tail_mark[slot]`。竞争到 `tail_lock` 的线程调用 `move_tail`，从当前 tail 开始检查连续标记，并在有进展时写一次 doorbell。[S3]

```text
当前 tail → 槽 A：ready
            槽 B：not ready
            槽 C：ready
            槽 D：ready
```

此时只能发布 A。即使 C、D 已经写好，也不能跳过 B 直接推进 tail，否则 SSD 可能把 B 中尚未完整准备的内容当成命令。等 B 就绪之后，另一次推进可以合并发布 B、C、D。

这叫**机会式批量发布**更准确：批量大小由当时连续就绪的前缀决定，不要求先攒够固定数量，也没有必要让 CPU 组织一个 batch。代价是可能出现队头阻塞：一个靠前槽位的生产者停顿，会延迟后面已准备好的命令。

当前 doorbell 写入使用 `st.mmio.relaxed.sys.global.u32`，直接体现了 GPU 对设备寄存器的操作。不过，“指令带有 system scope”不能被泛化成所有普通内存、DMA 和 MMIO 之间都自动获得完整排序；整条协议仍依赖目标平台的内存与 PCIe 行为。[S3][P]

## 六、完成队列：找到自己的完成，不等于能立即回收所有空间

SSD 执行命令后把 CQE 写回显存。`cq_poll` 按 command ID 寻找自己的完成项，并检查 phase bit，以区分环形队列不同轮次的有效条目。[S3]

完成项至少同时承担两种作用：

- 告诉某个请求者：对应命令已经有了完成记录。
- 带回 SQ head 信息：设备已消费了多少 submission entries。

两者不是一回事。某个命令已经被 SSD 从 SQ 取走，并不意味着这个命令的全部数据操作已经完成；SQ 存的是命令描述符，不必一直占用到应用消费结果。[S3]

找到 CQE 后，`cq_dequeue` 标记条目已经被软件处理。取得 CQ head 锁的线程合并推进连续已处理前缀，更新 CQ doorbell；`move_head_cq` 还依据最后一个相关 CQE 中的 SQ head 更新 SQ 槽位 turn counter。这样，设备看到可重用的 CQ 空间，后续生产者也看到可重用的 SQ 空间。[S3]

这里有两个性能边界：

1. **轮询没有消失。** 线程仍会消耗指令与显存访问来查找完成，源码也使用 nanosleep 退避。
2. **更深的队列并不保证更快。** `cq_poll` 会扫描候选条目；深度、并发者数量和完成分布都会影响扫描成本。

所以调优目标不是单纯增大 queue depth，而是在设备所需在途请求数、SM 执行资源和完成查找开销之间找到平衡。[S3]

## 七、最容易漏掉的环节：CQE 可见，数据就一定可见吗

如果只把调用链写到 `cq_poll` 就结束，会漏掉 BaM 一个关键正确性机制。

论文 §4.4 指出，在其原型使用的 GPUDirect RDMA/PCIe 环境下，外部设备写入 GPU 内存时，GPU 上并发运行的线程可能不能仅凭观察到完成记录，就确认此前数据写入已满足所需的可见顺序。CPU/GPU 之间的常规同步经验，不能直接套到持续运行的 GPU kernel 与外部设备之间。[P]

### 7.1 第二次请求为什么能够帮助建立顺序

论文使用的方案是：线程观察到第一次命令的完成之后，再提交第二次 I/O 请求并等待它完成。

```text
SSD 写入第一次请求的数据和 CQE
                │
GPU 观察到第一次 CQE
                │
GPU 提交第二次命令
                │
SSD 必须经 PCIe 读取新的 SQE
                │
SSD 执行并写回第二次 CQE
                │
GPU 观察到第二次完成，再发布缓存页
```

关键不是“又从介质读了一遍就把 CUDA cache 刷新了”，而是**第二次命令迫使控制器在此前写入之后，执行一次读取 GPU 侧命令的 PCIe read**，论文利用这一往返建立其原型所需的顺序。[P]

源码对应 `read_data → enqueue_second`。后者把额外读取缩小为一个逻辑块，而不是重新读取整个缓存页；它沿用已准备的请求信息，并等待这次附加命令完成。[S14]

### 7.2 不为每个 miss 都单独支付第二次读取

`page_cache_d_t` 维护 `q_head`、`q_tail`、`q_lock` 和 `extra_reads`。第一次命令完成的线程加入一个共享的软件进度序列；取得锁的线程提交附加请求，并在完成后推进已覆盖的进度。其他线程如果自己的位置已经被覆盖，就不再单独发起第二次请求。[S14]

设有 `R` 次正常读取，另外提交了 `E` 次这种辅助读取，那么请求数放大是：

```text
request_amplification = (R + E) / R = 1 + E/R
```

若每次辅助读取都只覆盖自己，则接近两倍请求数；能够覆盖更多并发完成时，比例就会下降。论文报告该合并策略在其评估中把相关性能开销控制在 8% 以下，**这不是本快照在任意 GPU、SSD 或 PCIe 拓扑上的保证**。[P]

这也是移植最需要重新核验的部分：共享进度是否确实覆盖当前拓扑中的相关写入、不同控制器的访问顺序如何成立，都不能仅凭“原子变量用对了”证明。本文确认源码实现了论文这条路径，不把它当作可复制到所有异构平台上的通用 fence。[S14][P]

## 八、写回与持久性：看起来像内存，不意味着像事务数据库

数组写路径通过 `seq_write` 获得页面、设置 DIRTY、修改显存内容，再释放引用。脏页被淘汰时，`find_slot` 先调用 `write_data` 写回后备位置，然后才使旧逻辑页失效。`__flush` 也提供了遍历缓存、写出脏页的路径。[S2][S13][S15]

这属于 write-back cache：写操作返回和数据被写回 SSD，不是同一个时刻。如果应用线程同时读写同一数据，仍然需要应用自己的同步；缓存引用计数保护的是页面驻留生命周期，不是为元素级数据竞争提供互斥。[P][S2]

更重要的是，`__flush` 这个函数名不能被理解为“完成了事务提交或断电持久化”。所阅读的路径是把脏页转换成 NVMe WRITE 并等待完成，不能仅据此承诺日志、原子提交、崩溃恢复、跨页一致性，或者等同于 NVMe FLUSH/FUA 的完整持久化语义。论文也明确不替应用保证 kernel 执行中崩溃后的状态。[S15][P]

对读多写少的只读图、特征表，这一限制相对容易接受；对需要可恢复更新的在线数据库，存储协议之上还要补一层应用一致性设计。这是适用性推论，不是仓库已经提供的能力。

## 九、多 SSD：条带与副本，解决的是不同问题

`get_backing_ctrl_` 和 `get_backing_page_` 支持两种映射。令逻辑页号为 `page`、设备数为 `device_count`：[S16]

| 模式 | 设备选择 | 设备内页号 | 主要交换条件 |
|---|---|---|---|
| `STRIPE` | `page % device_count` | `page_start + page / device_count` | 容量可聚合，但某页只能到其归属设备读取 |
| `REPLICATE` | 读 miss 时轮转选择设备 | `page_start + page` | 读负载更容易分摊，但完整副本消耗容量 |

复制模式淘汰脏页时，代码依次向相关控制器写回。它不是一条自动具备副本仲裁、故障切换和修复能力的分布式存储协议。[S13][S16]

还有两点容易读错：

- `range_t` 默认模式是 `REPLICATE`。所阅读的 BFS 初始化没有显式传入 `STRIPE`，所以不能把其多盘配置直接描述成容量条带化。
- 主读路径的 queue 选择使用 SM ID 对控制器 0 的 queue-pair 数取模，而真正的设备选择在缺页处理里完成。它不是每次访问都做最短队列调度；扩展到异构队列配置时，应额外检查不同控制器的队列数量是否兼容。[S2][S10][S12]

论文四盘图实验也明确采用数据复制来扩大聚合带宽。**四块盘参与读取，不等于可用数据集容量自动变成四倍。**[P]

## 十、三个推导：BaM 什么时候有机会快

以下模型用于解释趋势，不是仓库自带的性能预测器，也不是实测结果。

### 10.1 存储延迟靠足够多的独立请求隐藏

根据 Little 定律，若希望获得带宽 `bandwidth`，每个请求传输 `request_size` 字节，平均在途延迟为 `latency`，所需并发量近似为：

```text
required_outstanding ≈ bandwidth × latency / request_size
```

假设目标是 25 GB/s、平均延迟 100 μs：

| 请求大小 | 所需在途请求数，向上取整 |
|---|---:|
| 4 KiB | 611 |
| 512 B | 4,883 |

这里带宽使用十进制 GB/s，页面大小使用二进制 KiB。这个算例说明：小请求要维持同样带宽，需要高得多的并发度。

GPU 有很多线程，只能说明存在提供并发的潜力；**有效并发是同时可发现、可发出、并且拥有运行和缓冲资源的独立 I/O 数**。frontier 太小、访问形成长依赖链、可淘汰槽不足，都可能使这个数远小于线程总数。[S2][S3]

论文里的 uk-2007-05 BFS 正提供了这种反例：深层遍历中的小 frontier 限制重叠请求数量，使多 SSD 扩展收益受限。[P]

### 10.2 粒度决定 I/O 放大，缓存决定重复代价

假设一次冷访问只需要一个 8 字节元素，且读入的其他数据最终都没有被使用：

```text
512 B page → payload amplification = 512 / 8 = 64
4 KiB page → payload amplification = 4096 / 8 = 512
```

这不是说 BaM 必然产生这些放大：同页多个元素被使用时，分母应该累加这些有效数据。它只是说明，按需读取虽然避免了整列或整个文件预加载，仍然受最小访问粒度约束。

可以把有效读取速率粗略分解为：

```text
useful_value_rate
  ≲ min(device_IOPS_limit, link_bandwidth / page_size)
     × useful_values_per_fetched_page
```

更细的页减少无用字节，但提高 IOPS 需求；页内合并增加每次读取服务的有效元素数；缓存命中避免后续重复读；第二次一致性请求则消耗额外请求预算。它们分别作用于不同项，不能都笼统称为“零拷贝收益”。[S2][S3][S14]

### 10.3 隐藏在显存里的元数据，可能先把容量用完

当前 `data_page_t` 含状态字和槽位偏移，并显式按 32 字节对齐。结合 `range_t` 按所有逻辑页分配数组的实现，可以按每项 32 字节估算主要页状态表：[S2][S12]

```text
page_metadata_bytes ≈ dataset_bytes / page_size × 32
```

若逻辑数据集为 1 TiB：

| 缓存页大小 | 逻辑页数 | 仅 `data_page_t` 表的显存估算 |
|---|---:|---:|
| 512 B | 2,147,483,648 | 64 GiB |
| 4 KiB | 268,435,456 | 8 GiB |
| 64 KiB | 16,777,216 | 512 MiB |

这些还没有计算真正的数据缓存、缓存槽元数据、PRP、队列和应用工作集。因此，**“后备 SSD 容量很大”不等于“当前实现可以几乎零显存开销地寻址任意大的数组”**。512 B 细粒度的代价不只是更多 IOPS，还包括随整个数据集增长的页元数据。

队列也有不直观的固定成本：每个 SQ 初始化时分配 65,536 个 `padded_struct` 作为 CID 状态表，而该类型按 32 字节对齐。按这一布局，每个 SQ 的 CID 表约为 2 MiB，128 个 SQ 约为 256 MiB；如果四个控制器各建 128 个 SQ，光这部分就约 1 GiB。[S5][S17]

这给出一个实际调参顺序：先核算显存预算，再定页大小与缓存容量，再决定队列数量。不能只看“减少 I/O 放大”和“增加提交并行度”，却忽略它们对 HBM 的共同消耗。

## 十一、如何正确理解论文的性能结论

BaM 论文的结果有价值，但几个数字回答的不是同一个问题：

| 论文结果 | 比较口径 | 不应扩大成的结论 |
|---|---|---|
| BFS 平均约 1.00×、CC 平均约 1.49× | 四块 Optane SSD、复制数据，与包含文件加载时间的 host-memory 目标系统比较端到端时间 | SSD 普遍比已驻留 DRAM 的逐次访问更快 |
| 数据分析最高约 5.3× | 论文 NYC Taxi 查询与 RAPIDS v21.12 基线，收益涉及按谓词减少后续列读取等因素 | 任何数据分析任务都能获得该加速比 |
| 摘要中最高约 21.7× 成本优势 | 论文当时的容量扩展与硬件价格比较 | 2026 年服务器整机仍具有同样成本比 |
| 热缓存有效带宽最高约 430 GB/s | 软件缓存命中时的数据访问实验 | SSD 实际输出了 430 GB/s |
| VectorAdd 约慢 1.51× | 与适合规则访问、能够流水重叠的主动分块基线比较 | GPU 自主 I/O 总优于 CPU 编排 |

来源分别为论文摘要、§5.1—§5.4 与对应图表；全部是论文实验，不是本文重新测试。[P]

其中最容易被误读的是图实验。把“数据尚在存储上，需要完成整次任务”作为起点，BaM 可以边发现需求边读，不必先把整个数据集加载进主机内存。把“主机 DRAM 已经装好数据，可以多次复用”作为起点，比较就不同了。前者的优势不能直接证明后者的每次访问延迟更低。

VectorAdd 的负结果同样重要。论文把它的劣势关联到 read miss 与 write-back 未能充分重叠。当前同步写回路径也能解释这种风险：淘汰脏页时，请求者要先承担写回，再装入新页；**但只凭这条源码，不能把论文测得的 1.51× 数字认定为当前快照的结果**。[S13][P]

从这些证据更稳妥的结论是：BaM 用 GPU 并行性对抗存储延迟，并减少 CPU 协调与过度读取；它最有吸引力的场景，是大容量、细粒度、数据依赖且拥有充足独立请求的访问，而不是规则大块吞吐的无条件替代品。

## 十二、从研究原型到可部署系统，还缺哪些核验

这里不是给仓库做泛化的质量评级，而是指出沿本文调用链能够直接看到的工程边界。

### 12.1 数据平面没有消除错误处理需求

所阅读的 `cq_poll` 在匹配 CID 与 phase 后返回；附近打印 NVMe 错误状态的逻辑处于注释中，并没有在这条路径上形成面向应用的完整错误返回链。多处等待循环也没有显式超时出口。[S3]

因此不能假设 SSD 故障、命令错误、队列停止前进时，kernel 总会快速失败并清理缓存 BUSY 状态。生产化至少要设计完成状态传播、超时与取消、控制器复位，以及挂起页面和引用的回收规则；这些是需要补充验证的要求，不是本次已经复现的故障。

### 12.2 页面引用复用有收益，也会占住淘汰空间

`bam_ptr` 会保留当前页的地址和引用，在换页或结束使用时释放；这样可减少同页连续访问中的重复缓存查询。[S18]

但引用保留得越久，可淘汰槽就越少。结合 `find_slot` 的持续探测逻辑，可以推导：如果应用让过多页面一直被固定，同时又发起需要新槽的访问，就可能出现严重停顿；若释放依赖于尚未完成的后续获取，还需要分析是否形成循环等待。[S13][S18]

这不是“引用计数有问题”，而是软件内存层级把资源进展条件显式暴露了出来。缓存大小要与应用同时持有的页面集合一起设计。

### 12.3 历史修复说明哪些不变量最脆弱

两个与本文直接相关的修复值得对照阅读：

- 2024 年 10 月 16 日的 `2484872` 修复 `find_slot` 中页号截断引起的错误淘汰：地址身份被截断后，可能把仍被使用的页判断成可淘汰页。
- 2024 年 11 月 8 日的 `11cbf91` 修复同一逻辑页被重复装入不同缓存槽的竞争。[H1][H2]

这两个问题在本文快照中已经包含修复，不能再作为当前未修复漏洞报告。它们的启示是：这样的系统不仅依赖“锁有没有加”，还依赖**页身份宽度、状态检查时机，以及不同表之间映射关系的一致性**。

## 十三、复现应当分层，不能直接照抄一条 benchmark 命令

仓库 README 要求支持 PCIe P2P 的系统、能暴露所需 peer-memory 的 NVIDIA 数据中心 GPU，并依赖 Volta 及以后的同步能力；它还列出了 Above 4G Decoding、IOMMU/ACS 配置要求。当前 README 写有 CUDA 12.3+，同时提醒测试内核主要是 5.8.x，较新内核可能不兼容。[S1]

这些是仓库声明的要求，不是本文认证的兼容矩阵。CMake 仍有旧 CUDA 查找写法，更说明“README 的最低版本字段”和“某套现代工具链实际可构建”需要分别验证。[S19]

尤其要注意：仓库流程会把 NVMe 设备从 Linux 默认驱动解绑，并使用自定义模块；应用还能直接写后备块地址。**只能对确认无业务数据、无挂载、无其他消费者的专用测试盘执行。本文不运行驱动解绑、设备重置或裸盘写入。**[S1]

合理的实验顺序是：

1. **先确认环境与映射。** 记录 GPU、SSD 型号与固件、PCIe 拓扑、BAR 能力、驱动、CUDA、内核和实际 queue 限制，验证目标显存区域能被目标设备正确访问。
2. **只测 block 层。** 用固定提交的 block benchmark 检验数据正确性和队列进展，再扫描请求大小、队列数、深度和设备数；此时不要把结果称作完整数组访问性能。
3. **加入数组和缓存。** 分别测试冷缓存、热缓存、同页竞争、不同页竞争和小缓存反复淘汰，观察真实读取数、命中数与 `extra_reads`。
4. **加入写路径。** 在专用区域验证脏页淘汰、显式写回和重新读取；并单独分析错误返回和退出时未完成请求，不能仅以 kernel 返回代表可恢复持久化。
5. **最后测试应用。** 区分初始化、数据准备、计算、写回时间，注明是否预加载、缓存是否热、后备分布是 STRIPE 还是 REPLICATE，再讨论端到端收益。[S1][S2][S3][S20]

AOE 文档尤其值得细读：作者明确说明它面向 artifact available/functional 验证，小型原型并不能复现论文全部结果。甚至其中单盘 block 示例正文给出的 512 B 参数，与链接日志实际记录的 4 KiB 参数也不同；引用历史数字前，应先核对日志里的真实命令，而不是只读周围说明。[S20][S21]

## 结语：BaM 把显存扩展问题改写成了 GPU 侧的并发协议问题

BaM 没有让 SSD 获得 HBM 的延迟。它做的是把数据依赖的发现者和 I/O 发起者放回同一执行位置，再用缓存合并、页面生命周期协议和并发 NVMe 队列，把 GPU 的大量独立线程转化成存储系统需要的在途请求。

理解这套系统，最值得继续跟读的是 `seq_read → acquire_page → find_slot → read_data → sq_enqueue → cq_poll → enqueue_second`。这条链同时解释了它为什么能省掉逐请求 CPU 协调、为什么仍然要等待设备，以及为什么性能边界最终落在**有效并发、访问粒度、显存元数据预算和跨设备可见性**上。[S2][S3][S13][S14]

---

## 参考资料与源码定位

所有 `[S]` 源码链接固定到本文分析的提交；`[H]` 为历史修复，`[P]` 为论文。正文公式与容量算例为基于上述实现的推导。

- [P] [论文：GPU-Initiated On-Demand High-Throughput Storage Access in the BaM System Architecture，arXiv v3](https://arxiv.org/abs/2203.04910v3)，重点阅读 §3.3、§3.4、§4.4、§5。
- [S1] [README：系统目标、硬件条件与安装流程](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/README.md)。
- [S2] [page_cache.h：页面状态、数组访问与 acquire_page](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/include/page_cache.h#L1109)。
- [S3] [nvm_parallel_queue.h：并发 SQ/CQ 协议](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/include/nvm_parallel_queue.h#L35)。
- [S4] [ctrl.h：Controller 初始化和寄存器注册](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/include/ctrl.h#L147)。
- [S5] [queue.h：QueuePair、GPU 队列和 CID 表分配](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/include/queue.h#L58)。
- [S6] [module/pci.c：mmap_registers](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/module/pci.c#L66)。
- [S7] [module/map.c：GPU peer-memory 固定和 DMA 映射](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/module/map.c#L305)。
- [S8] [buffer.h：GPU 缓冲区分配与 DMA 缓冲区构建](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/include/buffer.h#L54)。
- [S9] [page_cache.h：缓存数据区与 PRP 初始化](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/include/page_cache.h#L764)。
- [S10] [BFS：frontier kernel 的按需边读取](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/benchmarks/bfs/main.cu#L312)。
- [S11] [bafs_ptr.h：数组指针包装](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/include/bafs_ptr.h#L18)。
- [S12] [page_cache.h：range_t 为全部逻辑页分配元数据](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/include/page_cache.h#L982)。
- [S13] [page_cache.h：find_slot 与脏页淘汰](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/include/page_cache.h#L1786)。
- [S14] [page_cache.h：enqueue_second 和 read_data](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/include/page_cache.h#L1959)。
- [S15] [page_cache.h：显式脏页写回](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/include/page_cache.h#L584)。
- [S16] [page_cache.h：STRIPE 与 REPLICATE 映射](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/include/page_cache.h#L555)。
- [S17] [nvm_types.h：padded_struct 与队列元数据](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/include/nvm_types.h#L94)。
- [S18] [page_cache.h：bam_ptr 的页面引用复用](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/include/page_cache.h#L415)。
- [S19] [CMakeLists.txt：构建配置](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/CMakeLists.txt#L1)。
- [S20] [ASPLOS AOE：验证范围和实验入口](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/asplosaoe/README.md#L1)。
- [S21] [AOE 单盘 block 历史日志：实际使用 4 KiB 请求](https://github.com/ZaidQureshi/bam/blob/315fadfc5c5c018a64596157bfac94ecbb7d87a2/asplosaoe/nvm_block_bench_1_sam.log#L1)。
- [H1] [历史修复 11cbf91：同页重复装入竞争](https://github.com/ZaidQureshi/bam/commit/11cbf91)。
- [H2] [历史修复 2484872：页号截断导致错误淘汰](https://github.com/ZaidQureshi/bam/commit/2484872)。
