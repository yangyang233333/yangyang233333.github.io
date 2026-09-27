---
title: "jemalloc 内存分配链路：tcache、bin、arena 与 extent"
date: 2026-09-27T15:01:48+08:00
draft: false
description: "基于 jemalloc 5.4.0 源码，从 malloc(100) 追到页分配后端，解释 bin、arena、slab、extent 的关系，并用实测展示 free、tcache flush 与 purge 的区别。"
tags: ["jemalloc", "内存管理", "Linux", "C", "性能优化"]
categories: ["系统设计"]
---

理解 jemalloc，可以先抓住一条主线：**对象尽量在线程本地复用，小对象按尺寸组织，底层再按页管理内存。** 因而一次 `malloc` 通常不需要向操作系统申请内存，一次 `free` 也不意味着页面马上归还操作系统。

本文以 **jemalloc 5.4.0，源码提交 `7a34f18502e7`** 为准，讨论 Linux 上启用 tcache、使用默认 PAC 页分配后端的常见路径。HPA、自定义 extent hooks、特殊对齐和性能采样会引入其他分支。文中的具体尺寸来自 x86-64、4 KiB 页环境的实际构建，不应当作所有平台的常量。

## 一、先把六个概念放到正确的位置

| 概念 | 管理什么 | 容易混淆的地方 |
| --- | --- | --- |
| tcache | 线程本地、按 size class 缓存的可复用对象指针 | 缓存的不只是 small 对象；不是操作系统页缓存 |
| arena | 一组相对独立的分配、页面管理、回收与统计状态 | 可以被多个线程共享，不是一线程一个，也不是一段固定连续内存 |
| bin | 某个 arena 中、某一 small size class 对应的 slab 管理结构 | 不是一个对象，也不是所有大小请求都要经过的统一桶 |
| slab | 被切成多个同尺寸 region 的页级内存块 | 是一种用途特殊的 extent；不保证只有一页 |
| region | slab 中用于容纳一个小对象的固定尺寸槽位 | `malloc(100)` 对应的槽位可能大于 100 字节 |
| extent | 页对齐、虚拟地址连续的一段内存范围 | 不要求物理连续，也不一定对应一次独立的 `mmap` |

这里有两个维度：**arena 和 bin 是组织、管理结构；extent、slab、region 是被管理的内存及其切分方式。** 不能把它们理解成几个相互独立、依次复制数据的缓存层。

small 对象的关系可以写成：一个 arena 有多种尺寸的 bin，每种 bin 管理多个 slab，每个 slab 切出若干 region。源码还支持对同一尺寸的 bin 做分片，降低锁争用。

另一个常见歧义是“bin”：arena 的 `bin_t` 管理 slab，而 tcache 的 `cache_bin_t` 管理缓存指针。两者都按尺寸分类，但**不是同一个结构，也不在同一个共享范围内**。

## 二、跟踪一次 malloc(100)

### 1. 先确定 size class，而不是直接申请 100 字节

jemalloc 将请求大小映射到预设的 size class。尺寸不是简单地全部向上取到二次幂，例如本文环境中存在 80、96、112、128 字节等类别。

`nallocx(100, 0)` 的实测结果是 112，表示这个请求对应的可用分配尺寸为 112 字节。与请求量相比，多出的 12 字节就是尺寸取整带来的内部碎片。后续选择 tcache 槽、arena bin 和 slab 布局，都围绕这个 size class 进行。

### 2. tcache 命中：直接取出一个对象指针

源码入口 `je_malloc` 首先调用 `imalloc_fastpath`。对已经初始化、没有触发慢路径事件的普通小请求，它找到当前线程的 tcache，再从对应 `cache_bin_t` 弹出一个指针返回。

**这条命中路径不需要获取共享 arena bin 的锁，也不需要调用 `mmap`。** 线程本地存取，加上尺寸索引和少量状态检查，是 jemalloc 高频小对象分配的重要快路径。

tcache 没有对象、线程状态需要处理，或者统计/维护事件到期，都会使执行进入更完整的路径。因此“有 tcache”不代表每次调用都只执行几条无锁指令。

### 3. tcache 未命中：向 arena 的 bin 批量补货

