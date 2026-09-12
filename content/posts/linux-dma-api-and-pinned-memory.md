---
title: "读懂 Linux DMA API：地址映射、内存一致性，以及 Pin 为什么能加速传输"
date: 2026-09-12T17:32:00+08:00
draft: false
tags: ["Linux", "DMA", "IOMMU", "CUDA", "Pinned Memory"]
categories: ["技术"]
description: "从 Linux DMA API 的地址模型和所有权规则出发，解释页锁定、DMA 映射、异步传输与零拷贝之间的区别。"
---

CPU 内存传到 GPU，为什么先做一次 pin 往往更快？不是因为 pin 提高了 PCIe 的物理带宽，而是因为它让内存能够更直接地参与设备传输，减少中间复制，并为传输与计算重叠创造条件。

要理解这件事，可以先读 Linux 的 [Dynamic DMA mapping Guide](https://docs.kernel.org/core-api/dma-api-howto.html)。它面向驱动开发者，讲的是如何让设备正确访问内存；CUDA 的 pinned memory 则是应用层的一种内存注册机制。两者不是同一套 API，但背后需要解决的地址、生命周期和可见性问题是相通的。

本文以传统 Linux 主机加独立 GPU 为主要模型。支持共享页表、设备缺页等能力的平台，会在文末单独说明边界。

## 一、DMA 绕开的是 CPU 搬运，不是 CPU 管理

DMA，即 Direct Memory Access，允许设备的数据传输引擎读写内存，不必让 CPU 用加载、存储指令逐字节搬运整个缓冲区。

但 CPU 和驱动仍要准备缓冲区、建立映射、提交描述符，以及处理完成通知。DMA 不是“CPU 完全不参与”，更不是“有个指针就能发给设备”。

原文首先区分了三种地址，这是理解后续 API 的基础：

| 地址 | 使用者 | 常见表示 |
| --- | --- | --- |
| CPU 虚拟地址 | CPU 上运行的代码 | `void *` |
| CPU 物理地址 | CPU 物理地址空间、内存管理系统 | `phys_addr_t` |
| DMA 地址 | 设备发起内存访问时使用 | `dma_addr_t` |

它们可能有相同的数值，但不能因此当成同一种地址。

```text
CPU 路径： CPU 虚拟地址 X ──CPU 页表──→ 物理内存 Y

设备路径： DMA 地址 Z     ──IOMMU 等映射──→ 物理内存 Y
```

在有 IOMMU 的系统中，Z 通常是设备侧的 I/O 虚拟地址，即 IOVA；其他平台可能采用直接映射或主桥地址转换。驱动应使用 DMA API 返回的地址，而不是假设 `virt_to_phys()` 的结果就能写进设备描述符。

原文还讨论了 MMIO：CPU 通过 `ioremap()` 等接口访问设备寄存器，而设备通过 DMA 访问 RAM。这是两个方向不同的问题，不能把映射设备寄存器和映射数据缓冲区混为一谈。

## 二、DMA 映射是设备相关的访问契约

一个典型的 streaming DMA 映射调用是：

```c
dma_addr = dma_map_single(dev, buffer, length, DMA_TO_DEVICE);
if (dma_mapping_error(dev, dma_addr))
    return -EIO;
```

这只是局部示意：假设 `buffer` 已经来自合适的内核分配接口，相关变量已声明，调用方另行负责失败清理与后续设备操作。

这里的 `dev` 很重要。同一块 RAM 对不同设备可能需要不同映射，因此 DMA 地址不是一个全系统通用、永远有效的指针。

映射过程中，平台可能需要建立 IOMMU 映射、处理缓存一致性，或在设备无法直接寻址目标内存时使用 bounce buffer。并非每次调用都会完成全部动作；DMA API 的价值恰恰是把这些平台差异藏在接口后面。

### 先声明设备能访问哪些地址

驱动初始化时，需要根据硬件能力配置 DMA mask。例如设备及其相关引擎确实支持 64 位 DMA 地址时，可以使用：

```c
if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64)))
    return -ENODEV;
```

这不是“让设备变成 64 位”，而是向内核声明设备能够接收的 DMA 地址范围，并检查平台是否支持。某些设备的描述符和数据引擎能力不同，还需要分别配置 streaming 与 coherent mask。

### 不是任意虚拟地址都能交给 `dma_map_single`

内核文档强调缓冲区来源。适当的 `kmalloc()` 内存可以用于单段映射，但不能把任意用户指针、栈地址，或者仅虚拟连续的 `vmalloc()` 区域直接交给它。

用户内存要先通过适当的页管理机制取得并保持页面有效；不连续页面可以组织成 scatterlist 再映射。这里也已经能看出：**锁住页面和得到 DMA 地址是两项不同的工作。**

## 三、Coherent 与 Streaming：关键是可见性和所有权

文档把 DMA 映射分成两大类。它们不是“同步 API”和“异步 API”的简单对应关系，也不是 CUDA stream 的分类。

### Coherent：适合长期共享的控制结构

`dma_alloc_coherent()` 同时返回 CPU 可访问的地址和设备使用的 DMA handle，常用于描述符环等控制结构。

这里的 coherent 保证 CPU 与设备之间的内存可见性，不需要像 streaming 映射那样在每次所有权切换时显式调用缓存同步接口。但它不保证所有访问都按程序书写顺序到达设备。

例如“先填写描述符内容，再设置有效标志”，仍需要符合平台与设备协议的内存屏障和寄存器访问顺序。**缓存一致不等于访问有序，也不等于不存在并发竞争。** 原文专门用写屏障示例强调了这一点。

### Streaming：适合传输数据缓冲区

网络数据包、磁盘 I/O 缓冲区等常用 streaming 映射。其基本生命周期是：

```text
CPU 准备缓冲区
    ↓
dma_map_single / dma_map_sg
    ↓
把 DMA 地址提交给设备
    ↓
设备访问缓冲区
    ↓
确认设备操作完成
    ↓
dma_unmap_single / dma_unmap_sg
    ↓
CPU 重新使用或释放缓冲区
```

调用 `dma_map_*()` 不会启动传输；调用 `dma_unmap_*()` 也不是等待设备完成的替代品。必须先通过设备规定的完成机制确认 DMA 已结束。

方向参数以主存与设备之间的数据流向为准：

| 方向 | 含义 | 示例 |
| --- | --- | --- |
| `DMA_TO_DEVICE` | 设备读取主存内容 | 主机向设备发送数据 |
| `DMA_FROM_DEVICE` | 设备向主存写入内容 | 设备把接收结果写回主机 |
| `DMA_BIDIRECTIONAL` | 同一映射允许两个方向 | 设备协议确实需要双向访问 |

方向影响缓存维护和平台优化，不能为了省事一律使用双向。

如果要长期保留 streaming 映射并重复使用缓冲区，则在 CPU 重新访问前使用 `dma_sync_*_for_cpu()`，准备再次交给设备前使用 `dma_sync_*_for_device()`，具体按方向和访问方式遵循 API 契约。

这些同步函数用于可见性与所有权切换，不代替设备完成通知。即使某个平台把部分同步操作实现成空操作，驱动也不应绕过跨平台规则。

## 四、物理不连续不妨碍 DMA

一段连续的虚拟内存可能对应分散的物理页。DMA 不要求所有应用缓冲区都先被整理成一块连续物理内存；驱动可以用 scatter-gather 描述分散区域，并交给 `dma_map_sg()`。

```text
内存区域： [区域 A]   [区域 B]   [区域 C]
               \        |        /
                scatterlist
                     ↓ dma_map_sg
             设备可使用的 DMA 段列表
```

映射层可能合并相邻条目，所以返回的 DMA 段数可能小于原始 scatterlist 条目数。原文有一个很容易写错的规则：

- 编程设备时，使用映射返回的段数，以及 `sg_dma_address()`、`sg_dma_len()`。
- 调用 `dma_unmap_sg()` 时，使用最初传入的条目数，而不是映射返回的段数。
- `dma_map_sg()` 返回 `0` 表示映射失败，不能继续提交设备访问。

IOMMU 映射空间、bounce buffer 等都是有限资源，所以 map 必须与 unmap 配对。DMA 地址不是拿到以后就可以永久保存、跨生命周期复用的地址。

## 五、Pin 到底解决哪一层问题？

现在把场景从内核驱动换成用户态 CUDA：

```text
应用已有 CPU buffer
    ↓
cudaHostRegister(ptr, size, flags)
    ↓
注册该区域，以便 CUDA 访问和传输
```

`ptr` 仍是进程的 CPU 虚拟地址。CUDA runtime 和驱动负责处理注册，不要求应用自己调用内核的 `dma_map_sg()`。

在传统页锁定路径中，可以把过程理解为：确保相关页面存在并保持有效，阻止它们在设备使用期间被换出或以不兼容的方式回收、迁移，再建立或登记设备访问需要的资源。

Linux 的 [pin_user_pages 文档](https://docs.kernel.org/core-api/pin_user_pages.html) 解释了 DMA-pinned pages，以及 `FOLL_PIN`、长期 pin 等机制。它提供了操作系统层面的理解框架，但不能据此断言所有 NVIDIA 驱动版本内部都执行完全一样的函数序列。

三个概念必须分开：

| 操作 | 解决的问题 | 不代表什么 |
| --- | --- | --- |
| Pin 页面 | 让设备使用期间的页面保持有效 | 不会自动启动传输 |
| 建立 DMA 映射 | 让指定设备获得可用访问地址及相关一致性保证 | 不等于分配显存 |
| 提交拷贝 | 真正把数据从源送到目的地 | 不一定与计算重叠 |

因此，`mlock()` 不能替代 CUDA 注册：限制换页并不等于完成设备侧注册。同样，缓存对象上一个名叫 `pin()` 的引用计数操作，也可能只表示“不要淘汰这个对象”，与操作系统锁页毫无关系。

## 六、为什么 Pinned Memory 往往更快？

### 1. 减少 pageable buffer 的中间复制

普通 pageable 内存也能用于 CUDA 数据传输。不能说“不 pin 就完全不能 DMA”：驱动可以使用自己的 pinned staging buffer，在应用内存和真正参与设备传输的区域之间中转。

以传统 pageable H2D 路径为例：

```text
应用 pageable CPU buffer
    ↓ CPU 复制
驱动 pinned staging buffer
    ↓ DMA，经 PCIe 等互连
GPU 显存
```

如果应用缓冲区已经注册，则可以避免这类中转：

```text
应用 pinned CPU buffer
    ↓ DMA，经 PCIe 等互连
GPU 显存
```

减少一次中间复制，不只减少 CPU 指令，还减少了 CPU 内存读写、缓存污染和 staging 管理开销。具体 pageable 路径是否中转、在哪一步同步，要以 [CUDA API 同步语义](https://docs.nvidia.com/cuda/cuda-runtime-api/api-sync-behavior.html) 和平台实现为准，不能把示意图视为所有方向、所有设备的固定实现。

还有一个容易忽略的前提：**最好让数据直接产生在可复用的 pinned buffer 中。** 如果每次先在普通内存里生成数据，再自己复制到 pinned buffer，中间复制仍然存在，只是从驱动移到了应用。

### 2. 为异步传输与计算重叠创造条件

页锁定 host 内存是传统 CUDA host-device 异步传输的重要条件。但 pin 不是异步开关；要实际重叠，还需要硬件并发能力、合适的 stream，以及不存在阻止并发的数据依赖。

不能让“计算 chunk A”与“尚未完成的 chunk A 上传”无序并发，但可以在计算 A 时上传 B：

```text
时间 →
传输： [上传 A][上传 B][上传 C]
计算：         [计算 A][计算 B][计算 C]
```

若暂时忽略启动和流水线收尾成本，并假设不同阶段能独立并发，每个 chunk 的稳态周期可以从接近 `Tcopy + Tcompute`，降到接近 `max(Tcopy, Tcompute)`。这是理想化模型，不是性能保证。

CUDA 的 [Pinned Memory 与异步传输最佳实践](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html#pinned-memory) 同时强调了页锁定内存、stream 和硬件能力。仅给拷贝 API 加上 `Async` 后缀，或者仅把指针注册一次，都不足以证明已经实现重叠。

### 3. 把注册成本摊薄到多次使用

Pin 本身是重操作，可能涉及页面准备和驱动资源管理。如果每次传输都 register、copy、unregister，注册成本可能抵消复制收益。

更常见的设计是：

```text
建立 pinned 内存池
    → 分配子块
    → 多次传输与复用
    → 子块归还池中，但保持注册
    → 池关闭且设备操作完成后统一解除注册
```

这也是大型 KV cache 缓冲池采用预注册或后台分批注册的原因：既复用注册资源，又避免启动时必须等待整个大池全部注册完。只有成功注册并满足传输要求的区域，才能按 pinned 路径使用。

## 七、Mapped Memory 不等于“免费访问”

`cudaHostRegisterMapped` 请求把 host 内存映射到 CUDA 设备地址空间。在平台支持并按要求取得设备可用指针后，GPU kernel 可以直接访问这段 CPU RAM。

这与“先复制到显存再计算”是两条不同路径：

```text
显式复制： CPU RAM ──拷贝──→ GPU 显存 ←──kernel 访问

映射访问： CPU RAM ←──CPU–GPU 互连──kernel 访问
```

后一种常称为 zero-copy，因为没有显式建立一份显存副本。但物理数据仍然要经过互连，延迟和带宽成本不会消失。对于独立 GPU 上会反复读取的数据，先搬进显存可能更划算。

也不能因为 CPU 与 GPU 支持统一虚拟寻址，就假设所有 host 指针在所有平台上都能直接交给 kernel。应按 [CUDA host registration API](https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__MEMORY.html) 的映射能力和指针规则处理。

## 八、优化之前，先保住正确性

Pinned memory 不是越多越好。它会限制操作系统管理内存的灵活性，并消耗驱动注册资源；过量使用可能降低整机性能。NUMA 放置、PCIe 拓扑、传输块大小和并发度，也可能成为比 pageable staging 更主要的瓶颈。

异步传输还带来明确的生命周期要求：

- H2D 完成之前，不要修改或复用源缓冲区。
- D2H 完成之前，不要把目标缓冲区当作完整结果读取。
- 所有相关设备访问完成之前，不要注销注册或释放底层内存。
- 不要把 streaming DMA 的缓存同步、CUDA stream 同步、设备完成通知当成同一件事。

测量性能时，应分开记录一次性注册成本与稳态传输成本，确保计时覆盖真正的完成事件，并确认数据来源是否仍包含一次应用侧复制。否则很容易测到“API 很快返回”，却误认为“数据已经很快传完”。

最后，本文的锁页解释不是所有平台的唯一模型。CUDA 文档明确说明，当 `pageableMemoryAccessUsesHostPageTables` 为真时，`cudaHostRegister()` 不会对指定区域锁页，而是补齐尚未建立的页面。Linux 文档也讨论了结合 MMU notifier、设备缺页能力管理用户内存的路径。因此，传统 pin-and-DMA 模型适合建立直觉，但不能替代具体平台的能力查询。

## 小结

Linux DMA API 管的是设备如何正确访问内存：使用哪种地址、如何映射、何时同步，以及何时交还所有权。Pin 管的是传统设备访问期间页面的稳定性；CUDA 注册还把这些页面纳入自己的设备访问机制。

**Pin 的加速来源是减少中间复制、支持有效的传输计算重叠，以及通过复用摊薄注册成本，而不是让 RAM 或 PCIe 突然变快。**

### 参考资料

- [Linux：Dynamic DMA mapping Guide](https://docs.kernel.org/core-api/dma-api-howto.html)
- [Linux：pin_user_pages() and related calls](https://docs.kernel.org/core-api/pin_user_pages.html)
- [CUDA Runtime API：Memory Management](https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__MEMORY.html)
- [CUDA Runtime API：API synchronization behavior](https://docs.nvidia.com/cuda/cuda-runtime-api/api-sync-behavior.html)
- [CUDA C++ Best Practices：Pinned Memory](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html#pinned-memory)
