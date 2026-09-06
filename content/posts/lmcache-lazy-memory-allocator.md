---
title: "LMCache LazyMemoryAllocator 源码分析：延迟锁页、分配释放与内存碎片"
date: 2026-09-06T18:11:00+08:00
draft: false
tags: ["LMCache", "内存管理", "Pinned Memory", "大模型推理"]
categories: ["大模型推理"]
summary: "从源码拆解 LMCache LazyMemoryAllocator 如何预留连续 CPU 缓冲区、后台分批注册 pinned memory、通过地址管理器完成 alloc/free，并分析其内部碎片、外部碎片与暂时性容量不足问题。"
---

`LazyMemoryAllocator` 是 LMCache 的 CPU pinned-memory 分配器。它解决的核心问题不是“如何凭空扩展一块 Tensor”，而是：**如何在保持底层地址稳定的前提下，避免启动时同步注册全部 pinned memory。**

它采用“先预留最终大小的连续 CPU buffer，再逐步 pin 并开放可分配区间”的设计。实际的空闲区管理由 `TensorMemoryAllocator` 和 `AddressManager` 完成。因此，理解它需要区分三层：物理 buffer、pinned 范围和逻辑可分配地址空间。

## 一、整体结构

```text
LazyMemoryAllocator
├── _buffer: 最终大小的连续 CPU uint8 Tensor
├── _pin_record: 已成功 pin 的物理区间
├── TensorMemoryAllocator
│   └── AddressManager: 管理当前可分配的偏移区间
└── 后台扩容线程
    ├── 每次 pin 64 MiB
    └── 每累计 1 GiB，通过 sbrk 开放给 AddressManager
```

它实现 `MemoryAllocatorInterface`，对外提供：

```text
allocate / batched_allocate
free / batched_free
close / memcheck
```

上层 `L1MemoryManager` 不需要了解后台扩容过程，只把它当作普通 allocator 使用。

## 二、“Lazy”到底延迟了什么

类中有三个关键常量：

```python
PIN_CHUNK_SIZE = 1 << 26  # 64 MiB
COMMIT_SIZE = 1 << 30     # 1 GiB
LOG_INTERVAL = 10 << 30   # 10 GiB
```

初始化时，`init_size` 和 `final_size` 都向上对齐到 64 MiB：

```python
self._curr_size = align_to(init_size, self.PIN_CHUNK_SIZE)
self._final_size = align_to(final_size, self.PIN_CHUNK_SIZE)
```

随后分配一个覆盖 `final_size` 的底层 buffer。普通路径使用 CPU `torch.uint8` Tensor；NUMA 路径则通过设备层 API 在指定 NUMA 节点分配原始内存，再包装成 Tensor。

这里容易产生误解：`LazyMemoryAllocator` 并没有等到需要时才创建整个虚拟 buffer。完整 buffer 在构造阶段就已经建立。被延迟的是：

1. 将 host memory 注册为 pinned memory；
2. 将对应地址范围加入 `AddressManager`，使其可以被业务分配。

初始化完成后的状态类似：

```text
底层 buffer（final_size）
┌──────────────────────────────────────────────────┐
│ 已 pin、已提交、可分配 │ 未 pin、尚不可分配      │
└──────────────────────────────────────────────────┘
          init_size                 final_size
```

这样可以获得稳定的基地址，满足 RDMA、O_DIRECT 等场景对地址和对齐的要求，同时把大规模 pin 操作移出启动关键路径。

## 三、为什么要手动对齐 buffer

普通 `torch.empty()` 不能保证满足任意较大的 `align_bytes`。实现会多申请 `align_bytes - 1` 字节，然后从中切出一个对齐视图：

```python
backing = torch.empty(
    final_size + align_bytes - 1,
    dtype=torch.uint8,
    device="cpu",
    pin_memory=False,
)
offset = (-backing.data_ptr()) % align_bytes
self._buffer = backing[offset : offset + final_size]
```

这里的切片仍共享 `backing` 的 storage，因此不发生数据复制。最终还会检查：

```python
self._buffer.data_ptr() % align_bytes == 0
```

