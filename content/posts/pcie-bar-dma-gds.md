---
title: "PCIe BAR 深入解析：从设备地址窗口到 DMA 与 GPUDirect Storage"
date: 2026-08-21T16:25:00+08:00
draft: false
tags: ["PCIe", "BAR", "DMA", "IOMMU", "GPUDirect Storage", "GPU Direct"]
categories: ["技术"]
---

在操作系统驱动、高性能网卡、NVMe SSD 和 GPU 系统中，经常会同时看到 BAR、MMIO、DMA、IOMMU、peer-to-peer DMA、GPUDirect RDMA 和 GPUDirect Storage 等概念。这些名词都与“设备怎样通过 PCIe 交换控制信息和数据”有关，但它们处在不同层次。

最简洁的理解是：

```text
BAR：让 CPU 能够定位并访问 PCIe 设备中的寄存器或显存窗口
DMA：让 PCIe 设备能够主动读取或写入系统内存
P2P DMA：让一个 PCIe 设备直接访问另一个 PCIe 设备暴露的地址空间
GDS：利用 DMA、GPU 内存映射和驱动协作，让存储数据尽量直接进入 GPU 显存
```

本文从 PCIe 地址空间和事务模型出发，解释 BAR 是如何分配和映射的，驱动为什么通过 BAR 下发命令，DMA 地址为什么不能简单等同于物理地址，以及 GPUDirect Storage 如何把这些机制组合成一条高性能数据路径。

## 一、先建立 PCIe 系统视图

一个典型服务器的 PCIe 拓扑如下：

```text
                         CPU
                          │
                Memory Controller
                          │
                       Host RAM
                          │
                    Root Complex
                   ┌──────┴──────┐
                   │             │
              PCIe Switch    PCIe Endpoint
              ┌────┴────┐         GPU
              │         │
           NVMe SSD     NIC
```

CPU 和内存构成主机侧。Root Complex 把 CPU/内存系统连接到 PCIe fabric。NVMe、网卡和 GPU 通常是 Endpoint。PCIe Switch 用于扩展端口和转发事务。

PCIe 并不是简单的“串行总线寄存器协议”。它传输的是事务层数据包，即 TLP。常见事务包括：

- Memory Read：读取某个地址。
- Memory Write：写入某个地址。
- Configuration Read/Write：访问 PCIe 配置空间。
- Completion：返回读请求结果或状态。
- Message：传递中断、电源管理等消息。

理解 BAR 和 DMA 的关键，不是只看“谁拷贝数据”，而是看：

1. 谁发起 PCIe Memory Read/Write；
2. 请求中的地址属于哪个地址空间；
3. Root Complex、IOMMU 或 Switch 如何路由该请求；
4. 最终由主机内存还是某个 PCIe Endpoint 响应。

## 二、PCIe 设备有哪些地址空间

PCIe 设备通常涉及三类容易混淆的地址空间。

### 1. 配置空间

每个 PCIe Function 都有配置空间，用于描述设备身份、能力和资源需求。常见字段包括：

- Vendor ID 和 Device ID；
- Class Code；
- Command 和 Status；
- BAR0 到 BAR5；
- MSI/MSI-X capability；
- PCIe capability；
- SR-IOV、ATS、Resizable BAR 等扩展能力。

操作系统通过配置事务枚举设备。Linux 中常见的 BDF：

```text
0000:65:00.0
```

分别表示 domain、bus、device 和 function。

配置空间不是设备传输大块业务数据的主要通道。它主要用于发现设备、启用功能和分配资源。

### 2. 主机物理地址空间

CPU 看到的是统一的物理地址空间，其中既可以包含 DRAM，也可以预留若干 MMIO 区域：

```text
CPU Physical Address Space

0x0000_0000  ┌──────────────────────┐
             │ Host DRAM            │
             │ ...                  │
0x8000_0000  ├──────────────────────┤
             │ PCIe MMIO window     │──> Device BAR resources
             │ Firmware / APIC ...  │
0xFFFF_FFFF  └──────────────────────┘
```

CPU 对 DRAM 地址执行 load/store，最终访问内存控制器；对 MMIO 地址执行 load/store，Root Complex 会把访问转换为 PCIe Memory Read/Write TLP，并路由到目标设备。

