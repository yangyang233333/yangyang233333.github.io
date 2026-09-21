---
title: "jemalloc 原理详解：从小对象分配到页面回收"
date: 2026-09-21T16:32:00+08:00
draft: false
description: "基于 jemalloc 5.4.0 源码，解析大小类、arena、slab、tcache、extent 与 decay，并用可运行实验解释 free 后内存为何不立即归还操作系统。"
tags: ["jemalloc", "内存管理", "Linux", "性能优化", "C/C++"]
categories: ["系统原理"]
---

jemalloc 的核心不是“找到一块空闲内存”，而是**把不同成本的内存管理工作拆成多层：线程缓存处理高频请求，arena 分散共享状态，小对象通过 slab 紧凑存放，页级分配器负责复用、拆分与回收映射。**

理解这几层，就能回答三个常见问题：为什么 `malloc` 通常不需要系统调用？为什么 `free` 之后 RSS 不下降？为什么增大缓存、增加 arena 能提高吞吐，却可能增加内存占用？

本文以截至 **2026-09-21** 核对的 **jemalloc 5.4.0** 发布版及同版本手册为基线。数值示例和实验采用 64 位 Linux、4 KiB 页、16 字节 quantum、默认大小类；页大小、构建选项和运行配置改变后，应通过 `mallctl` 重新查询，不能照搬数字。

## 一、先分清：对象、页和物理内存不是同一层

应用请求的是字节，操作系统管理的是页，而 CPU 真正访问的是经过地址转换的物理内存。jemalloc 工作在这几者之间，但它不能把任意多个物理页变成物理连续内存，也不会替应用移动仍然存活的普通 C/C++ 对象。

默认配置下，可以把主要分配路径画成下面这样。图中省略了 profiling、显式对齐和部分特殊缓存分支。

```text
application: malloc / free
                 |
        size class + thread state
                 |
          thread-local tcache
          hit |          | miss / flush / bypass
              |          v
              |        arena
              |          |
              |     +----+--------------------+
              |     |                         |
              |   small                     large
              |     |                         |
              | bin -> slab -> regions   dedicated extent
              |     |                         |
              |     +------------+------------+
              |                  |
              |         page allocator (PA)
              |                  |
              |        PAC: reusable extents
              |                  |
              |       extent hooks / OS VM
              |                  |
              |       mmap / madvise / munmap
              v
        return object pointer
```

这里有几个容易混淆的名词：

- **arena** 是一组分配器状态与内存资源的管理域，不是一块固定大小的连续内存。
- **bin** 在 arena 中管理一个小对象大小类的 slab；tcache 中的 cache bin 则主要保存同一大小类的对象指针。两者不是同一个结构。
- **slab** 是用于某个小对象大小类的一段内存，切分成等长的 region；一个 region 对应一个对象槽位。
- **extent** 描述页对齐、虚拟地址连续的内存区间。它可以承载 slab、单个大对象，也可以处于空闲状态。

因此，不能把现代 jemalloc 简化成旧教程里固定的“chunk → run → object”层级。本文使用 5.4.0 实际实现中的 slab、extent、PA 和 PAC 术语。

还有一个贯穿全文的区别：**获得虚拟地址映射，不等于立即获得同样多的驻留物理内存。** 在常见 Linux 匿名映射中，物理页通常随实际访问建立；同样，丢弃物理页内容也不一定要撤销虚拟地址映射。

## 二、大小类：用有限种规格处理任意字节请求

假设应用反复申请 100、103、108 字节。如果每种长度都管理一套空闲块，元数据、查找和碎片控制都会很复杂。jemalloc 把请求向上取整到有限的 **size class**，让相近大小的请求复用同一种对象槽位。

在本文配置下，通过 `nallocx(size, 0)` 查询得到：

| 请求大小 | 对应大小类 | 类内多出的字节 |
| --- | --- | --- |
| 1 B | 8 B | 7 B |
| 9 B | 16 B | 7 B |
| 33 B | 48 B | 15 B |
| 100 B | 112 B | 12 B |
| 129 B | 160 B | 31 B |
| 4097 B | 5120 B | 1023 B |

`nallocx` 只计算给定大小和 flags 对应的可用大小，不实际分配对象。普通 `malloc` 的调用者仍应按请求长度使用内存；不要因为知道分配器内部向上取整，就越过语言或 API 规定的对象边界。

### 不是简单地全部取整到 2 的幂

除最小的 tiny 类和起始特殊分组外，普通分组覆盖 `(base, 2 × base]`，其中 `base` 是 2 的幂。默认每组有四档：

```text
delta = base / 4

base + delta
base + 2 * delta
base + 3 * delta
base + 4 * delta

base = 128:
160, 192, 224, 256
```