对于仍可缓存的 small 请求，路径会进入 `tcache_alloc_small`，再由 `tcache_alloc_small_hard` 调用 `arena_ptr_array_fill_small`，批量取得同尺寸 region。

arena 的 bin 先检查当前 slab，即 `slabcur`；当前 slab 无法继续供应时，再寻找仍有空位的 slab。空闲槽位通过 bitmap 管理，分配时选中相应 region 并更新状态。

批量填充把一次共享锁操作的成本摊到多个对象上。得到的指针一部分留在 tcache，当前调用取走一个。**tcache 中的空闲对象，是从 arena 取出、尚未交还给 arena 的 region。** 这个区别会直接影响后面的释放和统计。

5.4.0 会依据观察到的需求调整各 bin 的补充和保留目标。旧文章里“每次固定取一半”“满了固定退一半”的说法，不应直接套到这一版。

### 4. bin 也不够：申请新的 slab

只有现有 slab 无法满足需求时，才需要通过 `arena_slab_alloc` 向页分配层要一个新 slab。新 slab 初始化 bitmap 后，才能继续提供 region。

在本文构建中，112 字节类别的布局为：

```text
size class = 112 bytes
slab size  = 28672 bytes = 7 pages
regions    = 256

slab / extent
  [ region 0 ][ region 1 ] ... [ region 255 ]
      112 B       112 B              112 B
```

这说明 slab 不一定是一页。jemalloc 按尺寸类别选择 slab 布局，以兼顾切分利用率、管理成本和批量分配。

## 三、arena 如何通过 extent 管理底层内存？

### small 和 large 在这里汇合

large 分配不经过 small bin，也不把目标内存切成一组 small region。常见非缓存路径是 `large_malloc` → `arena_extent_alloc_large` → `pa_alloc`；small 路径在新建 slab 时，也会到达 `pa_alloc`。

但 **large 不等于绕过 tcache**。本文构建中，最大 small 类别是 14336 字节，默认 `opt.tcache_max` 则是 32768 字节。因此，16 KiB、32 KiB 这类 large 分配仍可能命中 tcache。large 缓存未命中时通常只分配当前需要的一个对象，而不是像 small 那样批量生成多个大对象。

以默认配置下的 1 MiB 请求为例，它超过 tcache 上限，进入 large/arena 路径。整体关系如下，图中省略了初始化、对齐和异常处理：

```text
request -> size class -> tcache hit? -> return pointer
                            |
                       miss / bypass
                            |
                          arena
                +-----------+-----------+
                |                       |
              small                   large
                |                       |
        bin -> existing slab            |
                |                       |
        new slab if necessary           |
                +-----------+-----------+
                            |
                    pa_alloc -> PAC
                            |
                   reuse cached extent
                            |
                   grow / map if needed
```

### extent 是范围，不是固定大小的“超级对象”

默认情况下，PAC 先尝试复用缓存中的 extent，再尝试从保留的地址空间取得合适范围；仍不够时才通过底层 extent hooks 向系统申请。标准 Linux 路径会使用 `mmap` 等虚拟内存机制。

一块较大的 extent 可以在条件允许时拆分；相邻空闲范围也可以在规则和 hooks 允许时合并。所以一个 slab 或 large allocation 所用的 extent，不必对应一次新的系统调用。旧版本资料里的固定 chunk 布局，也不能直接替代这里的 extent 管理方式。

还要区分**虚拟地址映射与物理页驻留**：获得一段地址范围，不意味着每一页已经消耗实际物理内存。匿名页可能在后续访问时才获得物理页支持。

arena 的意义则主要在于把共享管理状态分散开，减少所有线程争抢同一套锁。不过，更多 arena 也意味着更多独立的 slab 和空闲内存状态，未必更省内存。它是在并发扩展性与复用、占用之间做权衡，而不是“越多越好”。

## 四、free 的终点，通常不是操作系统

### 先找归属，再决定留在哪一层

普通 `free(ptr)` 没有尺寸参数。jemalloc 可以通过 `emap`/`rtree` 一类地址索引，得到该分配的尺寸类别、是否属于 slab，以及相应的 extent 元数据 `edata_t`。不需要假设每个小对象前面都紧贴一个完整管理头。