这不只是性能优化。若底层地址不满足 O_DIRECT 或设备注册要求，错误可能到真正执行 I/O 时才以 `EINVAL` 等形式出现。构造阶段检查可以更早暴露问题。

## 四、初始内存如何开放

获得完整 buffer 后，分配器先 pin `[0, init_size)`：

```python
self._pin_memory_chunk(0, self._curr_size)
```

然后构造内部 `TensorMemoryAllocator`：

```python
self._allocator = TensorMemoryAllocator(
    tensor=self._buffer,
    align_bytes=align_bytes,
    init_address_space=self._curr_size,
)
```

关键参数是 `init_address_space`。`TensorMemoryAllocator` 虽然持有完整 buffer，但其 `AddressManager` 最初只管理 `init_size` 范围。

因此系统中同时存在三个容量概念：

| 容量 | 含义 |
| --- | --- |
| `final_size` | 底层 buffer 的最终大小 |
| `_curr_size` | 当前已经尝试 pin 的大小 |
| `AddressManager.get_heap_size()` | 当前已提交、允许业务分配的大小 |

正常运行时，后两个值在每轮提交后保持一致；扩容轮次内部，`_curr_size` 可以暂时领先于地址管理器。

## 五、后台扩容过程

后台 daemon 线程执行 `_expand_worker()`。每轮最多处理 1 GiB：

```text
循环 16 次：
    pin 64 MiB
    _curr_size += 64 MiB

一轮完成：
    AddressManager.sbrk(1 GiB)
```

`_pin_memory_chunk()` 根据 buffer 基地址和偏移计算指针：

```python
ptr = self._buffer.data_ptr() + offset
current_device_spec.pin_memory(ptr, size, 2)
```

成功的区间记录在 `_pin_record` 中，以便 `close()` 时逐段 unpin。

pin 完一轮后，`_commit_expansion()` 调用：

```python
self._address_manager.sbrk(expand_size)
```

`sbrk()` 在这里不是操作系统调用，而是 `AddressManager` 的逻辑扩容接口。它把新的尾部空间加入空闲区集合。只有这一步之后，新空间才能被 `allocate()` 使用。

这种批量提交减少了地址管理器更新次数，但也带来一个短暂窗口：一些内存已经完成 pin，却尚未加入可分配空间。

## 六、allocate 如何实现

`LazyMemoryAllocator.allocate()` 自己不执行空闲块搜索，而是委托给 `TensorMemoryAllocator`：

```python
obj = self._allocator.allocate(shapes, dtypes, fmt, allocator_type)
```

完整路径是：

```text
LazyMemoryAllocator.allocate
    ↓
TensorMemoryAllocator.allocate
    ↓
根据 shape 和 dtype 计算逻辑字节数
    ↓
AddressManager.allocate
    ↓
从空闲区中找到可容纳的连续地址范围
    ↓
在 _buffer 上创建对应 Tensor view
    ↓
返回 MemoryObj
```

`AddressManager` 返回的是相对 buffer 起点的地址偏移及对齐后的物理长度。`TensorMemoryAllocator` 再根据这个区间建立 Tensor view。多个 `MemoryObj` 因此共享同一个底层大 buffer，但对应不同、不重叠的切片。

### 分配失败的含义

当 `allocate()` 返回 `None` 时，不一定表示 `final_size` 已经耗尽，还可能表示：

- 后台线程尚未将后续空间提交给 `AddressManager`；
- 空闲总量足够，但没有足够大的连续区间；
- 对齐后的实际需求大于剩余连续空间。

这是 lazy 扩容的重要语义：它不会因为当前空间不足而等待后台线程，也不会主动触发同步扩容后重试。

### 批量分配

`batched_allocate()` 同样直接委托给 `TensorMemoryAllocator`。底层实现会为相同布局的多个对象申请空间；若无法满足整批请求，则按其失败语义返回 `None`，避免上层拿到难以管理的半批结果。

## 七、free 如何实现

释放也由内部 `TensorMemoryAllocator` 完成：

```python
self._allocator.free(memory_obj)
self._allocator.batched_free(memory_objs)
```

单个对象释放的主要步骤是：