### 3. 设备使用的 DMA 地址空间

PCIe 设备发起 DMA 时，请求中携带的是设备可见地址。在没有 IOMMU 或采用恒等映射时，它可能等于主机物理地址；启用 IOMMU 后，它通常是 IOVA：

```text
Device-visible DMA address / IOVA
                 │
                 ▼
               IOMMU
                 │
                 ▼
        Host physical address
                 │
                 ▼
              Host RAM
```

因此，程序中的虚拟地址、CPU 物理地址和 DMA 地址不能天然互换：

```text
CPU virtual address != CPU physical address != DMA address / IOVA
```

驱动必须使用内核 DMA API 建立映射，而不能随意把普通指针交给设备。

## 三、BAR 到底是什么

BAR 是 Base Address Register。它位于 PCIe 配置空间中，用于描述某个 Function 希望暴露的一段资源窗口。

一个 PCIe Function 最多通常有六个常规 BAR。每个 BAR 可以描述：

- Memory BAR：设备寄存器或设备内存窗口；
- I/O BAR：传统端口 I/O 空间，现代 PCIe 设备很少依赖；
- 32 位或 64 位地址；
- prefetchable 或 non-prefetchable 属性。

64 位 BAR 会占用两个连续 BAR 寄存器。例如 BAR0 和 BAR1 共同保存一个 64 位基地址。

需要注意：

> BAR 寄存器保存的是系统分配给设备资源窗口的基地址，不是设备寄存器内容本身。

例如，NVMe 控制器内部定义了一组寄存器：

```text
offset 0x0000: Controller Capabilities
偏移 0x0014: Controller Configuration
偏移 0x1000 起: Submission Queue Doorbell
```

如果系统将 BAR0 映射到主机物理地址 `B`，那么 CPU 访问控制器配置寄存器时，实际访问的是：

```text
MMIO address = B + register_offset
```

其中 `B` 由固件或操作系统分配，offset 由设备规范定义。

## 四、操作系统怎样确定 BAR 大小

PCIe 设备在硬件中实现了 BAR 的可写掩码。系统枚举设备时，可以通过经典探测过程确定资源大小：

1. 保存 BAR 原值；
2. 向 BAR 写入全 1；
3. 读回设备实现的地址掩码；
4. 根据最低有效地址位计算窗口大小；
5. 恢复或重新分配 BAR 地址。

假设读回的有效掩码表示低 16 位地址不可配置，则设备要求一个 64 KiB、按 64 KiB 对齐的窗口。

概念上可写为：

```text
size = ~(address_mask) + 1
```

实际处理还要排除 BAR 低位中的类型和属性位，并正确组合 64 位 BAR。

操作系统随后从可用 MMIO 地址区间中分配一段满足大小和对齐要求的地址，把基地址写回 BAR，并设置 PCI Command 寄存器中的 Memory Space Enable。设备才会响应相应 Memory Request。

因此 BAR 同时连接了两个世界：

```text
配置空间中的 BAR 值
          │
          ▼
主机物理地址中的 MMIO 窗口
          │
          ▼
设备内部寄存器或设备内存
```

## 五、BAR 背后映射的资源

BAR 并不限定只能映射几 KB 控制寄存器。它可以对应不同设备资源。

### 1. 控制和状态寄存器

最常见用途包括：

- 启停设备；
- 配置队列地址和长度；
- 查询设备状态；
- 设置中断；
- 更新生产者/消费者索引；
- 敲 doorbell 通知设备有新工作。

这些寄存器通常是 non-prefetchable，因为读取可能有副作用，且访问必须严格到达设备。

### 2. Doorbell 区域

NVMe、网卡和 GPU 常使用 doorbell。驱动先在内存中准备命令或描述符，再通过一次很小的 MMIO Write 通知设备：

```text
Host RAM: command queue prepared
                  │
CPU writes BAR doorbell
                  │
                  ▼
Device observes new queue tail
                  │
Device DMA reads command descriptors
```

Doorbell 的价值是控制面只传递“队列推进到哪里”，而不通过 MMIO 搬运整个数据包。

### 3. 设备本地内存窗口