若允许缓存，指针可能先进入**执行释放操作的线程**的 tcache。跨线程释放时，它不必马上返回最初分配它的线程。

等到批量 flush，源码根据每个对象的原始 arena 和 bin shard 归组，再在相应锁下归还 region。当前线程关联哪个 arena，不能改变已有对象的实际归属。

对于普通 small 对象，几个阶段必须分开：

1. **应用调用 free。** 应用已经失去对象所有权；指针不可再使用，但对象空间可能只是进入 tcache。
2. **tcache 将 region 交回 bin。** bitmap 中的槽位才重新对 arena 可用。一次 flush 不保证整块 slab 变空。
3. **slab 全空。** 当所有 region 都已归还，整块 slab 才能退出 small 分配用途，交回页级管理。只剩少数存活对象，也可能使整块 slab 继续保持 active。
4. **底层复用或回收。** 空闲 extent 可以再次服务分配，也可能经过 decay/purge 处理。此时才涉及页级回收与地址映射保留策略。

large 分配没有第二步中的 small bitmap 管理，但它也可能先被 tcache 保留，并且交回页层后仍不保证立即解除映射。

### dirty、muzzy、retained 分别意味着什么？

- **dirty**：已经不再承载活跃分配，但尚未完成相应清理的空闲页；可以直接复用，也会消耗内存资源。
- **muzzy**：已进行惰性清理、由操作系统决定何时回收物理页的范围，例如支持 `MADV_FREE` 的路径。它不意味着 RSS 必须立即下降。
- **retained**：jemalloc 保留下来的虚拟地址映射，通常已被清理、解除物理提交，或尚未触碰；不能把它的字节数直接当作额外 RSS。

这三者不是每块内存都必须经过的固定流水线。操作系统支持、decay 配置、保留策略和 hooks 会改变分支；清理物理页与 `munmap` 解除虚拟映射也不是同一个动作。

`dirty_decay_ms`、`muzzy_decay_ms` 控制近似的回收节奏，而不是给每块对象设置精确到期闹钟。在支持的配置下，后台线程可以异步执行页面清理，但**不会替所有空闲线程清空各自的 tcache**。自动 tcache 的增量回收受分配/释放活动驱动；线程长时间停止活动时，缓存可能持续保留。

## 五、用一个小实验把这些层次分开

文末提供完整程序 `observe_tcache.c`。它创建一个独立 arena 和显式 tcache，只分配一个 100 字节对象，然后依次执行释放、tcache flush、arena purge。

为避免观察过程被自动回收打断，程序把这个测试 arena 的两项 decay 都设为 `-1`，运行时关闭后台线程与 HPA。**这是实验隔离条件，不是生产调优建议。** 显式 tcache 仅由这个单线程程序使用。

将源码与解压后的 jemalloc 5.4.0 目录放在同一目录，可在 Linux 上构建运行：

```bash
cd jemalloc-5.4.0
./configure --disable-shared
make -j4
cd ..
cc -std=c11 -O2 -Ijemalloc-5.4.0/include \
  observe_tcache.c jemalloc-5.4.0/lib/libjemalloc.a \
  -pthread -lm -ldl -o observe_tcache
MALLOC_CONF='background_thread:false,hpa:false' ./observe_tcache
```

在 GCC 13.3.0、x86-64、4 KiB 页环境下，实测关键输出如下，省略完整版本字符串：

```text
page=4096 small_max=14336 tcache_max=32768
request=100 usable=112 bin=7 nregs=256 slab_size=28672
allocated  curregs=100 pactive=7 pdirty=0
freed      curregs=100 pactive=7 pdirty=0
flushed    curregs=0 pactive=0 pdirty=7
purged     curregs=0 pactive=0 pdirty=0
```

`curregs` 是该 arena、该 bin 的 region 计数，`pactive` 和 `pdirty` 的单位都是页。这里只分配了一个应用对象，为什么第一行有 100 个 region？因为首次 refill 批量拿出了 100 个：一个给应用，其余留在 tcache。**100 是这次初始状态下的观测值，不是所有 refill 的固定批量。**

