---
title: "Phoenix 论文中文详译：一种不使用伪缓冲区的 GPU Direct Storage 重构方案"
date: 2026-08-24T14:49:50+08:00
draft: false
tags: ["GPU Direct Storage", "P2P-DMA", "ZONE_DEVICE", "Linux 内核", "SC 2025", "论文翻译"]
categories: ["论文阅读"]
summary: "按原论文结构详译 SC'25 论文 Phoenix：解释 NVIDIA GDS 的伪缓冲区问题、基于 ZONE_DEVICE 的 GPU 显存映射、POSIX/io_uring 编程模型，以及本地 NVMe、远程存储、KV Cache 和模型加载实验。"
---

> **译者说明**：本文是对 SC'25 论文 *Phoenix: A Refactored I/O Stack for GPU Direct Storage without Phony Buffers* 的章节级中文忠实详译。为适合公开博客阅读，本文保留原论文的完整论证顺序、设计要点、实验设置、关键数据和作者结论，但不逐句复制论文排版文本；部分图表以文字和表格重述。建议研究引用使用论文原文。

- 论文 DOI：[10.1145/3712285.3759862](https://doi.org/10.1145/3712285.3759862)
- 会议：SC '25（The International Conference for High Performance Computing, Networking, Storage and Analysis）
- 作者：Jianqin Yan、Shi Qiu、Yina Lv、Yifan Hu、Hao Chen、Zhirong Shen、Xin Yao、Renhai Chen、Jiwu Shu、Gong Zhang、Yiming Zhang
- 开源实现：[xPU-IO/Phoenix](https://github.com/xPU-IO/phoenix)

## 术语约定

| 英文 | 本文译法 | 含义 |
| --- | --- | --- |
| GPU Direct Storage, GDS | GPU 直接存储 | 存储设备与 GPU 显存之间直接传输数据的技术栈 |
| Peer-to-Peer DMA, P2P-DMA | 点对点 DMA | PCIe 设备之间不经过主机 DRAM 的 DMA |
| bounce buffer | 中转缓冲区 | 传统路径中位于主机内存的数据中转区 |
| phony buffer | 伪缓冲区 | GDS 为满足 Linux `struct page` 语义而创建、代表 GPU buffer 的主机内存 |
| GPU buffer | GPU 缓冲区 | GPU 显存中的应用数据区域 |
| ZONE_DEVICE | 设备内存区 | Linux 将设备内存纳入页模型的机制 |
| HBM | 高带宽显存 | GPU 板载高带宽内存 |

# 摘要

GPU Direct Storage（GDS）是 GPU 训练与推理系统中的重要组成部分。它利用 PCIe 的 P2P-DMA，在 GPU 与存储设备之间建立直接数据通路。与传统的 CPU 中转路径相比，这条直达路径能够降低存储访问延迟和 CPU 开销，提高数据传输效率。

然而，现有 GDS 为了与 Linux 内核交互，会在主机内存中使用一种“伪缓冲区”。这种设计带来三个问题：I/O 性能不理想、资源消耗额外增加，以及部署与编程复杂度较高。

论文提出 **Phoenix**：一种不依赖伪缓冲区的重构版 GDS I/O 栈。Phoenix 使用 Linux 4.3 起支持的 `ZONE_DEVICE` 内存映射服务，在系统启动阶段将 GPU 内存映射进 Linux 页表。Phoenix 内核模块保存映射产生的地址信息，在用户空间分配虚拟内存，并将其与指定 GPU 内存建立映射。

作者在新近发布的 GPU 和 NPU 上实现了 Phoenix。实验表明，相比当时最先进的 GDS I/O 栈，Phoenix：

- 将 I/O 关键路径上的平均软件处理开销降低 **70.3%**；
- 将 KV Cache 等小粒度 I/O 的性能最高提升到 **2.29 倍**；
- 将 checkpoint 等大文件加载性能最高提升到 **4.11 倍**。

# 1. 引言

大语言模型等 GPU 加速应用对存储容量的需求持续增长。模型、训练数据和中间状态可能分布在大量文件中，总规模达到数十 TB，并且仍在扩大。GPU 与存储设备之间的数据传输必须足够快，否则存储访问会成为阻塞 GPU 计算的瓶颈。

传统 GPU 存储访问通常依赖主机中转缓冲区。以 PyTorch DataLoader 为例，数据先通过主机文件系统接口从存储设备读到 CPU 内存，然后再由设备驱动复制到 GPU 显存：

```text
存储设备 -> 主机中转缓冲区 -> GPU 显存
```

额外的数据复制拉长了路径，降低 I/O 性能。

为绕过中转缓冲区，NVIDIA 提出了 GDS，使 GPU 显存与 NVMe 等存储设备之间能够直接传输数据。GDS 以 PCIe P2P-DMA 为基础。传统 DMA 的目标通常是主机内存，而 P2P-DMA 的地址指向另一个 PCIe 外设的内存。数据传输由 PCIe Switch 或 Root Complex 协调，不必经过低效的主机 DRAM 中转。

GDS 对 I/O 密集的 GPU 应用尤其重要，例如：

- KV Cache 卸载与回载；
- 模型 checkpoint 加载和保存；
- 分布式文件系统中的 GPU 数据访问；
- 大模型服务系统中的存储分层。

但是，论文认为当时的 GDS 软件栈没有充分发挥 P2P-DMA 的优势。

## 1.1 为什么 GDS 需要“伪缓冲区”

Linux 内核处理 DMA I/O 时，需要为相关内存页取得 `struct page` 引用，保证 I/O 进行期间页面不会被释放。普通 CPU 内存天然属于 Linux 页管理体系；GPU 显存却不一定具有可供通用文件系统和块层直接使用的 `struct page`。

为解决这个问题，GDS 在主机内存中申请一块与 GPU buffer 对应的伪缓冲区。文件系统和块设备驱动先把它当成普通用户缓冲区；NVMe 驱动接收到请求后，再把伪缓冲区中的 DMA 地址替换成 GPU buffer 的 DMA 地址，由此实现 P2P-DMA。

可以把它理解为：

```text
应用 GPU buffer
      |
      | 由主机内存中的伪 buffer 代表
      v
Linux 文件系统 / 块层
      |
NVMe 驱动替换 DMA 地址
      |
      v
真正 DMA 到 GPU 显存
```

伪缓冲区不承载实际数据，但承担了“让 Linux 相信这里有一组合法内存页”的角色。

## 1.2 伪缓冲区带来的三个问题

### 性能不理想

为了创建、维护和释放伪缓冲区，GDS 引入了复杂且耗时的逻辑。伪缓冲区的申请和释放也会增加关键路径开销，最终影响整体 I/O 性能。

### 资源消耗过高

伪缓冲区与 GPU buffer 大小相同，因此会额外消耗主机内存。异步 I/O、批量 I/O、伪缓冲区管理及其状态维护还会消耗大量 CPU 周期。

### 兼容性较差

GDS 需要定制设备驱动来替换 DMA 地址。I/O 过程中还必须增加伪缓冲区的引用计数，避免它被提前释放。因此应用不能简单使用标准 POSIX 接口，而要使用 GDS 提供的专用接口，增加了编程和异步 I/O 实现的复杂度。

## 1.3 Phoenix 的思路与贡献

Phoenix 的目标是重构 GDS，彻底去掉伪缓冲区。

它使用 `ZONE_DEVICE` 把 GPU 内存直接映射进 Linux 页表，在系统初始化时为 GPU 显存建立 `struct page`。随后，Phoenix 内核模块保存地址信息，在用户空间建立虚拟地址，并把这段虚拟地址与指定 GPU 显存映射起来。

这样，GPU 显存具备了 Linux I/O 所需要的页语义，文件系统不再需要主机伪缓冲区，也不再需要在 NVMe 驱动中替换 DMA 地址。

论文总结的主要贡献是：

1. 分析现有 GDS I/O 栈，指出伪缓冲区会造成软件处理开销、资源浪费和兼容性问题；
2. 提出 Phoenix，通过 `ZONE_DEVICE` 直接为 GPU 内存提供 Linux 页描述和用户空间映射；
3. 重新设计面向 GDS、GDR 和 CUDA Stream 的编程模型，使应用可以使用 POSIX 和 `io_uring`；
4. 在本地 NVMe、远程存储、KV Cache 和模型加载场景中验证性能收益。

# 2. 背景与动机

## 2.1 ZONE_DEVICE

Linux 4.3 引入 `ZONE_DEVICE`，将设备内存接入内核的内存区域和页管理体系。驱动可以为设备暴露的物理地址范围建立 `dev_pagemap`，内核则为该范围构造对应的 `struct page`。

这项机制最初常用于持久内存和 GPU 等设备内存。建立映射后，设备内存可以通过 `mmap` 等标准接口暴露给用户空间。由于内核能够识别这些页，应用可以避免中间缓冲区和额外复制，也更容易实施直接 I/O 与 P2P-DMA。

Phoenix 正是利用 `ZONE_DEVICE` 为 GPU 显存提供 `struct page` 服务，从根本上消除 GDS 对伪缓冲区的需求。

## 2.2 NVIDIA GDS 的架构与流程

论文把 NVIDIA GDS 分为三部分：

1. 用户态 `libcufile`；
2. 内核模块 `nvidia-fs`；
3. 支持 GDS 的定制 NVMe 或网络设备驱动。

一次典型同步 GDS 操作包含以下阶段：

1. 打开 GDS 驱动并检查系统兼容性；
2. 打开普通文件；
3. 向 GDS 注册文件句柄；
4. 注册 GPU buffer，并为它创建伪缓冲区；
5. 通过 `cuFileRead` 或 `cuFileWrite` 发起 I/O；
6. 注销 GPU buffer 并释放伪缓冲区；
7. 注销文件句柄；
8. 关闭文件；
9. 关闭 GDS 驱动。

真正的数据面在第 5 步。应用把请求提交给 `nvidia-fs`，内核再把请求交给文件系统、块层和 NVMe 驱动。定制驱动用 GPU buffer 的 DMA 地址替换伪缓冲区地址，最终完成 P2P-DMA。

伪缓冲区导致 GDS 必须：

- 使用定制设备驱动；
- 在初始化和清理阶段执行额外兼容性检查与函数注册；
- 在注册阶段分配主机内存；
- 在 I/O 期间维护引用计数；
- 强制应用使用非 POSIX 的专用 I/O 接口。

## 2.3 软件栈开销分析

作者使用 Intel Optane P5800X 测量 GDS 各步骤延迟。该设备执行 64 KiB 读取的底层介质延迟约为 25 微秒。

对一笔 64 KiB 同步读取，GDS 除真正 I/O 外还需要多个管理步骤。论文把它们分为：

- **驱动管理**：驱动打开和关闭；
- **文件管理**：文件句柄注册和注销；
- **缓冲区管理**：GPU buffer 注册和注销。

驱动和文件管理通常只在应用开始与结束时执行一次，但在 Serverless、RPC 或频繁初始化的场景中仍会影响端到端性能。

缓冲区管理更加关键：每个参与 I/O 的 GPU buffer 都需要注册。其开销位于关键路径上，甚至可能超过真正的数据传输时间。

作者测得，对 64 KiB 读取而言，GDS 软件栈开销占关键路径总延迟的 **82.5%**。这正是 Phoenix 要重点消除的部分。

# 3. Phoenix 的设计与实现

Phoenix 的设计目标是：

- 高兼容性；
- 高性能；
- 低资源消耗。

核心挑战在于：如何把应用的用户空间虚拟地址与 GPU 显存地址建立可靠映射，并使 Linux 文件系统能够取得这些页的 `struct page`。

## 3.1 总体架构

Phoenix 包含两部分：

- 内核模块 `phoenix.ko`：负责 GPU 内存页映射和管理；
- 用户态 `libphoenix`：负责设备与缓冲区管理，向应用提供 I/O 接口。

数据路径为：

```text
应用
  |
libphoenix
  |
POSIX / io_uring
  |
文件系统 -> 块层 -> 标准 NVMe 驱动
  |
P2P-DMA
  |
GPU HBM
```

与 GDS 相比，Phoenix 不需要伪缓冲区，也不需要 NVMe 驱动在请求中替换 DMA 地址。

## 3.2 缓冲区管理

### 3.2.1 内存初始化

Phoenix 在内核模块初始化阶段读取 GPU BAR 地址和显存容量。随后调用 `ZONE_DEVICE` 映射服务，为 GPU 可寻址空间构造 `struct page`，并保存物理地址、页描述和映射元数据。

作者实验中的 GPU 有 48 GiB 板载显存，但通过 PCIe 暴露 64 GiB 可寻址空间，其中包含额外的 host-mapped 区域。为这 64 GiB 地址空间构造 `struct page` 大约需要 **150 ms**。

这项成本发生在模块初始化阶段，而不是每笔 I/O 或每次 buffer 注册时。

### 3.2.2 GPU Buffer 注册

应用调用 `phxfs_regmem` 注册 GPU buffer。其接口大致为：

```c
int phxfs_regmem(
    int device_id,
    const void *addr,
    size_t len,
    void **target_addr);
```

其中：

- `addr` 是应用持有的 GPU 虚拟地址；
- `len` 是注册长度；
- `target_addr` 返回 Phoenix 创建的用户空间虚拟地址。

注册流程可以概括为：

1. 内核通过 GPU 驱动接口 pin 指定 GPU buffer；
2. 获取 GPU 页表和物理地址信息；
3. 应用使用 `mmap` 分配一段用户虚拟地址；
4. Phoenix 把该虚拟地址映射到此前由 `ZONE_DEVICE` 建立的 GPU 页；
5. 保存“应用 GPU 地址—Phoenix 虚拟地址—物理页”的映射关系。

得到的 `target_addr` 对 CPU 来说是合法用户虚拟地址，对 Linux 内核来说背后有合法 `struct page`，但实际数据位于 GPU 显存。

因此，标准文件 I/O 可以直接使用 `target_addr`：

```c
pread(fd, target_addr, size, file_offset);
```

底层 DMA 最终写入 GPU HBM，而不是主机 DRAM。

注销时，Phoenix 解除映射并释放 GPU 页表引用，不需要申请或释放同等大小的主机伪缓冲区。

## 3.3 编程模型

### 3.3.1 与 GPU Direct Storage 集成

Phoenix 提供设备打开、关闭、注册和注销接口。完成注册后，应用可通过 `phxfs_xfer` 或标准 POSIX 文件接口发起 I/O。

与 GDS 不同，Phoenix 不需要 `cuFileHandleRegister` 一类文件句柄注册。它直接使用 POSIX 文件描述符，并将映射后的用户虚拟地址作为 I/O buffer。

由于不再维护伪缓冲区状态，Phoenix 可以直接复用 Linux 原生 I/O 能力：

- 同步 `pread/pwrite`；
- 异步 `io_uring`；
- 批量 I/O；
- 文件系统和远程存储的标准访问路径。

### 3.3.2 与 GPU Direct RDMA 集成

NVIDIA Magnum IO 同时包含 GDS 和 GPU Direct RDMA（GDR）。传统方案通常分别安装 `nvidia-fs` 和 `nvidia-peermem` 等内核模块。

Phoenix 映射后的用户虚拟地址既可以交给文件系统，也可以注册为 RDMA Memory Region。这样，同一段 GPU 内存只需注册一次，就可以同时用于存储访问和 RDMA 网络传输。

论文认为，这提供了一种更统一的 GPU P2P 访问机制，并减少重复模块依赖。

### 3.3.3 与 CUDA Stream 集成

CUDA Stream 可以表达 GPU 操作之间的顺序和依赖。为了把文件 I/O 纳入 stream，Phoenix 使用 CUDA callback/host function：

```text
前序 GPU 工作
    |
CUDA callback：执行同步 POSIX I/O
    |
后续 GPU 工作
```

Phoenix 将文件描述符、buffer 地址、长度、文件偏移等 I/O 元数据传给 callback。callback 通过 POSIX 接口发起 GDS I/O，并把结果写入调用者提供的 `bytes_done`。

为了保证 stream 内的数据完整性，一笔 I/O 的发起与结果检查必须完全包含在 callback 中。因此论文实现选择在 callback 内直接执行同步 I/O，而不是使用异步接口。作者认为这样可以避免额外异步管理开销。

# 4. 实验评估

实验试图回答四个问题：

1. Phoenix 能降低多少 GDS 软件栈开销？
2. 在同步、异步、批量和 CUDA Stream I/O 中，Phoenix 有多少性能优势？
3. 在本地 NVMe 和远程网络存储上表现如何？
4. 在 KV Cache 和 checkpoint 加载等 LLM 工作负载中表现如何？

## 4.1 实验平台

| 项目 | 配置 |
| --- | --- |
| CPU | 128 核 Intel Xeon 6530 |
| 主机内存 | 512 GiB |
| 操作系统 | Ubuntu 22.04 |
| Linux 内核 | 6.1.0 |
| GPU | 48 GiB 显存 |
| CUDA | 12.4 |
| NVIDIA GDS | `nvidia-fs` 2.19 |
| 网络 | Mellanox ConnectX-5 100G，支持 RDMA |
| 本地/后端存储 | Intel Optane P5800X |
| 远程协议 | NVMe-oF 与 NFS |

GPU、NIC 和 NVMe 位于同一 PCIe Switch 下，以保证能够执行 P2P-DMA。

基线为 NVIDIA GDS，使用官方 Magnum IO 示例和 `cuFile` 接口实现。除专门评估注册开销的实验外，其他实验都预先注册 GPU buffer，尽量把数据面性能与注册性能分开。每组实验总 I/O 量至少为 40 GiB。

## 4.2 操作开销分解

作者将 GPU buffer 大小从 4 KiB 增加到 16 MiB，重复测量 GDS 和 Phoenix 的注册、注销和 I/O 开销。

GDS 为平衡内存开销和效率，把单次注册的 GPU 内存限制为 16 MiB。当请求大于等于 16 MiB 时，GDS 内部固定使用 16 MiB 伪缓冲区，并将大请求拆成多个 16 MiB 分段顺序提交。这会损害大粒度 I/O 性能。

### 驱动管理

Phoenix 的 driver open/close 延迟几乎可以忽略。GDS 在打开阶段需要检查系统兼容性、注册定制驱动函数，关闭时需要释放伪缓冲区和注销函数；Phoenix 不需要这些步骤。

Phoenix 的 `ZONE_DEVICE` 初始化虽然需要约 150 ms，但只在内核模块初始化时执行一次。

### Buffer 注册与注销

相较 GDS：

- Phoenix 的注册延迟降低 **63%–90%**；
- Phoenix 的注销延迟降低 **54%–75%**；
- 关键路径平均软件处理开销降低 **70.3%**。

收益主要来自不再申请和释放主机伪缓冲区。

## 4.3 与 NVIDIA GDS 的本地 NVMe 对比

### 同步 I/O

在 4 KiB 到 1 GiB 的不同块大小下，Phoenix 对同步读取的带宽提升为 **1%–76%**。

小 I/O 的提升最明显，因为 GDS 的状态管理和 DMA 地址查询等固定软件开销占比更大。随着块大小增加，设备数据传输时间成为主导，固定开销影响降低。

当请求大于 16 MiB 时，GDS 的内部拆分又带来额外开销，因此 Phoenix 在大块请求上重新拉开差距。同步写入呈现相似趋势。

### 多线程小 I/O

论文使用 4 KiB 同步读测试并发扩展性。随着线程数从 1 增加到 512，Phoenix 能更充分地把并行请求交给 Linux I/O 栈，而 GDS 受专用接口和内部管理开销影响更大。

Phoenix 在高并发下更接近底层 NVMe 的带宽上限，说明移除伪缓冲区不仅降低单请求延迟，也减少了共享状态和 CPU 管理成本。

### 批量异步 I/O

Phoenix 使用 Linux `io_uring` 实现异步与 batch I/O。GDS 使用 `cuFileBatchIO`。

Phoenix 不需要为每个 batch 请求维护伪缓冲区状态，也不受 GDS 单批最多 **256 个请求**的接口限制。对于大量小请求，Phoenix 可以在一次批量提交中覆盖更多 I/O，并减少多轮提交开销。

### CUDA Stream

作者还比较了 CUDA Stream 中的异步 I/O。Phoenix 通过 callback 把同步 POSIX I/O插入 stream，避免 GDS 异步接口的额外状态管理。

这一实现重点不是让单笔存储 I/O变成异步，而是让 I/O 与 stream 中的 GPU 工作保持正确顺序，并减少软件栈成本。

## 4.4 端到端结果

作者设计了两类端到端场景。

### 小粒度 I/O

每个请求都包含 GPU buffer 注册和读取，用于模拟数据集与 KV Cache 等频繁小 I/O。

Phoenix 同时降低 buffer 管理和数据传输开销。论文摘要给出的代表性结果是，小粒度 I/O 性能最高达到 GDS 的 **2.29 倍**。

### 大文件加载

该场景模拟 checkpoint 加载，每次操作包含：

- 驱动管理；
- 文件管理；
- GPU 内存注册；
- 数据传输；
- 清理过程。

Phoenix 移除了大量启动、注册和分段开销。论文摘要报告，大文件加载性能最高达到 GDS 的 **4.11 倍**。

## 4.5 远程网络存储

GDS 的远程存储路径要求网络或存储客户端与其定制接口协作。Phoenix 依赖标准 POSIX I/O 和 Linux 页语义，因此更容易复用现有 NVMe-oF、NFS 和 RDMA 栈。

作者在 100G ConnectX-5 环境中测试 NVMe-oF 与 NFS。结果显示，Phoenix 在远程路径上仍能提供与本地实验一致的收益：小请求受益于更低的软件开销，大请求最终受网络和后端设备带宽限制。

论文强调，Phoenix 的优势不仅是某个本地 NVMe microbenchmark 更快，还包括更大的存储访问范围。与直接把 NVMe 队列交给 GPU 的方案相比，它保留文件系统语义，也能扩展到网络存储。

## 4.6 LLM 工作负载

### 卸载 KV Cache 的回载

LLM 推理系统通常使用 PagedAttention，以 block 为粒度管理 KV Cache。所有 token 的 KV block 分散在一块预分配的连续 GPU 内存中。

作者使用四组真实对话 trace：

| Trace | 序列数 | Block 数 |
| --- | ---: | ---: |
| Paper Assistant | 23 | 15,708 |
| GSM-100 | 100 | 33,600 |
| QuALITY | 15 | 6,428 |
| ShareGPT-197 | 197 | 20,195 |

实验用大文件模拟卸载到存储的 KV Cache，按模型层读取。作者考虑三种 KV block 大小：

- INT8 + MLA：8 KiB；
- FP16 + MLA：16 KiB；
- FP16：64 KiB。

其中假设 MLA 将 KV Cache 大小减少 75%。

读取时，系统尽量在一次 batch 中取回一个序列的所有 block，并把物理连续 block 合并成更大的请求。

GDS 每次 batch 最多支持 256 个请求，某些长序列必须多次提交；Phoenix 没有这一限制，因为它去掉伪缓冲区依赖，并直接集成 `io_uring`。

相较 GDS，Phoenix 在三种配置下分别提升：

- **246%**（8 KiB）；
- **101%**（16 KiB）；
- **5%**（64 KiB）。

随着 KV block 增大，Phoenix 的优势逐渐缩小。这与前面的结论一致：Phoenix 主要消除了固定软件开销，因此对碎片化、小粒度访问帮助最大；大粒度传输更容易被设备带宽主导。

### 模型 Checkpoint 加载

作者使用开源 C++ safetensors 解析库。程序先把 safetensors header 读入主机内存，解析每个 tensor 的元数据，然后按照文件 offset 将 tensor 内容直接传到 GPU buffer。

对比三种方案：

1. 原生 `mmap + cudaMemcpy`；
2. NVIDIA GDS；
3. Phoenix。

结果显示：

- GDS 相对原生方案减少模型加载延迟 **39%**；
- Phoenix 相对原生方案减少模型加载延迟 **54%**；
- Phoenix 相对 GDS 再减少 **23%** 的 checkpoint 加载延迟。

Phoenix 的收益同时来自 buffer 管理与数据传输两部分。

# 5. 讨论

## 5.1 Direct I/O 的对齐限制

GDS 本质上仍是 direct I/O，因此 NVIDIA GDS 和 Phoenix 都受直接 I/O 约束。GPU buffer 地址、I/O 长度和文件 offset 必须按特定粒度对齐，例如 4 KiB；否则请求会失败。

应用需要显式保证内存分配和访问模式满足对齐要求。这可能增加显存浪费和编程复杂度。

例如，实际数据没有按 GPU 页粒度（如 64 KiB）对齐时，应用仍要按整页申请显存。为了满足对齐而执行的 padding 或额外数据复制，也可能降低整体效率。

## 5.2 单次映射大小限制

Phoenix 通过 `mmap` 建立应用虚拟地址与 GPU 重映射页之间的关系。论文实现受 `mmap` 单次映射规模限制：使用大页时，每次最多映射约 1 GiB。

注册更大的 GPU buffer 时，应用必须管理多个映射段，例如向 `phxfs_regmem` 提供指针数组保存多个返回地址。

相比之下，论文中的 NVIDIA GDS 单次 buffer 注册上限是 16 MiB，因此不会遇到同一种 1 GiB 单映射限制，但会更频繁地进行内部拆分。

## 5.3 统一 P2P 访问

Phoenix 的注册区既可以服务 GDS，也可以注册为 GDR 的 RDMA Memory Region。因此用户有机会用一个模块和一次内存注册同时支持：

- 存储设备到 GPU；
- RDMA NIC 到 GPU；
- 在必要时通过内存语义访问映射区。

作者也提醒，直接使用 CPU 内存访问语义读写 Phoenix 映射区可能产生一致性问题，需要应用谨慎处理 GPU、CPU 和设备之间的同步。

# 6. 相关工作

## 6.1 P2P 直接存储访问

### SPIN

SPIN 较早使用主机伪缓冲区代表 GPU buffer，与 direct disk I/O 接口交互。它能无缝接入操作系统，但继承了伪缓冲区的性能和资源问题。

### BaM

BaM 更激进：把 NVMe 队列直接映射到 GPU 内存，让 GPU 通过 P2P-DMA 发送 NVMe 命令，甚至接管 NVMe 控制面，完全绕过 CPU。

这种方案性能很强，但类似 SPDK，放弃了通用文件系统抽象。它难以满足典型训练和推理场景中的高并发共享与文件语义需求。

### GeminiFS

GeminiFS 通过扩展 NVMe 驱动，在 CPU 与 GPU 之间共享控制面，并在文件头中嵌入文件系统元数据，为 GPU 程序提供配套文件系统抽象。

但 BaM 和 GeminiFS 的访问范围主要局限于本地 NVMe，难以像 Phoenix/GDS 一样扩展到网络存储。

### FlashNeuron 与 hcache

这类方案使用 GDRCopy 暴露 GPU 内存、获取 DMA 地址，再通过 SPDK 用户态块 I/O 发起存储与 GPU 之间的 P2P 传输。它们同样缺乏文件系统语义。

作者认为，相比 SPIN/GDS 的伪缓冲区路线，Phoenix 的栈更简单、性能更高、Linux 兼容性更好；相比 GPU 接管 NVMe 的路线，Phoenix 的存储访问范围和易用性更强。

## 6.2 其他 GPU 直接数据传输技术

论文写作时，NVIDIA GDS 是唯一实用的 GPU—存储双向直接传输方案。

AMD DirectGMA 将 GPU 内存映射到 PCIe BAR，使 PCIe 设备内存之间可以直接传输，但普通商用存储设备不能主动协调完整 GPU Direct Storage 流程。

AMD peer memory client 可以支持 GPU 与 RDMA NIC 之间的直接传输，但不提供 GPU 存储访问。

Microsoft DirectStorage 通过允许 GPU 直接读取 NVMe 数据来加速游戏资源加载，但主要支持存储到 GPU 的单向传输。

# 7. 结论

论文提出 Phoenix：一种去除伪缓冲区的重构版 GDS 软件栈。

Phoenix 使用 Linux 4.3 起提供的 `ZONE_DEVICE`，在系统初始化阶段直接将 GPU 内存接入 Linux 页表。通过重新设计映射和注册流程，它减少了 I/O 关键路径的软件开销与资源消耗，并使应用可以使用标准 POSIX 和 `io_uring` 接口。

实验显示，在小粒度 I/O、多线程 I/O、端到端加载和真实 LLM 工作负载中，Phoenix 相比当时的 NVIDIA GDS 具有更高的存储性能、更低的软件栈开销和更好的兼容性。

作者计划继续把 Phoenix 集成到 PyTorch、TensorFlow 等框架中，以优化大语言模型、图神经网络等 GPU 应用。

# 译者小结

这篇论文最重要的观察，不是“P2P-DMA 比 CPU 中转更快”——这已经是 GDS 的基本前提——而是：

> 现有 GDS 为了适配 Linux 页模型，引入了一个与数据无关却主导软件开销的主机伪缓冲区；如果直接把 GPU 显存纳入 Linux 的 `struct page` 与用户虚拟地址体系，就能删掉大段专用 I/O 栈。

Phoenix 选择保留 Linux 文件系统、POSIX 和 `io_uring`，只重构 GPU 内存如何进入内核页模型。它没有把 NVMe 控制面搬到 GPU，也没有放弃文件系统，因此性能上未必在所有场景压过 GPU-centric 存储栈，但兼容性和可组合性更强。

论文结果也清楚说明了优化边界：收益最大的是注册频繁、请求细碎、batch 数量大、软件固定成本占主导的场景；当 I/O 足够大、设备或网络带宽成为瓶颈时，去掉伪缓冲区带来的相对优势会缩小。

结合当前开源代码看，Phoenix 已从论文原型继续演进，加入了 NUMA worker pool、`io_uring` 引擎、vLLM/LMCache 适配器、staging 映射模式和更完整的生命周期管理。论文解释了它为什么成立，源码则展示了这套思路如何逐步工程化。

# 参考资料

- [论文 DOI：Phoenix: A Refactored I/O Stack for GPU Direct Storage without Phony Buffers](https://doi.org/10.1145/3712285.3759862)
- [Phoenix 当前开源仓库](https://github.com/xPU-IO/phoenix)
- [Phoenix 论文版本开源仓库](https://github.com/nicexlab/phoenix)
- [Linux ZONE_DEVICE / Device Memory 文档](https://docs.kernel.org/mm/hmm.html)
- [Linux PCI Peer-to-Peer DMA 文档](https://docs.kernel.org/driver-api/pci/p2pdma.html)