这样既能用位运算、索引和查表快速定位大小类，也避免了直接按 2 的幂取整时过大的浪费。

手册提到的“内部碎片约不超过 20%”，需要明确分母。对于普通分组，如果请求刚刚超过 `base`，会取整到 `1.25 × base`：多出的空间接近实际分配大小的 20%，也接近请求大小的 25%。**这不是进程总内存浪费的上界，最小的特殊大小类也不满足这个统一比例。**

### small 和 large 的边界也由配置决定

在本文的默认 4 KiB 页配置中：

- small 的最大大小类是 **14336 B，即 14 KiB**。
- large 的最小大小类是 **16384 B，即 16 KiB，也就是四页**。
- small 一共有 **36 个 bin 大小类**。

例如申请 14337 B，向上取整后已经走 large 路径。这里的分类看大小类，不是看业务语境中“这个对象感觉大不大”。

另一个独立边界是 `tcache_max`，默认是 **32 KiB**。所以 **large 不等于绕过 tcache**：默认配置下，部分 large 大小类仍然可以在线程缓存中复用。

## 三、arena：用资源分区降低竞争

如果所有线程共用一套 slab、空闲 extent 和统计状态，那么即使查找算法很快，共享锁和缓存行争用也会限制扩展性。jemalloc 的一个解决办法是让不同线程分散到多个 arena。

arena 内部有各大小类的 bin、页级分配状态，以及相关元数据和统计。普通线程的 arena 关联保存在其线程状态中；线程通常复用已关联的 arena，而不是每次 `malloc` 都重新在全局挑选。

但 **arena 不是每线程独占堆**。多个线程可以关联同一个 arena，也可以操作来自其他 arena 的对象。共享的 bin、extent 集合等仍需要同步；arena 只是缩小竞争范围，并没有让整个分配器无锁。

5.4.0 手册中，自动 arena 数量上限默认是 CPU 数的四倍，单 CPU 时为一个。它是自动复用 arena 的数量上限，不代表启动时就为每个 arena 预分配同样大小的堆。arena 的创建和资源使用具有按需特征。

实现还支持 bin 分片，让同一个 arena、同一种大小类的访问分散到不同 bin shard。默认 bin 分片数为一个，不要把“支持分片”误读成所有部署都已经开启了额外分片。

### 为什么 arena 不是越多越好？

arena 之间主要独立管理内存。增加 arena 可以降低竞争，却也使空闲 slab、extent 和元数据分散：一个 arena 有合适的空闲空间，不代表另一个 arena 的请求一定能直接拿到它。

因此，需要同时权衡两种成本：

- arena 较少：资源更集中，复用机会更多，但共享锁和缓存行竞争可能增大。
- arena 较多：并发竞争更少，但可能保留更多局部空闲资源，增加碎片与元数据开销。

`percpu_arena` 可以按运行 CPU 选择 arena，但默认关闭。即使开启，它也不等价于 NUMA 内存绑定：CPU 调度、首次触页线程和操作系统内存策略仍会影响物理页落在哪个 NUMA 节点。

## 四、小对象：bin、slab 和位图怎样配合

一个 arena 的某个 bin，只管理一种小对象规格。以 64 B 大小类为例，本文配置中一个 slab 是 4096 B，可以放 64 个 region。

```text
arena
  |
  +-- bin: 64-byte objects
        |
        +-- slabcur
        |     [used][free][used][free] ...
        |      64 B  64 B  64 B  64 B
        |
        +-- non-full slabs
        |
        +-- full slabs

slab metadata: bitmap + free-region count
```

arena bin 中几个关键成员是：

- `slabcur`：当前优先服务分配请求的 slab。
- `slabs_nonfull`：还有空位的其他 slab，使用堆结构组织。
- `slabs_full`：已经填满的 slab 的跟踪结构。

从 arena 取对象时，优先使用当前 slab，再尝试已有的非满 slab；两者都无法满足时，才向页级分配器申请新 slab。非满 slab 的选择具有较旧、较低地址优先的倾向，有助于集中分配，而不是无目的地把存活对象摊开。

### 一个 slab 不一定只有一页

如果对象大小不能整除页大小，只用一页会留下尾部空间。jemalloc 会为不同大小类计算相应的 slab 大小；默认配置下的实际查询结果包括：

| 对象大小类 | slab 大小 | 每个 slab 的 region 数 |
| --- | --- | --- |
| 48 B | 12288 B，三页 | 256 |
| 64 B | 4096 B，一页 | 64 |
| 112 B | 28672 B，七页 | 256 |
| 14336 B | 28672 B，七页 | 2 |

这说明“slab 就是一页”和“一个 slab 固定放几十个对象”都不准确。对应数据可以从 `arenas.bin.<i>.size`、`arenas.bin.<i>.slab_size`、`arenas.bin.<i>.nregs` 读取。