调用释放函数后，应用对象已经没有了，但 region 回到 tcache，而没有回到 bin，所以前两个阶段的计数相同。程序每次读统计前都更新 `epoch`；计数不变不是单纯忘记刷新快照，**刷新统计也不等于清空 tcache**。

flush 之后全部 region 归还，7 页 slab 变空，进入 dirty 集合；显式 purge 后，dirty 页数才归零。最后一行不代表整个进程 RSS 为零，也不证明地址映射已经解除：其他 arena、分配器元数据以及映射保留仍然存在。

完整程序通过严格编译检查；另外运行了该版本中尺寸类别、tcache 上限、自适应缓存目标、tcache GC、bin 分片和 PAC decay 对应的六组上游测试，全部通过。这里验证的是机制与计数关系，不是吞吐或内存占用基准。

## 六、观察内存占用时，应该看哪些数字？

只看 RSS，无法判断内存留在链路的哪一层。几个常用统计各有口径：

| 统计项 | 主要含义 | 不能直接推出什么 |
| --- | --- | --- |
| `stats.allocated` | 分配给应用的字节统计；需注意缓存和批量记账 | 不保证是任意瞬间、逐对象同步的存活字节计数 |
| `stats.active` | 承载分配的 active 页总量，按页计 | 与 allocated 的差值不能全部归因于某一种碎片 |
| `stats.resident` | 分配器对相关物理驻留页的上界性统计，包含元数据等 | 不是操作系统测得的整个进程精确 RSS |
| `stats.retained` | 保留而未解除的虚拟地址映射量 | 不等于这些范围都占着物理内存 |

尺寸取整产生内部碎片，未满 slab 产生页内空洞，tcache 延迟把 region 还给 bin，多 arena 又会分散可复用容量。jemalloc 通常不能移动仍被应用引用的对象来压实 slab，因此“释放了大部分对象”未必释放“大部分 active 页”。

排查时，应把持续增长的存活分配、active 页中的空洞、tcache 保留、dirty/muzzy 页和 retained 地址空间分开。`free` 后 RSS 不降不能直接证明泄漏；反过来，存在缓存机制也不能用来排除真正的泄漏。

回到最初的问题：**arena 分散共享管理，bin 组织同尺寸 small 对象的 slab，extent 承载页级内存范围；tcache 则尽量让频繁的对象分配和释放留在线程本地。** 把对象所有权、槽位可复用、页面可回收和虚拟映射解除分开，jemalloc 的分配与回收链路就清楚了。

## 参考资料

- [jemalloc 5.4.0 发布说明：本版本重构与自适应 tcache 策略变更](https://github.com/jemalloc/jemalloc/releases/tag/5.4.0)
- [5.4.0 版本手册源文件：size class、tcache、decay、extent hooks 与统计口径](https://github.com/jemalloc/jemalloc/blob/5.4.0/doc/jemalloc.xml.in)
- [5.4.0 头文件目录：在 internal 子目录阅读 jemalloc_internal_inlines_c.h、malloc_dispatch_inlines.h 与 bin.h](https://github.com/jemalloc/jemalloc/tree/5.4.0/include/jemalloc)
- [5.4.0 arena 实现：slab 创建、批量填充及按原始 arena 归还](https://github.com/jemalloc/jemalloc/blob/5.4.0/src/arena.c)
- [5.4.0 tcache 实现：自适应补充、保留目标与缓存回收](https://github.com/jemalloc/jemalloc/blob/5.4.0/src/tcache.c)
- [5.4.0 large 分配实现：large_malloc、large_palloc 与 extent](https://github.com/jemalloc/jemalloc/blob/5.4.0/src/large.c)
- [5.4.0 页分配路由：pa_alloc 对 PAC/HPA 的选择](https://github.com/jemalloc/jemalloc/blob/5.4.0/src/pa.c)
- [5.4.0 PAC 实现：空闲 extent 的复用、decay 与 purge](https://github.com/jemalloc/jemalloc/blob/5.4.0/src/pac.c)
- [本文完整实验程序：observe_tcache.c](https://yangyang233333.github.io/code/jemalloc-allocation-chain/observe_tcache.c)