GPU、FPGA 和某些加速器可以通过 BAR 暴露一部分设备本地内存。CPU 对该窗口访问时，事务会抵达设备内存，而不是主机 DRAM。

传统 GPU BAR 可能只暴露显存的一小段 aperture，需要驱动切换窗口。Resizable BAR 允许系统为设备分配更大的 BAR，在平台资源允许时甚至映射整个显存范围。

但“BAR 足够大”只解决地址可见性问题，不自动保证 CPU 访问显存的带宽、缓存一致性或访问延迟等同于访问 DRAM。

## 六、驱动如何使用 BAR

Linux 驱动通常经历以下过程：

```text
pci_enable_device()
        │
pci_request_regions()
        │
pci_iomap() / ioremap()
        │
readl() / writel()
        │
pci_iounmap()
```

简化示例：

```c
void __iomem *regs;

regs = pci_iomap(pdev, 0, 0);
if (!regs)
    return -ENOMEM;

writel(queue_tail, regs + DOORBELL_OFFSET);
status = readl(regs + STATUS_OFFSET);
```

这里有三个重要点。

第一，`regs` 是内核用于访问 I/O 内存的映射，不能当普通 RAM 指针随意解引用。

第二，应使用 `readl`、`writel` 等 MMIO accessor，以满足架构相关的访问宽度、顺序和屏障要求。

第三，MMIO 通常比普通内存访问昂贵。频繁读取设备寄存器会引入 PCIe Read Request 和 Completion 往返，因此高性能设备倾向于：

- 将描述符和完成项放在内存队列中；
- 使用批处理；
- 减少 MMIO Read；
- 使用少量 posted MMIO Write 敲 doorbell。

## 七、为什么 MMIO Write 通常比 MMIO Read 友好

PCIe Memory Write 通常是 posted request。发送方提交写 TLP 后，不必等待协议层返回数据完成包。请求仍受到流控和错误处理约束，但 CPU 不需要像读取那样等待目标返回内容。

Memory Read 是 non-posted request：

```text
Requester ── Memory Read Request ──> Completer
Requester <── Completion with Data ── Completer
```

访问延迟包含请求路由、设备处理和 Completion 返回。因此在数据路径中反复读取 BAR 状态寄存器非常低效。

现代设备通常让 CPU 写 doorbell，然后通过以下方式获知完成：

- 设备 DMA 写回 completion queue；
- 设备触发 MSI/MSI-X；
- CPU 批量轮询主机内存中的 completion entry。

这也是 SPDK、DPDK 等用户态高性能框架大量使用内存队列和轮询，而不是不断读取设备状态寄存器的原因。

## 八、DMA 是什么

DMA 即 Direct Memory Access。对 PCIe 设备而言，DMA 通常表示设备成为 PCIe Requester，主动对某个地址发起 Memory Read 或 Memory Write。

以网卡接收数据为例：

```text
1. 驱动分配接收缓冲区
2. 驱动将缓冲区映射为 DMA 地址
3. 驱动把 DMA 地址填入 RX descriptor ring
4. 驱动通过 BAR doorbell 通知网卡
5. 网卡 DMA 读取 descriptor
6. 网卡 DMA 把报文写入缓冲区
7. 网卡更新 completion，并触发中断或等待轮询
```

数据路径是：

```text
NIC ── PCIe Memory Write ──> Host RAM
```

CPU 不负责逐字节搬运数据，但 CPU 仍负责创建队列、管理缓冲区、同步所有权和处理完成事件。

### DMA Read 与 DMA Write

从设备视角看：

- DMA Read：设备从目标地址读取数据，例如网卡读取待发送报文；
- DMA Write：设备向目标地址写数据，例如网卡写入收到的报文。

从业务语义看容易产生方向歧义。例如“存储读取”表示应用从 SSD 读取数据，但 NVMe 控制器执行的是向内存 DMA Write。

因此分析时最好明确主语：

```text
NVMe read command
= 应用从 SSD 读取
= NVMe 控制器向目标内存执行 DMA Write
```

## 九、BAR 与 DMA 的关系

BAR 和 DMA 不是同一种机制，也不是互相替代关系。

```text
CPU / Driver ── MMIO through BAR ──> Device
Device       ── DMA               ──> Host memory
```