### 位图负责标记槽位，不必给每个小对象加一个独立头部

每个 slab 的元数据记录 region 使用情况、空闲数量等信息。分层位图通过位操作定位可用槽位，避免为了找一个对象而线性遍历整块 slab。定位到 region 后，再根据 slab 起始地址和 region 大小计算对象地址。

用户对象的信息也不是统一放在“指针前面几个字节”。5.4.0 使用 `emap` 和内部的 radix tree 建立地址到 extent 元数据的映射；`edata_t` 保存 extent 的 arena、大小类、状态、是否为 slab 等信息。对于 slab，映射还覆盖内部页，使任意合法对象指针都能找到对应管理信息。

读取这些信息是 `free` 能够只接收指针、却仍然找到所属 arena 和大小类的基础。地址查询也有线程侧缓存来降低常见查询成本。

### 小对象碎片的难点：一个活对象可以拖住整个 slab

假设很多 slab 里各剩一个活对象。应用实际存活的数据可能很少，但这些 slab 都还不是整体空闲区间，不能直接交还页级分配器重新分配。

普通 C/C++ 指针不能被分配器随意改写，所以 jemalloc 不能把这些对象搬到同一个 slab 里再释放其他 slab。它能通过放置与复用策略减少这种局面，却不能像可移动对象的垃圾回收器那样任意压缩堆。

这类 **slab 占用率造成的碎片**，和上一节“100 B 取整为 112 B”的 **大小类内部碎片**，是两个不同的问题。

## 五、tcache：把共享状态的操作批量化

虽然从 slab 的位图里找一个槽位已经很快，但访问 arena bin 仍涉及共享状态。tcache 再把常见路径往前挪了一层：线程为不同大小类缓存一批可复用对象指针。

命中时，分配通常只需要从本线程 cache bin 的指针栈取出一个指针；释放时，如果该大小类允许缓存且有空间，就把指针放回本线程缓存。这里的栈主要是缓存中的指针数组，不要把它想成必须在每个空闲用户对象内部串起的链表。

**tcache 命中的普通路径可以避免 arena 共享锁；这不代表所有 `malloc`、所有 `free` 都没有锁或系统调用。** 初始化、缓存补充、批量归还、统计事件和页级操作都可能进入更重的路径。

### 从一次 100 B 分配看慢路径怎样被摊薄

1. 将 100 B 映射到 112 B 大小类。
2. 查看当前线程对应的 cache bin；有缓存对象就直接返回。
3. 缓存为空时，选择 arena 和对应的 bin shard，从已有 slab 批量获取 region。
4. slab 空间不足时，先退出相应 bin 临界区，再申请新 slab，然后重新检查、补充。
5. 将一批对象放进 tcache，返回其中一个；后续多次分配可以消耗这一批对象。

批处理的价值是让一次共享锁操作服务多个对象。新 slab 的页级分配也不会简单地全程压在同一个 bin 锁下面。

### 5.4.0 的缓存目标不是固定的“每次填一半、刷一半”

这是阅读旧版本资料时需要特别留意的变化。5.4.0 改为根据各 bin 在 GC 事件之间观察到的使用情况，调整补充数量与保留目标。

从 `tcache_ncached_target.h` 可以看到几项具体机制：

- 发生补充后，提高后续补充目标，并受缓存容量相关上限约束。
- GC 观察到缓存持续有余量时，降低补充目标。
- 根据 low-water mark 推导需求估计，重算应保留多少对象。
- 释放导致缓存溢出时，调整保留目标，再批量归还多余对象。

low-water mark 记录观察窗口中缓存数量到达过的低点。它是廉价的需求信号，不是精确的业务工作集测量。小对象 GC 还会使用地址邻近性启发式，优先刷走某些“远离”当前局部区域的对象；这里的 remote 是地址局部性概念，不等于 NUMA 远端节点。

因此，旧文章中的固定缓存数量、固定 flush 比例，不能直接当作 5.4.0 的实现说明。`tcache_nslots_small_max`、`lg_tcache_flush_small_div` 等七个旧控制项已在这一版本移除；其中一些旧配置字符串会被兼容性逻辑静默忽略，而相应 `opt.*` 查询返回 `ENOENT`。

### 跨线程 free：缓存归属和对象归属不是一回事

一个由线程 A 分配的对象，可以在线程 B 安全接管所有权后释放。若满足缓存条件，它可以先进入 **B 的 tcache**，而不是必须立即回到 A 的线程缓存。

```text
thread A allocates from arena X
              |
      application transfers ownership
              |
thread B frees -> thread B's tcache
              |
       batch flush when needed
              |
    original arena X / original bin shard
```

对象所在 slab 的 arena 归属并没有因此改变。批量 flush 时，jemalloc 读取对象元数据，按原 arena 和 bin shard 分组归还。

