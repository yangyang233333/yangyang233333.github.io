---
title: "nvidia-fs 源码阅读（二）：GPU 内存注册、影子页与 DMA 映射"
date: 2026-08-21T10:46:00+08:00
draft: false
tags: ["NVIDIA", "GPUDirect Storage", "GDS", "nvidia-fs", "GPU 内存", "DMA", "源码阅读"]
categories: ["技术"]
---

上一篇从 `nvfs_init()`、字符设备和 ioctl 看到了 `nvidia-fs` 的外部形态。本文进入 GDS direct path 的核心：怎样把用户分配的 GPU virtual address 变成存储设备能够 DMA 的地址，同时又让 Linux 文件 I/O 栈能够携带它。

阅读版本仍为 NVIDIA `gds-nvidia-fs`：

```text
commit: 328d1d8cce1175c013720985c30e123e9a35242c
GDS_VERSION: 2.29.4
```

## 一、问题本质

普通块 I/O 最终围绕 `bio_vec`、`struct page`、scatterlist 和 DMA mapping 展开。但 `cudaMalloc` 得到的是 GPU 虚拟地址，对 Linux 页缓存和块层来说，它并不是普通 CPU 内存页。

驱动需要解决三次“翻译”：

```text
GPU virtual address
  -> NVIDIA P2P page table 中的 GPU physical pages
  -> Linux I/O 栈可携带的影子 struct page
  -> 某个存储 PCIe 设备可使用的 DMA address
```

三个地址空间不能混为一谈：

- GPU virtual address 是应用看到的地址；
- P2P page table 描述 GPU 内存页；
- DMA address 与发起 DMA 的 peer device 有关，同一段 GPU 内存面对不同设备可能得到不同映射。

## 二、入口：`NVFS_IOCTL_MAP`

用户态注册 GPU buffer 后，`nvfs_ioctl()` 进入 `NVFS_IOCTL_MAP` 分支：

```text
nvfs_ioctl
  -> nvfs_map
  -> nvfs_map_gpu_info
  -> nvfs_pin_gpu_pages
```

`nvfs_map()` 先检查地址、长度和 ABI 参数，再创建描述对象。源码把 GPU 页大小定义为 64 KiB：

```c
#define GPU_PAGE_SHIFT 16
#define GPU_PAGE_SIZE ((u64)1 << GPU_PAGE_SHIFT)
```

驱动仍需处理系统 `PAGE_SIZE`，因此一个 GPU page 可能对应多个内核基础页单位。对齐、offset 和长度计算贯穿后续路径。

## 三、核心对象 `nvfs_gpu_args`

每个已注册 GPU buffer 都由 `nvfs_gpu_args` 一类对象维护。虽然字段会随版本演化，但职责稳定地包括：

- 用户 GPU 虚拟地址和长度；
- NVIDIA P2P page table；
- 影子 page 组成的 memory group；
- 针对不同 peer PCI 设备的 DMA mapping 缓存；
- 完成栅栏和元数据页；
- 当前状态、引用与正在执行的 I/O；
- GPU PCI BDF、UUID 等身份信息；
- RDMA 注册附加信息。

映射对象不能只以虚拟地址为键。进程地址空间、GPU 上下文、peer device 和生命周期都影响它是否仍然有效，因此代码使用锁、哈希结构、引用计数和状态机共同管理。

## 四、向 NVIDIA 驱动请求 P2P 页表

`nvfs_pin_gpu_pages()` 是注册流程的中心。它调用 NVIDIA GPU 驱动导出的 P2P 接口，为指定 GPU virtual range 获得 page table，并注册 free callback。

概念过程为：

```text
GPU VA + size
  -> nvidia_p2p_get_pages(..., free_callback)
  -> struct nvidia_p2p_page_table
  -> pages[] 描述 GPU 物理页
```