典型工作流如下：

1. 驱动在主机内存中分配命令队列、描述符和数据缓冲区；
2. 驱动获得这些对象的 DMA 地址；
3. 驱动通过 BAR 寄存器配置队列基地址和长度；
4. CPU 填写新命令；
5. CPU 通过 BAR doorbell 通知设备；
6. 设备 DMA 读取命令和输入数据；
7. 设备 DMA 写回输出数据和完成项；
8. 设备通过 MSI-X 或内存状态通知 CPU。

可以把二者分别看作：

```text
BAR/MMIO = 控制路径
DMA      = 批量数据路径
```

这只是常见设计，而非协议强制。例如 CPU 可以通过设备 BAR 直接读写设备内存，设备也可以 DMA 访问主机内存中的控制结构。

### BAR 地址不能作为普通 DMA 地址使用

BAR 地址的语义是“某段主机物理 MMIO 地址路由到设备资源”；普通 DMA 地址的语义是“设备发起请求时可访问的目标地址”。两者可能都表现为 64 位整数，但归属、映射和生命周期不同。

```text
BAR address
CPU ── Root Complex ──> PCIe device resource

DMA address / IOVA
PCIe device ── IOMMU / Root Complex ──> Host RAM
```

只有在受支持的 peer-to-peer 场景下，一个设备才可能把另一个设备的 BAR 地址作为 DMA 目标，并且还需要 PCIe 拓扑、地址路由、驱动和安全策略共同允许。

## 十、DMA 为什么需要内核映射 API

应用看到的虚拟内存可能：

- 由不连续物理页组成；
- 被换出或迁移；
- 不在设备 DMA mask 可达范围；
- 尚未建立 IOMMU 映射；
- 与 CPU cache 存在一致性要求；
- 生命周期短于设备异步操作。

因此驱动需要通过 DMA API，例如：

```c
void *cpu_addr;
dma_addr_t dma_addr;

cpu_addr = dma_alloc_coherent(dev, size, &dma_addr, GFP_KERNEL);
```

或者对已有内存执行流式映射：

```c
dma_addr = dma_map_single(dev, buffer, size, DMA_TO_DEVICE);
```

两者返回的 `dma_addr` 才是应该写入硬件描述符的地址。

### Coherent DMA 与 Streaming DMA

常见模型包括：

- Coherent DMA：CPU 和设备可持续共享，适合 descriptor ring、completion queue 等控制结构；
- Streaming DMA：为一次或一段时间的数据传输建立映射，适合数据 buffer。

“coherent”并不意味着完全不需要内存屏障。CPU 仍可能需要确保描述符字段在敲 doorbell 前对设备可见，并正确处理设备和 CPU 对队列所有权的切换。

## 十一、IOMMU 如何改变 DMA

没有隔离时，具备总线主控能力的设备可能访问大范围主机物理内存。这既不安全，也不利于虚拟化。

IOMMU 在设备和物理内存之间建立地址翻译与权限检查：

```text
Requester ID + IOVA + access type
                │
                ▼
           IOMMU page table
                │
        translation + permission
                │
                ▼
         Host physical page
```

IOMMU 的主要价值包括：

- 将设备限制在被授权的内存范围内；
- 隔离不同设备、进程和虚拟机；
- 将离散物理页映射成连续 IOVA；
- 支持设备直通；
- 检测非法 DMA 访问。

PCIe Requester ID 通常参与选择 IOMMU domain。SR-IOV 的不同 VF 可以获得独立隔离。

IOMMU 也会引入页表维护、IOTLB miss 和映射操作成本。高性能系统常通过大页、长期映射、批量映射和合理的 IOVA 管理降低开销。

## 十二、一次 NVMe I/O 中 BAR 与 DMA 如何配合

NVMe 是理解二者关系的典型设备。

### 初始化阶段

驱动通常会：

1. 映射 NVMe BAR；
2. 读取控制器能力；
3. 在主机内存中分配 Admin Submission Queue 和 Completion Queue；
4. 将队列 DMA 地址写入控制器寄存器；
5. 启用控制器；
6. 创建 I/O Queue。

### 提交读取请求