这既减少跨线程释放时的即时争用，也引入内存滞留的可能：对象已被业务释放，却仍被某个线程缓存持有。尤其是线程完成一个高峰任务后长期休眠，基于分配、释放活动触发的缓存 GC 未必继续推进。

还有一个关键后果：**对象停留在 tcache 时，arena 侧并没有把对应 region 当作已归还的空闲槽位。** 所以即使业务已经 `free`，它仍可能让 slab 无法完全空闲。这也是后面实验中 `allocated` 和 active 页数没有立刻归零的原因。

## 六、大对象与 extent：不是“一次 malloc 对应一次 mmap”

大对象不再从小对象 slab 的位图中获取 region，而由独立的 extent 承载。这里的“独立”指分配器的管理单元，不意味着操作系统一定为它创建了一条新的映射。

一个大对象的 extent 可以来自已有空闲 extent 的复用，也可以从更大的映射中拆出；只有现有资源无法满足时，才需要进一步申请地址空间。

通常需要区分三种大小：

- 应用请求大小，例如 1 MiB 左右的某个长度。
- 取整后的可用对象大小。
- 底层承载它的 extent 大小。

后两者也不保证总相等。显式对齐、构建时启用的 cache-oblivious 大对象布局、保护页等机制，都可能改变页级开销。特别是 cache-oblivious 布局会避免所有大对象都从完全相同的页内偏移开始，以改善某些缓存索引冲突，但可能付出额外页面成本。

### PA 负责分派，PAC 管理经典页级路径

5.4.0 的 PA 层对接经典页分配器 **PAC，Page Allocator Classic** 和可选的 **HPA，Hugepage Allocator**。默认 `hpa:false`，本文的状态转换与实验主要讨论 PAC。

在普通、未启用额外特殊缓存的路径上，PAC 会优先复用空闲资源：尝试 dirty extent，必要时尝试 muzzy extent，再尝试 retained 空间及其扩展；仍不能满足时，才通过 extent hooks 申请新的映射。5.4.0 还支持可选的小 extent 缓存 SEC，默认 PAC 配置不启用它。

空闲 extent 不是放在一个从头扫描到尾的链表里。`eset` 按量化后的大小组织 extent 集合，利用位图定位非空档位，档内用堆管理候选；同时保留用于回收顺序的链表信息。

找到候选后，还要检查大小、地址、对齐及状态是否满足要求：

- **split**：从较大的 extent 中拆出当前请求需要的区间，剩余部分继续管理。
- **coalesce**：将相邻且兼容的空闲 extent 合并，提高后续大请求的可满足性。
- **grow**：现有保留空间不足时，扩展地址空间来源，而不是只申请一个用户对象那么大的映射。

相邻不代表必然能合并。arena 归属、extent 状态、提交属性、保护布局以及 hooks 是否支持合并等条件，都可能限制操作。也不要把它当作纯粹的 buddy allocator：对象大小类并非全是 2 的幂，extent 拆分与合并也不只发生在固定伙伴之间。

### 大请求隔离、HPA 和 pinned 是三件不同的事

`oversize_threshold` 默认是 8 MiB，用于将超过阈值的大请求引导到专门的 arena，减少它们与普通请求混杂造成的碎片。显式指定 arena 的请求有自己的规则；这不是 small/large 分类的第三种对象布局。

HPA 则从 huge page 的利用率出发组织页级分配，配合大页提升与回收策略。它不等于“所有大对象都自动使用大页”，也不等于 `opt.thp`。默认使用 PAC 时，不能把 HPA 的内部策略套进来解释观测结果。

5.4.0 新增的 `EXTENT_ALLOC_FLAG_PINNED` 又是另一条路径：自定义 extent 分配 hooks 可以标记不可按普通方式回收的映射，例如某些 HugeTLB 映射。这类空闲 extent 进入单独的 pinned 集合，优先用于适合的后续分配，而不是进入通常的 decay/purge 流程。

这个标记本身不是一次 `mlock`，也不会把普通 `malloc` 自动变成设备 DMA 所需的 pinned memory。相关闲置资源由 `stats.pinned` 等统计单独反映。

### calloc 和 realloc 在这套结构上的含义

`calloc` 必须返回满足清零语义的对象，但实现可以利用已知为零的页；如果复用的是旧的脏对象，就需要确保内容被清零。因此，“新映射可能延迟分配物理页”不代表任意 `calloc` 都没有触页成本。

`realloc` 前后落在相同大小类时，当前实现通常可以原地完成；大对象在尾部空间和 extent 拆分、合并能力允许时，也可能原地扩缩。其他情况下则可能分配新对象、复制内容并释放旧对象。调用者仍必须遵守 `realloc` 的 API 契约，不能依赖指针一定不变。