free callback 非常重要。GPU context 被销毁、显存被释放或驱动回收映射时，NVIDIA 驱动可以异步通知 `nvidia-fs`：这些页不能再用于新 DMA。

源码中的 `nvfs_get_pages_free_callback()` 不会把释放当成普通同步函数处理，而是进入终止状态机。这是因为 callback 到达时可能仍有 I/O 正在飞行，直接释放 page table 会造成 use-after-free 或设备访问无效地址。

## 五、为什么需要“影子页”

Linux 的文件和块 I/O 习惯传递 `struct page *`。GPU 显存并没有天然对应的普通系统内存 `struct page`，所以 `nvfs-mmap.c` 创建一组影子 page，也可理解为占位 page 或 carrier page。

影子页的作用不是保存数据，而是：

1. 让 GPU buffer 能进入 Linux 的 iov/bio/request 表示；
2. 在 page 私有元数据中关联回 GPU memory group；
3. 当存储驱动准备 DMA mapping 时识别“这是 GPU page”；
4. 依据 page offset 找回对应 GPU 物理页。

因此数据不会先写入影子页再复制到显存。影子页只是控制结构，真实 DMA 目标仍是 GPU 内存。

可以把它理解为：

```text
Linux 看到：struct page + offset + length
nvidia-fs 看到：这个 page 属于某个 GPU mgroup
DMA 层得到：对应 GPU page 的 peer DMA address
```

`nvfs_mmap()` 把这些页映射到用户态约定区域，使 `libcufile` 可以构造文件 I/O 所需的内存视图。`vm_operations_struct` 负责 VMA 生命周期和 fault 行为，避免把它当作普通匿名内存。

## 六、memory group 如何连接两套世界

源码中的 `nvfs_mgroup` 是影子页到 GPU 注册对象之间的桥梁。每个 I/O 页可以反查所属 group，再定位：

- GPU page table；
- GPU page index；
- 页内 offset；
- 当前映射状态；
- 稀疏文件元数据区；
- 终止与引用信息。

这使块层不需要理解 CUDA 地址空间。块层仍处理 page 和 request，只有在 DMA 映射阶段由 nvfs 扩展识别特殊页。

这也是 `nvidia-fs` 与存储驱动需要协作的根本原因：标准 `dma_map_page()` 面向普通系统内存，而 GPU peer memory 需要走 NVIDIA P2P mapping。

## 七、peer DMA mapping

获得 GPU physical pages 后，还不能直接把地址交给 NVMe。PCIe DMA 地址以具体 peer device 为上下文。

`nvfs_get_p2p_dma_mapping()` 和 `nvfs_get_dma()` 负责这部分工作，概念路径为：

```text
存储设备 struct pci_dev
  + nvfs_gpu_args
  + NVIDIA P2P page table
       │
       ▼
nvidia_p2p_dma_map_pages
       │
       ▼
nvidia_p2p_dma_mapping
       │
       ▼
按 GPU page index 取得 dma_address
```

映射结果会按 peer device 缓存。这样同一 GPU buffer 多次对同一个 NVMe 控制器发起 I/O 时，无需每次重新建立完整 peer mapping。

释放时则必须调用相应的 unmap API，并确保没有 request 仍引用该地址。

## 八、从 `bio_vec` 到 scatterlist

`nvfs-dma.c` 是存储数据真正指向 GPU 的关键文件。它提供两套适配：

- 较早接口的 `nvfs_blk_rq_map_sg`；
- 新内核 iterator 风格的 `nvfs_blk_rq_dma_map_iter_start/next`。

主要步骤为：

```text
遍历 request 中的 bio_vec
  -> 判断 page 是否属于 nvfs
  -> 反查 mgroup 和 GPU page index
  -> 获取该 peer device 的 P2P DMA mapping
  -> 计算 dma address + page offset
  -> 填充或迭代 scatter-gather segment
  -> 合并物理连续且满足边界约束的 segment
```