```text
CPU prepares NVMe command in Submission Queue
                     │
                     │ command contains data target address
                     ▼
CPU writes SQ tail doorbell through BAR
                     │
                     ▼
NVMe DMA reads command from host memory
                     │
NVMe reads flash media
                     │
NVMe DMA writes data to target memory
                     │
NVMe DMA writes Completion Queue entry
                     │
NVMe triggers MSI-X or host polls completion
```

这里 BAR 承担少量控制交互，DMA 承担命令、完成项和实际数据传输。

## 十三、什么是 PCIe Peer-to-Peer DMA

普通 DMA 的目标通常是主机内存：

```text
Device A ──> Host RAM
```

Peer-to-peer DMA 希望一个设备直接访问另一个设备的资源：

```text
Device A ── PCIe fabric ──> Device B BAR memory
```

例如 NVMe 控制器把读取的数据写入 GPU 显存。GPU 显存通过 GPU BAR 形成 PCIe 可路由地址，NVMe 的 Memory Write TLP 最终由 GPU 接收。

P2P 是否可行取决于多个条件：

- 两个设备是否处在允许 P2P 的拓扑中；
- PCIe Switch 和 Root Complex 是否正确转发 peer request；
- ACS 设置是否强制请求绕行或隔离；
- IOMMU 是否支持所需映射方式；
- 目标设备是否暴露可 DMA 的 BAR 内存；
- 驱动是否能注册、固定并导出目标内存；
- 平台固件是否分配了足够的 MMIO aperture；
- 虚拟化和安全策略是否允许。

因此“设备都插在 PCIe 上”并不意味着天然支持直接互传。

## 十四、GPU 显存与 BAR 的关系

GPU 拥有自己的 VRAM。CPU 若要通过 PCIe 地址访问 VRAM，需要 GPU 将某段显存映射到 BAR aperture。

```text
CPU MMIO address
       │
       ▼
GPU BAR aperture
       │
       ▼
GPU VRAM pages
```

早期系统中，BAR aperture 往往远小于显存容量，驱动需要动态改变窗口映射。启用 64 位 BAR 和 Resizable BAR 后，可以给 GPU 分配更大的 MMIO 窗口。

对于 P2P DMA，关键不是 CPU 是否频繁读写该 BAR，而是 PCIe fabric 中的其他设备是否能把请求路由到 GPU 暴露的显存地址，以及 GPU 驱动能否让特定显存页在传输期间保持稳定并可访问。

## 十五、从传统存储读取到 GDS

假设应用需要从 NVMe SSD 读取数据供 CUDA kernel 使用。

### 传统路径

传统路径通常经过主机页缓存或用户态缓冲区：

```text
NVMe SSD
   │ DMA
   ▼
Host memory / page cache
   │ CPU coordination or copy
   ▼
Pinned host memory
   │ GPU DMA engine
   ▼
GPU VRAM
```

简化代码可能是：

```text
read(file, host_buffer)
cudaMemcpy(gpu_buffer, host_buffer, size, HostToDevice)
```

这条路径存在一些成本：

- 数据先进入主机内存，再传入显存；
- 可能发生额外内存拷贝；
- CPU 参与文件系统、页缓存和传输提交；
- 主机内存带宽被占用两次；
- 大规模并发 I/O 会增加 CPU 和内存子系统压力。

### GPUDirect Storage 路径

GDS 的目标路径是：

```text
NVMe SSD
   │
   │ PCIe DMA / peer-capable data path
   ▼
GPU VRAM
```

从应用角度，数据由存储读取到 GPU buffer，减少主机内存 bounce buffer 和 CPU 参与。

但“直接”需要谨慎理解。GDS 是一个软硬件协同的数据路径，不只是让 NVMe 控制器随意拿到 GPU 虚拟地址。它通常涉及：

- CUDA 分配和管理 GPU memory；
- NVIDIA 驱动固定并导出 GPU 内存映射；
- `nvidia-fs` 等内核组件协调存储驱动与 GPU 驱动；
- `cuFile` 提供用户态文件 I/O API；
- 支持的文件系统、块设备或网络存储路径；
- DMA 映射、拓扑检查、对齐和回退策略。