## 七、free 之后：内存要经过哪些回收阶段？

释放对象和归还物理内存，是不同层次的动作。对一个可缓存的小对象，最典型的链条是：

```text
application no longer owns object
                 |
                 v
              tcache
                 |
              flush
                 |
                 v
         free region in a slab
                 |
       all regions become free
                 |
                 v
         unused dirty extent
          |              |
      immediate reuse    decay / purge
                         |
                  +------+------+
                  |             |
                muzzy       retained / unmap
                  |
            further purge
                  |
           retained / unmap
```

这是一张常见路径的语义图，不是说每块内存都必须逐站经过。绕过 tcache、直接释放大对象、立即回收配置、自定义 hooks 和 pinned extent 都会改变路径。

### dirty、muzzy、retained 的准确区别

| 状态 | 对分配器的意义 | 对物理内存的典型影响 |
| --- | --- | --- |
| active | 仍承载分配器视角下的活跃对象或 slab | 页可能已经驻留，也可能尚未实际触页 |
| dirty | 已空闲，但内容可能被写过，尚未 purge | 通常仍关联可复用的物理页 |
| muzzy | 做过惰性 purge，物理回收时机交给 OS | 可能仍在 RSS 中，也可能已被回收 |
| retained | 保留虚拟映射，供后续复用 | 通常尚未触页、已 purge 或已 decommit，不应直接等同 RSS |
| pinned | 自定义 hooks 标记的闲置、不可普通回收的 extent | 不走通常 decay/purge，需要单独观察 |

在 Linux 普通匿名私有映射上，可以借助两个典型接口理解 purge：

- `MADV_FREE` 表示这些内容不再需要，内核可以稍后回收；调用完成不保证 RSS 立即下降。
- `MADV_DONTNEED` 丢弃相应页的现有内容，后续访问按匿名页语义重新获得零页内容；虚拟地址区间并不因此消失。

而 `munmap` 撤销的是地址空间映射本身。**purge 和 unmap 不是同义词。** jemalloc 最终使用哪条路径，还取决于平台能力、配置和 extent hooks。

### decay 不是给每个 free 对象设一个十秒定时器

PAC 对未使用页进行平滑、渐进的回收，维护时间窗口和历史信息，使用平滑衰减策略安排应回收的页数。它试图避免内存一空闲就全部丢弃，也避免每隔固定周期制造一次大规模回收尖峰。

5.4.0 的普通默认值是：

| 控制项 | 默认值 | 含义 |
| --- | --- | --- |
| `dirty_decay_ms` | 10000 | dirty 页使用约十秒的衰减窗口 |
| `muzzy_decay_ms` | 0 | 不维持常规的 muzzy 等待窗口，可直接推进更彻底的回收 |
| `background_thread` | false | 默认不启用后台回收线程 |
| `retain` | 本文 64 位 Linux 环境为 true | 倾向保留虚拟映射供后续复用 |

decay 值为 `0` 表示立即处理相应闲置页，`-1` 才表示禁用该阶段的自动 purge。尤其不要把 `muzzy_decay_ms:0` 理解成“永远不回收 muzzy”。专用 oversize arena 的默认回收行为也可能不同。

“十秒窗口”不是严格的逐页到期承诺：页可能提前被复用，实际执行依赖触发事件、调度和回收策略。不开后台线程时，相关工作主要由分配器活动驱动；开启后台线程，可以将页级回收更多地移出应用线程的关键路径。

但后台线程不能把所有其他线程的 tcache 一次性清空。对象如果尚未归还 arena，页级 decay 就还没有拿到可以整体处理的空闲 extent。

### 为什么 retained 大，并不等于同样多的物理内存泄漏？

64 位地址空间通常比物理内存宽裕。保留一段已清理的虚拟映射，下一次可以直接重用地址空间，减少重新映射、拆分和布局管理的成本。

这是一种“保留地址空间，按需重新获得物理页”的策略。它可能让 VSZ 或 `stats.retained` 保持很高，却不要求 RSS 同样高。反过来，如果工作负载又访问这些页，仍然要付出缺页处理、物理页分配和可能的清零成本。

## 八、看懂统计：不要只盯着 RSS

定位内存问题，至少需要把以下口径拆开：

| 统计或测量 | 应该怎样理解 |
| --- | --- |
| 应用请求字节数 | 业务层记录的存活对象请求长度之和，jemalloc 的普通聚合统计不能直接替代它 |
| `stats.allocated` | 分配器记账中的已分配对象字节数，涉及大小类取整和 tcache 批量记账 |
| `stats.active` | 活跃页的总字节数，不含独立元数据页以及 dirty、muzzy 空闲页 |
| `stats.metadata` | 分配器元数据自身的内存开销 |
| `stats.resident` | jemalloc 对自身 resident 数据页的统计估计，不是内核读取的进程 RSS |
| `stats.mapped` | jemalloc 的映射统计口径，不含 retained，不能直接当成整个进程的 VSZ |
| `stats.retained` | 被分配器保留的虚拟映射空间 |
| `stats.pinned` | 闲置 pinned extent 的字节数 |
| OS RSS / smaps | 整个进程在操作系统视角下的驻留情况，还包括栈、代码和其他映射 |