`nvfs_validate_gpu_request()` 会防止一个 request 混入不支持的页面组合。`nvfs_get_gpu_page_info()` 提取 GPU 地址和长度，`nvfs_check_bvec_contiguity()`、`nvfs_coalesce_gpu_pages()` 尝试合并连续 GPU 页，降低 SG entry 数量。

合并不是越多越好，还必须受以下条件约束：

- GPU page 的物理连续性；
- DMA segment 最大长度；
- boundary mask；
- offset 与 I/O 长度；
- request 队列和设备能力。

## 九、为什么要分析 PCIe 拓扑

`nvfs-pci.c` 保存 GPU 和 peer device 的 PCI 路径，计算距离、公共上游桥、链路速率和宽度，并检查 ACS。

原因是“能够 DMA”不代表“路径同样好”。例如：

```text
NVMe 与 GPU 在同一 PCIe switch
```

通常比：

```text
NVMe -> CPU Root Complex A -> 互连 -> Root Complex B -> GPU
```

具有更短路径和更少跨根端口流量。

驱动维护 GPU—peer rank matrix，并通过 `/proc/driver/nvidia-fs/peer_affinity`、`peer_distance` 等接口暴露结果或统计。这些信息可以帮助上层为 GPU 选择更合适的 NVMe 或网卡。

ACS 也会影响 peer-to-peer 流量能否按预期经过 PCIe 层级。源码显式遍历开启 ACS 的桥，说明 GDS 性能问题不能只从文件系统参数排查，还要看硬件拓扑和固件配置。

## 十、释放路径为什么复杂

GPU buffer 的正常注销大致为：

```text
阻止新 I/O
  -> 等待 active I/O 归零
  -> 清理各 peer DMA mapping
  -> 释放 P2P page table
  -> 撤销用户映射与影子 pages
  -> 释放 mgroup / gpu_info
```

但实际还存在 GPU 驱动 free callback、进程异常退出和模块卸载等入口。`nvfs_free_gpu_info()` 的 `from_dma` 参数、延迟释放统计和状态转换，都是为避免重复释放和并发引用。

源码阅读时应始终追问两个问题：

1. 当前对象是否还允许新 I/O 获取引用？
2. 最后一个异步 I/O 完成后，谁负责真正释放？

如果忽略这两个问题，只看 `nvidia_p2p_get_pages()` 和 `dma_map_pages()`，会错过驱动实现最困难的部分。

## 十一、完整映射链总结

```text
应用 GPU VA
  -> NVFS_IOCTL_MAP
  -> nvfs_pin_gpu_pages
  -> NVIDIA P2P page table
  -> nvfs-mmap 创建影子 pages / mgroup
  -> 文件 I/O 使用影子 pages
  -> request 进入存储驱动
  -> nvfs-dma 识别特殊 pages
  -> 按 peer PCI device 建立 P2P DMA mapping
  -> scatterlist / iterator 返回 GPU DMA addresses
  -> 设备直接 DMA 到 GPU 显存
```

这条链说明，GDS direct path 不是简单地把 `cudaMalloc` 指针传给 `read()`。它需要 GPU 驱动、Linux page 抽象、块层和存储驱动共同接受一套受控的地址转换协议。

下一篇将沿 `NVFS_IOCTL_READ/WRITE` 继续：`nvfs_io_init()` 如何构造请求，怎样进入 Linux 文件操作，异步完成如何回写状态，以及 batch I/O、稀疏文件与 `/proc/fs/nvfs/stats` 如何工作。

## 参考资料

- NVIDIA `gds-nvidia-fs` 官方仓库，commit `328d1d8cce1175c013720985c30e123e9a35242c`
- `src/nvfs-core.c`、`src/nvfs-core.h`
- `src/nvfs-mmap.c`、`src/nvfs-mmap.h`
- `src/nvfs-dma.c`、`src/nvfs-dma.h`
- `src/nvfs-p2p.h`
- `src/nvfs-pci.c`