## 十六、BAR 在 GDS 中扮演什么角色

GDS 与 BAR 的联系可以分成控制面和地址可达性两部分。

### 1. NVMe 仍然通过 BAR 接收命令

即使数据目标变成 GPU 显存，NVMe 控制器仍然需要正常初始化和提交请求：

```text
CPU / NVMe driver
        │
        │ MMIO writes through NVMe BAR
        ▼
NVMe controller doorbell
```

Submission Queue 中包含读取命令和数据目标描述。CPU 写 doorbell 后，NVMe 才开始处理请求。

因此 GDS 并没有消除 BAR。它改变的是数据最终 DMA 到哪里，而不是取消设备控制路径。

### 2. GPU 显存需要形成 PCIe 可访问资源

为了让存储侧 DMA 能够抵达 GPU memory，GPU 驱动必须把目标显存页转换成对发起设备有效的 DMA 映射。底层会涉及 GPU 的 PCIe 地址窗口、页表和驱动导出的 P2P memory 信息。

从概念上看：

```text
CUDA virtual address
        │
        ▼
GPU driver pins VRAM pages
        │
        ▼
PCIe/DMA-visible mappings backed by GPU BAR resources
        │
        ▼
Storage DMA reaches GPU VRAM
```

不能把 CUDA 指针直接当作 BAR 地址或 NVMe DMA 地址。中间的固定、映射、权限和生命周期管理必须由驱动完成。

### 3. BAR 大小会影响设备内存可见性

平台需要为 GPU BAR 分配合适的 MMIO 地址范围。64 位 MMIO aperture、Above 4G Decoding 和 Resizable BAR 等能力会影响大容量设备内存的映射条件。

不过 GDS 能否工作不能只用“是否启用 Resizable BAR”判断。实际支持还依赖 GPU、驱动、CUDA/GDS 版本、存储驱动、文件系统、PCIe 拓扑和平台配置。

## 十七、GDS 的完整控制与数据路径

可以把一次 GDS 读取拆成以下阶段。

### 1. GPU 内存注册

应用准备 GPU buffer：

```text
cudaMalloc() -> CUDA virtual address
```

GDS 相关组件注册或缓存该 buffer 的映射，使 GPU 内存页在 I/O 期间不会失效，并建立存储设备可使用的 DMA 描述。

### 2. 文件和存储路径解析

`cuFile` 处理文件描述符，确认文件系统和设备是否支持目标路径。文件偏移最终需要映射到底层存储块或远端存储请求。

### 3. 构造存储请求

内核存储栈或用户态存储框架构造 NVMe 等设备命令。数据目标不再是普通 host buffer，而是经过注册的 GPU memory DMA mapping。

### 4. BAR doorbell 启动 I/O

驱动更新 Submission Queue，并写 NVMe BAR 中的 doorbell：

```text
CPU ── MMIO Write ──> NVMe BAR doorbell
```

### 5. 设备执行 DMA

NVMe 控制器读取命令，从介质取得数据，并通过可用的 PCIe 路径把数据写入 GPU memory：

```text
NVMe ── PCIe Memory Write ──> GPU memory mapping ──> VRAM
```

### 6. 完成通知

NVMe 写回 Completion Queue，随后通过中断或轮询通知软件。应用在同步完成后才能安全地让 GPU kernel 使用数据。

把整条链路画在一起：

```text
                         Control path
Application / cuFile / driver
           │
           ├── register GPU buffer
           ├── build storage request
           └── write NVMe BAR doorbell
                            │
                            ▼
                         NVMe SSD
                            │
                            │ Data path: DMA
                            ▼
                     GPU BAR / mapping
                            │
                            ▼
                         GPU VRAM
                            │
                            ▼
                       CUDA kernel
```

## 十八、GDS 为什么可能更快

GDS 的收益通常来自减少中间阶段，而不是让 PCIe 链路的物理带宽凭空增加。

### 1. 减少主机内存拷贝

传统路径可能同时消耗：

```text
NVMe -> Host RAM
Host RAM -> GPU VRAM
```

GDS 尽量将其压缩为：

```text
NVMe -> GPU VRAM
```

这样可以减少主机 DRAM 流量，尤其适合多 GPU、多 NVMe 并发读取。