`stats.resident` 会计入 active、dirty、相关元数据和 pinned 页，但部分 active 映射可能还没触页；某些 muzzy 页又可能仍在 OS RSS 中。再加上 jemalloc 管理范围之外的映射，不能建立一条对任意进程都成立的 `resident == RSS` 关系。

同样，`stats.active - stats.allocated` 可以作为观察活跃页利用率的线索，却不能被机械地解释为“所有碎片”。大小类取整已经包含在对象大小里，线程缓存、元数据和虚拟映射保留也有各自口径。

### epoch 刷新统计，不负责清空线程缓存

动态统计有缓存，读取前通常先写一次 `epoch`，再查询需要的字段。高频采样可以预先通过 `mallctlnametomib` 解析名称，用 `mallctlbymib` 减少反复解析字符串的成本。

不过，刷新 `epoch` 并不等于同步清空所有 tcache。缓存内尚未批量归还的对象，以及并发线程的持续活动，都可能让统计和业务某一瞬间的“活对象数”不完全一致。不要把一次快照当成全进程停止后的精确账本。

## 九、一个可运行实验：分离 free、flush 和 purge

下面的程序创建一个独立 arena，关闭其自动 decay，申请 4096 个 64 B 对象并实际写入，然后依次观察业务释放、线程缓存 flush、arena purge。

分配时使用 `MALLOCX_TCACHE_NONE`，让实验对象明确来自指定 arena，避免被已有 tcache 对象干扰；释放时使用普通缓存策略，故意展示“已释放，但尚未归还 arena”的阶段。`allocated` 列只读取该 arena 的 `small.allocated`，其他 arena 的初始化和标准库分配不混入这一列。

保存为 `jemalloc_walk.c`：

```c
#include <jemalloc/jemalloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static void control(const char *name, void *old_value, size_t *old_size,
                    void *new_value, size_t new_size) {
    int error = mallctl(name, old_value, old_size, new_value, new_size);
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", name, strerror(error));
        exit(EXIT_FAILURE);
    }
}

static size_t arena_stat(unsigned arena, const char *field) {
    char name[96];
    size_t value = 0, length = sizeof(value);
    snprintf(name, sizeof(name), "stats.arenas.%u.%s", arena, field);
    control(name, &value, &length, NULL, 0);
    return value;
}

static void snapshot(unsigned arena, const char *label) {
    uint64_t epoch = 1;
    control("epoch", NULL, NULL, &epoch, sizeof(epoch));
    printf("%-12s %10zu %7zu %7zu %10zu\n", label,
           arena_stat(arena, "small.allocated"),
           arena_stat(arena, "pactive"),
           arena_stat(arena, "pdirty"),
           arena_stat(arena, "retained"));
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *version = NULL;
    size_t length = sizeof(version);
    control("version", &version, &length, NULL, 0);
    printf("jemalloc: %s\n", version);
    printf("nallocx(33)=%zu, nallocx(100)=%zu\n",
           nallocx(33, 0), nallocx(100, 0));

    unsigned arena = 0;
    length = sizeof(arena);
    control("arenas.create", &arena, &length, NULL, 0);
    const char *decays[] = {"dirty_decay_ms", "muzzy_decay_ms"};
    ssize_t disabled = -1;
    char name[96];
    for (size_t index = 0; index < 2; ++index) {
        snprintf(name, sizeof(name), "arena.%u.%s", arena, decays[index]);
        control(name, NULL, NULL, &disabled, sizeof(disabled));
    }
    control("thread.tcache.flush", NULL, NULL, NULL, 0);

    enum { count = 4096, object_size = 64 };
    void *objects[count];
    int flags = MALLOCX_ARENA(arena) | MALLOCX_TCACHE_NONE;
    puts("stage         allocated  active   dirty   retained");
    snapshot(arena, "initial");
    for (size_t index = 0; index < count; ++index) {
        objects[index] = mallocx(object_size, flags);
        if (objects[index] == NULL) {
            return EXIT_FAILURE;
        }
        memset(objects[index], 0x5a, object_size);
    }
    snapshot(arena, "allocated");
    for (size_t index = 0; index < count; ++index) {
        dallocx(objects[index], 0);
    }
    snapshot(arena, "freed");
    control("thread.tcache.flush", NULL, NULL, NULL, 0);
    snapshot(arena, "flushed");
    snprintf(name, sizeof(name), "arena.%u.purge", arena);
    control(name, NULL, NULL, NULL, 0);
    snapshot(arena, "purged");
    return EXIT_SUCCESS;
}
```