1. 检查 `MemoryObj` 是否有效；
2. 将对象标记为 invalid，避免重复使用；
3. 把对象的 `address` 和 `phy_size` 归还给 `AddressManager`；
4. 更新活动对象数和已分配字节统计。

这里归还的是**对齐后的物理大小 `phy_size`**，而不是 tensor 的逻辑数据大小，否则空闲区边界会被破坏。

### 批量释放的优化

`batched_free()` 会先按地址排序，再把物理上相邻的对象合并：

```text
对象 A：[0, 4 KiB)
对象 B：[4 KiB, 8 KiB)
对象 C：[16 KiB, 20 KiB)

合并后：
[0, 8 KiB)
[16 KiB, 20 KiB)
```

随后只需向 `AddressManager.free()` 提交两个区间，而不是三个。这能减少锁操作和空闲链表维护开销。

需要注意，`free()` 只回收逻辑地址区间：

- 不会缩小底层 buffer；
- 不会降低 `_curr_size`；
- 不会立即 unpin 对应页面；
- 后台扩容不会反向收缩。

因此它是一个生命周期内单向扩容、重复利用空闲块的内存池。

## 八、AddressManager 如何管理空闲区

`AddressManager` 使用按起始地址排序的 `SortedList[FreeBlock]` 保存空闲区。初始化时只有一个区间：

```text
[0, init_address_space)
```

分配时先把请求大小向上对齐，再查找可容纳它的空闲块。从找到的区间头部分配，剩余部分继续留在空闲集合中。

释放时，根据地址找到前驱和后继空闲块，并尝试合并：

```text
前驱空闲 + 当前释放块 + 后继空闲
             ↓
         一个大空闲块
```

可以合并的条件是物理地址连续。`memcheck()` 还会检查是否存在本应合并却没有合并的相邻空闲块。

## 九、是否存在内存碎片

答案是：**存在，但要区分内部碎片和外部碎片。**

### 1. 内部碎片

所有申请都会按 `align_bytes` 向上取整。默认对齐通常是 4 KiB。

假设请求 5000 字节：

```text
逻辑大小：5000 B
物理大小：8192 B
内部浪费：3192 B
```

单个对象的内部碎片上限接近：

```text
align_bytes - 1
```

如果 KV chunk 很大，对齐浪费占比很低；如果保存大量小对象，浪费比例会明显增加。

此外，`init_size` 和 `final_size` 会先向上对齐到 64 MiB，这可能让实际预留或 pin 的容量略大于配置值，但这属于容量粒度带来的尾部开销，不是空闲链表碎片。

### 2. 外部碎片

即使总空闲空间足够，空间也可能被分散成多个小洞，导致大块申请失败：

```text
[已用 4 MiB][空闲 4 MiB][已用 4 MiB][空闲 4 MiB]
```

此时总空闲量是 8 MiB，但无法分配一个连续的 8 MiB 对象。

`AddressManager` 会在释放时合并相邻区间，因此：

- 相邻空闲块不会长期保持分裂；
- 非相邻空洞无法压缩；
- 已分配对象不会移动；
- 没有 compaction/relocation 机制。

所以外部碎片无法完全避免。

### 3. 实际场景中的严重程度

LMCache 的典型 KV chunk 通常具有固定布局，同一模型的大多数对象大小相同。固定大小分配天然比任意大小分配更不容易产生严重外部碎片：释放一个 chunk 后，后续同规格 chunk 可以直接复用。

但以下场景会增加碎片风险：

- 同一 L1 池存放多个模型；
- 不同 object group 的 shape 或 dtype 不同；
- KV、hidden state、压缩对象混用；
- 动态配置产生多种 chunk 大小；
- 大量大小不同的对象交错分配和释放。

因此它不是无碎片分配器，只是在“同模型、固定 chunk”这一常见负载下碎片通常较可控。

## 十、lazy 扩容带来的特殊问题

### 暂时性 OOM

后台尚未扩容完成时，业务只能使用已提交空间。此时可能出现：

```text
final_size 尚有大量容量
但当前 AddressManager 空间不足
allocate 返回 None
```

这不是碎片，而是可用容量尚未开放。若启动后立即施加高并发负载，`init_size` 太小会放大这个问题。