### 2. 降低 CPU 开销

CPU 不再负责大块数据复制，可以把更多周期用于：

- 数据预处理调度；
- 模型执行；
- 元数据管理；
- 网络和存储队列管理。

### 3. 改善流水线并行

应用可将 I/O、解压、预处理和 GPU 计算流水化。在数据集加载、检查点恢复、向量数据库、科学计算等场景中，降低数据到达 GPU 的延迟。

### 4. 降低 Host Memory 带宽争用

大型 GPU 服务器中，CPU、NIC、NVMe 和 GPU 都可能竞争主机内存带宽。绕过不必要的 host bounce buffer 能减轻内存控制器压力。

## 十九、GDS 并非永远完全绕过 CPU 和主机内存

“Direct”不等于完全没有 CPU，也不保证每次 I/O 都走纯 P2P 路径。

CPU 仍然负责：

- API 调用和请求提交；
- 文件系统元数据处理；
- 内存注册与映射管理；
- 错误处理和完成管理；
- 必要的同步。

某些条件下还可能使用兼容或回退路径，例如：

- 文件系统或存储驱动不支持直接路径；
- GPU、NVMe 或拓扑不满足要求；
- I/O 大小、偏移或内存地址不满足对齐要求；
- DMA 映射建立失败；
- 平台 IOMMU/ACS 配置限制 P2P；
- 数据需要经过软件处理。

因此评估 GDS 时，应使用实际工具和指标确认数据路径，而不是仅凭 API 调用成功判断已经绕过主机内存。

## 二十、PCIe 拓扑为什么重要

下面两种拓扑可能有明显差异。

### 同一个 PCIe Switch 下

```text
            PCIe Switch
            ┌─────────┐
            │         │
          NVMe       GPU
```

理论上 peer traffic 可以在 Switch 内部转发，不必上行到 Root Complex。但是否真正这样路由仍受 Switch、ACS 和平台配置影响。

### 跨 Root Complex 或跨 CPU Socket

```text
GPU ── Root Complex A ── CPU interconnect ── Root Complex B ── NVMe
```

请求可能跨 NUMA 节点或根端口，带来更高延迟、较低带宽，甚至不支持 P2P。设备距离不仅是逻辑概念，而会直接影响可达性和性能。

部署高性能 GDS 系统时，应关注：

- GPU 与 NVMe 的 PCIe 层级；
- NUMA node；
- Link speed 和 link width；
- Switch 上行带宽是否过度汇聚；
- ACS 和 IOMMU 状态；
- GPU BAR/MMIO 资源分配；
- 多设备并发下的共享链路瓶颈。

## 二十一、BAR、DMA 和 GDS 的常见误区

### 误区一：BAR 就是一块 DMA 内存

不是。BAR 是设备向系统暴露资源的地址窗口；DMA buffer 是设备被授权访问的目标内存。两者的地址语义和管理方式不同。

### 误区二：CPU 往 BAR 写数据就是 DMA

不是。CPU 对 BAR 的访问属于 CPU 发起的 MMIO。DMA 通常由设备作为 requester 发起。

### 误区三：DMA 完全不需要 CPU

DMA 避免 CPU 搬运数据，但请求准备、映射、同步和完成处理仍需要软件参与。

### 误区四：设备知道进程虚拟地址就能 DMA

不可以。进程虚拟地址必须经过固定和 DMA 映射，转换成设备可用且在生命周期内有效的地址。

### 误区五：GPU 开启大 BAR 就等于支持 GDS

不等于。大 BAR 有利于设备内存地址可见性，但 GDS 还依赖完整的软件栈、存储路径和 PCIe 拓扑支持。

### 误区六：GDS 总能让 NVMe 直接写显存

实际路径可能因平台和请求条件回退。必须结合系统配置和监控工具验证。

### 误区七：P2P 一定比经过主机内存快

如果 P2P 请求需要跨 socket、绕行 Root Complex，或设备间共享窄链路，它可能没有预期收益。性能最终由拓扑、传输大小、并发度和软件开销共同决定。

## 二十二、如何在 Linux 中观察 BAR 和拓扑

### 查看设备及 BAR 资源

```bash
lspci -s 65:00.0 -vv
```