需要链接启用统计功能、未使用自定义符号前缀的 jemalloc。头文件和库应来自同一次安装；下面假设安装前缀为 `$HOME/opt/jemalloc-5.4.0`，按实际位置修改：

```bash
JEMALLOC_PREFIX="$HOME/opt/jemalloc-5.4.0"

cc -std=c11 -O2 -Wall -Wextra \
  -I"$JEMALLOC_PREFIX/include" jemalloc_walk.c \
  -L"$JEMALLOC_PREFIX/lib" \
  -Wl,-rpath,"$JEMALLOC_PREFIX/lib" \
  -ljemalloc -o jemalloc_walk

env -u MALLOC_CONF ./jemalloc_walk
```

清除环境变量是为了减少已有调参的干扰，不会覆盖编译时配置或其他配置来源。程序打印的版本字符串则用来确认真正运行的库。本文实验从 5.4.0 tag 构建，启用 stats，其余相关选项使用前述默认值；可重复得到以下输出，其中首行只保留版本号：

```text
jemalloc: 5.4.0
nallocx(33)=48, nallocx(100)=112
stage         allocated  active   dirty   retained
initial               0       0       0          0
allocated        262144      64       0    1835008
freed              7424       2      62    1835008
flushed               0       0      64    1835008
purged                0       0       0    2097152
```

`allocated`、`retained` 的单位是字节；`active` 和 `dirty` 列读取的是 `pactive`、`pdirty`，单位是页，不是字节。

这组结果展示了四件不同的事：

1. **申请后**：4096 × 64 B = 262144 B，正好占 64 个 4 KiB slab。页级来源同时保留了尚未使用的地址空间。
2. **业务全部释放后**：仍有缓存对象未归还 arena，使两页保持 active，其余 62 页已经成为 dirty。此时业务已经没有这些活对象，但分配器的 `small.allocated` 仍为 7424 B。
3. **flush 后**：当前线程缓存归还对象，最后两个 slab 也整体空闲；active 归零，dirty 增至 64 页。物理页回收还没有发生。
4. **purge 后**：dirty 归零，但 retained 增至 2 MiB。这里没有出现“free 后地址空间消失”，而是空闲页被处理后，映射留下继续复用。

具体缓存余量和映射增长量属于此次运行结果，不是 API 承诺。实验也没有直接采样 RSS，所以它证明的是 jemalloc 内部状态转换；如果要验证内核驻留页变化，应在每个阶段另行采样进程的 `smaps_rollup` 等指标。

`thread.tcache.flush` 只处理调用线程的缓存，不能拿这个单线程实验的行为推导“调用一次就清空整个服务的线程缓存”。

## 十、调优要先判断内存卡在哪一层

不要先找一串所谓“高性能 jemalloc 参数”。更可靠的顺序是确认分配器与版本，再分辨瓶颈来自对象存活、slab 利用率、线程缓存、页回收，还是根本不属于 jemalloc 的映射。

| 观察到的现象 | 优先检查 | 调整的代价或边界 |
| --- | --- | --- |
| 稳态下 `allocated` 持续增长 | 业务对象存活量、容器容量、heap profile | 缩短 decay 不能回收真正的活对象 |
| `active` 相对对象记账明显偏大 | 大小类分布、slab 利用率、生命周期混合 | 移动不了活对象，强制 purge 也不能解决非空 slab |
| 大量线程在高峰后闲置 | 各线程缓存策略、任务结束后的显式 flush | 过度 flush 会提高之后的 refill 和锁开销 |
| dirty 页长期较多 | decay 设置、回收触发、后台线程 | 更激进回收可能带来更多缺页、清零与系统调用 |
| 分配吞吐或尾延迟差 | tcache miss、bin/extent 锁竞争、arena 分布 | 更多 arena 或更大缓存可能增加内存占用 |
| retained 很大而 RSS 不高 | 地址空间保留策略和平台需求 | 不应只为降低 VSZ 就关闭 retain |
| RSS 很高而 jemalloc 统计解释不了 | 线程栈、其他分配器、文件映射、muzzy、巨大页等 | 分配器参数不能控制所有进程内存 |

几条实践边界值得单独强调：