### pin 失败后的语义

`_pin_memory_chunk()` pin 失败时只记录 warning，扩容线程仍会增加 `_curr_size`，之后也会通过 `sbrk()` 开放该区间。

这意味着逻辑可分配不严格等同于“已成功 pin”。功能上仍可能正确，但 GPU DMA 性能可能下降。监控时不应只看 heap size，还应关注 pin 失败日志。

### 关闭过程不能收缩

只有 `close()` 才停止线程并逐段 unpin。运行期间没有 shrink，所以配置过大的 `final_size` 会一直保留对应虚拟 buffer，并最终尝试 pin 全部空间。

## 十一、线程安全

`LazyMemoryAllocator` 没有在自己的 `allocate()` 和 `free()` 外层增加统一锁，而是依赖内部组件：

- `AddressManager.allocate/free/sbrk` 自己加锁；
- `TensorMemoryAllocator` 维护对象和统计；
- 后台线程只通过 `AddressManager.sbrk()` 增加尾部空间。

因此业务线程分配与后台扩容可以并发执行。核心并发边界在地址管理器，而不是整个 lazy allocator。

不过 `_curr_size` 被约定为只由扩容线程访问。这是一种基于所有权约束的设计，而不是通过锁保护所有字段。

## 十二、与 MixedMemoryAllocator 的区别

| 维度 | LazyMemoryAllocator | MixedMemoryAllocator |
| --- | --- | --- |
| 启动时开放容量 | `init_size` | 全部容量 |
| pin 方式 | 后台按 64 MiB 注册 | 初始化阶段准备 |
| 地址空间增长 | `AddressManager.sbrk()` | 固定 |
| 启动延迟 | 较低 | 大内存池时较高 |
| 运行初期 OOM | 可能暂时发生 | 不因后台扩容发生 |
| 共享内存名称 | 当前不走该路径 | 支持 `shm_name` |
| 碎片模型 | 对齐碎片 + 空闲区碎片 | 基本相同 |

两者的主要区别是 pinned memory 的建立时机，而不是完全不同的分配算法。最终实际切块都依赖地址空间管理器。

## 十三、使用与调优建议

### `init_size` 不宜过小

它至少应覆盖服务启动后最早一批并发请求，否则可能在后台扩容完成前出现暂时性 OOM。

可以粗略估算：

```text
init_size >= 单个请求预计写入 L1 的字节数 × 启动阶段并发数
```

还应为对齐和并发波动预留余量。

### 观察扩容速度

扩容线程每次 pin 64 MiB，每 1 GiB 才提交一次。如果系统 pin memory 很慢，业务可用容量会阶梯式增长，而不是连续增长。

### 多尺寸对象需要关注最大连续空闲块

当前公开统计主要是已用和总量。判断外部碎片时，仅看总空闲量不够，还需要观察：

```text
最大连续空闲块 / 总空闲空间
```

若总空闲量很大但大对象频繁分配失败，通常意味着外部碎片或扩容尚未提交。

### 关注 pin 失败日志

pin 失败不会立即让 allocator 停止工作，因此可能表现为“功能正常但传输变慢”。生产环境最好为 pin 失败增加明确指标或告警。

## 总结

`LazyMemoryAllocator` 的设计可以概括为：

```text
一次性预留稳定的最终 buffer
        ↓
启动时只 pin 并开放初始区间
        ↓
后台以 64 MiB 为单位 pin
        ↓
以 1 GiB 为单位提交给 AddressManager
        ↓
TensorMemoryAllocator 在已提交区间内 alloc/free
```

它通过延迟 pinned-memory 注册降低大容量 L1 的启动阻塞，同时保持整个内存池地址稳定。分配和释放的核心仍是基于有序空闲区的连续区间管理：申请时按对齐大小切割空闲块，释放时合并相邻块。

该实现存在对齐造成的内部碎片，也可能出现无法通过相邻合并消除的外部碎片；不过在固定大小 KV chunk 为主的场景中，外部碎片通常较轻。相比碎片，实际部署中更值得关注的是启动阶段的暂时性 OOM，以及 pin 失败后内存仍被开放所造成的性能退化。