输出中的 Region 类似：

```text
Region 0: Memory at ... [size=16K]
Region 2: Memory at ... [64-bit, prefetchable] [size=32M]
```

它们分别对应设备 BAR 资源。

也可以查看：

```bash
cat /sys/bus/pci/devices/0000:65:00.0/resource
ls -l /sys/bus/pci/devices/0000:65:00.0/resource*
```

### 查看 PCIe 拓扑

```bash
lspci -t
```

结合设备 BDF，可以判断 NVMe 和 GPU 是否位于同一 Switch 或 Root Port 下。

### 查看 NUMA 归属

```bash
cat /sys/bus/pci/devices/0000:65:00.0/numa_node
```

### 查看链路能力和当前状态

```bash
lspci -s 65:00.0 -vv | grep -E 'LnkCap|LnkSta'
```

重点关注链路代际和宽度是否降级，例如设备支持 x16，但实际协商成 x8。

### 查看 NVIDIA GPU 拓扑

```bash
nvidia-smi topo -m
```

它可以辅助判断 GPU、NIC、CPU 和 NUMA 之间的距离。对 NVMe 仍应结合 `lspci -t` 查看完整 PCIe 树。

## 二十三、性能设计中的实用原则

### 1. 控制面和数据面分离

使用 BAR/MMIO 做少量控制，通过内存队列和 DMA 搬运大数据。不要把大块数据逐字节写入 BAR 寄存器窗口。

### 2. 减少 MMIO Read

优先使用 posted write、内存 completion queue、批量轮询和 MSI-X，避免频繁跨 PCIe 往返读取状态。

### 3. 批量提交与 doorbell 合并

一次填写多个描述符后再更新 doorbell，可摊薄 MMIO 和同步成本。但批量过大会增加单请求延迟，需要在吞吐和延迟之间权衡。

### 4. 控制 DMA 映射生命周期

频繁 map/unmap 会增加 IOMMU 和页固定开销。稳定的长生命周期缓冲池通常更适合高 IOPS 数据路径，但必须限制 pinned memory 规模并正确回收。

### 5. 尊重内存顺序

在写 doorbell 前，必须保证设备将读取的描述符已经对设备可见；处理完成项时，也要保证先观察所有权变化，再读取设备写入的数据。

### 6. 先检查拓扑，再优化软件

如果 NVMe 和 GPU 跨 NUMA 节点、链路降级或共享上行过载，仅优化提交代码很难达到目标带宽。

### 7. 用实际数据路径验证 GDS

同时观察：

- 存储吞吐和 IOPS；
- GPU copy engine 与 kernel 利用率；
- CPU 使用率；
- 主机 DRAM 带宽；
- PCIe link throughput；
- GDS 直接路径与兼容路径统计。

只有这些指标一起变化，才能判断瓶颈是否真的被消除。

## 二十四、总结

BAR、DMA 和 GDS 可以串成一条完整的 PCIe I/O 逻辑：

```text
1. PCIe 配置空间中的 BAR 描述设备资源需求
2. 固件或操作系统给 BAR 分配 MMIO 地址
3. 驱动映射 BAR，通过寄存器和 doorbell 控制设备
4. 驱动为内存建立 DMA 映射，把 DMA 地址交给设备
5. 设备作为 PCIe requester 执行 Memory Read/Write
6. IOMMU 可对 DMA 地址进行翻译和隔离
7. P2P 允许设备访问另一个设备暴露的 PCIe 地址资源
8. GDS 将 GPU 内存注册、存储请求和 P2P/DMA 能力组合起来
9. 数据尽量从存储设备直接进入 GPU VRAM
```

最终可以用三句话概括：

> BAR 解决 CPU 如何找到并控制设备。
>
> DMA 解决设备如何高效访问目标内存。
>
> GDS 解决存储设备如何在驱动和拓扑允许时，把数据更直接地送到 GPU 显存。

理解这三者后，再分析 NVMe、RDMA、GPU Direct、SPDK 或用户态设备驱动时，就能清楚地区分配置空间、MMIO 控制路径、DMA 数据路径与设备间 P2P 路径，而不会把几个看起来都是“地址”的概念混为一谈。