- **先确认替换真的生效。** 动态链接场景可用预加载等方式接入，但静态链接、符号前缀、多个运行时分配器混用会改变结果。不同分配器拥有的对象不能随意交叉释放。
- **缓存大小类边界和业务大小分布一起看。** 扩大 `tcache_max` 让更多请求能缓存，也可能让每个线程保留更昂贵的大对象。线程数乘上各档缓存，才是需要考虑的总体规模。
- **后台回收的目标是迁移回收工作，不是消灭成本。** 5.4.0 手册提示启动配置可能遇到初始化依赖问题；需要时优先评估通过 `mallctl("background_thread", ...)` 动态启用，并验证平台支持及返回值。
- **短生命周期和长生命周期对象混放，可能比参数更重要。** 调整对象布局、批次生命周期或适当的 arena 隔离，可以改善 slab 占用率；但额外 arena 自身也会带来资源分散。
- **定位存活对象时考虑采样 profiling。** 构建需要启用 profiling 支持，再通过 `prof.*` 控制和 `jeprof` 分析采样栈；它回答“对象由哪里分配”，不能代替页级碎片分析。

最后，使用相同的请求分布、并发度、运行时长和负载阶段，比较吞吐、P99、峰值与稳态 RSS、缺页及回收活动。只跑“同一线程不停申请再释放同一个小对象”的微基准，往往主要测到 tcache 命中路径，无法代表跨线程、混合生命周期的真实服务。

## 十一、阅读 5.4.0 源码的路线

与其从一个大文件从头读到尾，不如沿“分配入口 → 对象缓存 → slab → 页级分配 → 回收 → 统计”逐层追踪。

| 要回答的问题 | 优先阅读的位置 |
| --- | --- |
| `malloc` 怎样进入快慢路径？ | `src/jemalloc.c`、`src/malloc_dispatch.c`、`include/jemalloc/internal/jemalloc_internal_inlines_c.h` |
| 大小类如何编码和计算？ | `include/jemalloc/internal/sc.h`、`src/sc.c`、`src/sz.c` |
| tcache 怎样缓存和自适应调整？ | `include/jemalloc/internal/cache_bin.h`、`include/jemalloc/internal/tcache_ncached_target.h`、`src/tcache.c` |
| slab、位图和批量归还如何协作？ | `src/arena.c`、`src/bin.c`、`include/jemalloc/internal/bitmap.h` |
| 指针怎样找到管理元数据？ | `src/emap.c`、`include/jemalloc/internal/rtree.h`、`include/jemalloc/internal/edata.h` |
| 大对象和空闲 extent 怎样管理？ | `src/large.c`、`src/pa.c`、`src/pac.c`、`src/extent.c`、`src/eset.c` |
| 回收怎样到达操作系统？ | `src/decay.c`、`src/background_thread.c`、`src/ehooks.c`、`src/pages.c`、`include/jemalloc/internal/os/` |
| 配置与统计的真实含义是什么？ | `src/ctl.c`、`src/stats.c`、`doc/jemalloc.xml.in` |

5.4.0 做了前端和页级分配边界的重构，部分旧函数位置和抽象已经变化，例如旧的 PAI vtable 分派已被直接 PAC/HPA 调用替代。读源码时应固定 tag 或 commit，不要把旧版本文章里的函数图直接贴到当前版本上。

一句话收束：**jemalloc 用大小类降低对象管理复杂度，用 tcache 和 arena 降低同步成本，用 slab 和 extent 组织复用，再通过 decay 在“尽快归还物理页”和“保留资源供下一次快速使用”之间做权衡。** `free` 只完成这条链的起点，而不是承诺链上所有回收工作已经结束。

## 参考资料

- [jemalloc 官方仓库，5.4.0 发布 tag](https://github.com/jemalloc/jemalloc/tree/5.4.0)
- [jemalloc 5.4.0 发布说明：自适应 tcache、pinned extent 与内部重构](https://github.com/jemalloc/jemalloc/releases/tag/5.4.0)
- [本文固定的源码提交：7a34f18502e7b222724097cdcd499b437d189acc](https://github.com/jemalloc/jemalloc/commit/7a34f18502e7b222724097cdcd499b437d189acc)
- [5.4.0 手册源文件：实现说明、mallocx、mallctl、extent hooks 与统计口径](https://github.com/jemalloc/jemalloc/blob/5.4.0/doc/jemalloc.xml.in)
- [jemalloc 在线手册：Implementation Notes、Tuning 和 Mallctl Namespace](https://jemalloc.net/jemalloc.3.html)
- [5.4.0 内部头文件入口：下级 internal 目录包含 sc、cache_bin、tcache_ncached_target、edata 和 rtree](https://github.com/jemalloc/jemalloc/tree/5.4.0/include/jemalloc)
- [5.4.0 核心实现目录：arena、bin、tcache、large、PA/PAC、extent 和 pages](https://github.com/jemalloc/jemalloc/tree/5.4.0/src)
- [5.4.0 单元测试：大小类、位图、tcache 自适应目标和 decay 的边界行为](https://github.com/jemalloc/jemalloc/tree/5.4.0/test/unit)
