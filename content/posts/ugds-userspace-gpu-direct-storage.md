---
title: "uGDS 原理解析：在用户态打通 NVMe SSD 与 GPU 显存"
date: 2026-08-20T13:55:00+08:00
draft: false
tags: ["uGDS", "GPU Direct Storage", "NVMe", "CUDA", "ROCm", "PCIe"]
categories: ["技术"]
---

大模型推理正在把存储重新推到系统性能的核心位置。模型权重、KV Cache、Embedding 和训练检查点都可能在 SSD、主存与 GPU 显存之间频繁搬运。传统路径通常要经过文件系统、内核块层、页缓存或用户态中转缓冲区，不但增加 CPU 开销，也让小块 I/O 延迟变得难以控制。

[uGDS](https://github.com/ScaleX-IO/uGDS) 是 ScaleX-IO 开源的一套用户态 GPU Direct Storage 开发库。它的核心思路很直接：

> 让 CPU 在用户态构造 NVMe 命令并管理提交队列与完成队列，由 SSD 通过 PCIe P2P DMA 直接读写 GPU 显存，从数据路径中绕开内核 NVMe 驱动和文件系统。

本文结合 uGDS 当前源码，解释它为什么快、一次 I/O 如何执行、内核模块与用户态库如何分工，以及使用这类“绕过内核”的存储栈时必须承担哪些工程责任。

## 一、uGDS 想解决什么问题

先看一条常见的数据读取路径：

```text
NVMe SSD
   │
   ▼
内核 NVMe 驱动 → 块层 → 文件系统
   │
   ▼
主机内存缓冲区
   │
   ▼
GPU 显存
```

即使系统支持 Direct I/O，应用仍然通常要进入内核，由内核驱动准备 NVMe 命令、完成 DMA 映射并处理中断或轮询。若数据还要经过主机内存，再复制到 GPU，路径会更长。

NVIDIA GPUDirect Storage 已经能够缩短这条路径，让存储设备直接访问 GPU 内存。但 uGDS 继续向下推进了一步：不仅绕过主机内存，还把 NVMe 快路径放到用户态。

它希望消除的主要成本包括：

- 文件系统和通用块层带来的软件栈开销；
- 系统调用与用户态、内核态切换；
- 通用内核驱动为了兼容性引入的锁和调度路径；
- 小 I/O 场景中尤其明显的固定延迟；
- CPU 参与数据复制带来的带宽和缓存污染。

因此，uGDS 并不是一个新的文件系统，也不是一个普通的异步文件 I/O 库。它更接近一套面向 GPU 工作负载的用户态 NVMe 驱动与 P2P DMA 运行时。

## 二、整体架构：控制面留在内核，数据面搬到用户态

uGDS 的架构可以简化为：

```text
┌────────────────────────────────────┐
│ Application                        │
│ uGDSRead / uGDSWrite / Batch API   │
└────────────────┬───────────────────┘
                 │
┌────────────────▼───────────────────┐
│ libugds.so                         │
│ GPU Buffer 注册                    │
│ NVMe SQ/CQ 管理                    │
│ NVMe 命令构造与完成轮询            │
└────────────────┬───────────────────┘
                 │ ioctl + mmap
┌────────────────▼───────────────────┐
│ ugds_drv.ko                        │
│ PCI BAR 映射                       │
│ GPU 页固定与 DMA 地址导出          │
│ MSI-X / eventfd 中断注册           │
└────────────────┬───────────────────┘
                 │
          PCIe P2P DMA
                 │
        ┌────────▼────────┐
        │ NVMe SSD ↔ GPU  │
        └─────────────────┘
```

这里有一个容易误解的地方：**用户态 I/O 并不意味着完全不需要内核。**

应用没有权限自行完成 PCI 设备 BAR 映射、锁定 GPU 页面、获得 DMA 总线地址或配置 MSI-X。uGDS 仍然需要一个小型内核模块处理这些特权操作，但稳态 I/O 不再逐次进入内核。

这种设计把系统拆成两个部分：

- **控制面**：由 `ugds_drv.ko` 建立设备映射、内存映射和中断通道；
- **数据面**：由 `libugds.so` 在用户态提交 NVMe 命令、敲 doorbell 并等待完成。

完成初始化后，一次普通读写不需要经过文件系统、内核 NVMe 驱动和块层。

## 三、一次 uGDS I/O 是怎样执行的

uGDS 暴露的接口风格接近 cuFile。基本使用流程如下：

```cpp
uGDSDriverOpen();

int fd = open("/dev/ugds_drv0", O_RDWR);
uGDSDescr_t desc = {
    .type = UGDS_HANDLE_TYPE_OPAQUE_FD,
    .handle.fd = fd,
};

uGDSHandle_t handle;
uGDSHandleRegister(&handle, &desc);

void* gpu_buffer;
cudaMalloc(&gpu_buffer, 4096);
uGDSBufRegister(gpu_buffer, 4096, 0);

uGDSRead(handle, gpu_buffer, 4096, 0, 0);
uGDSWrite(handle, gpu_buffer, 4096, 0, 0);
```

这几步背后分别发生了什么？

### 1. 打开驱动并初始化全局状态

`uGDSDriverOpen()` 建立用户态库的全局状态。库内部维护设备句柄、已注册缓冲区、队列和并发保护结构。

### 2. 注册 NVMe 设备句柄

应用打开 `/dev/ugds_drv0`，然后调用 `uGDSHandleRegister()`。注册过程会映射 NVMe 控制器的 PCI BAR，并初始化控制器管理队列与 I/O 队列。

NVMe 的队列本质上是位于内存中的环形数组：

- SQ，Submission Queue，保存待执行命令；
- CQ，Completion Queue，保存设备写回的完成项。

用户态库获得 BAR 中的 doorbell 寄存器映射后，就能直接通知控制器“SQ 中出现了新命令”。

### 3. 注册 GPU 缓冲区

`uGDSBufRegister()` 不是简单地记住一个指针。GPU 虚拟地址不能直接交给 NVMe 控制器，必须先转换为设备可用于 DMA 的总线地址。

注册阶段大致完成：

1. 确认缓冲区所属后端与地址范围；
2. 固定 GPU 内存，防止映射在 I/O 中途失效；
3. 获取 GPU 页对应的 DMA 地址；
4. 建立虚拟地址到 DMA 页列表的映射；
5. 把结果保存到用户态缓冲区注册表。

这也是为什么缓冲区注册通常应该被复用，而不是每次 I/O 前注册、结束后立即注销。注册属于慢路径，数据传输才是快路径。

### 4. 构造 NVMe 命令

调用 `uGDSRead()` 时，uGDS 根据 SSD 偏移、长度和 GPU 缓冲区偏移构造 NVMe Read 命令。命令中的 PRP 等数据指针最终指向 GPU 显存对应的 DMA 地址，而不是主机内存。

数据方向为：

```text
uGDSRead:  SSD ──DMA──> GPU VRAM
uGDSWrite: GPU VRAM ──DMA──> SSD
```

CPU 只负责准备描述符、更新 SQ tail、写 doorbell，以及检查 CQ。真正的数据不会流经 CPU Cache。

### 5. 等待完成

默认模式下，uGDS 使用 busy polling 检查 CQ，并在循环中使用 `_mm_pause()` 降低自旋对流水线和超线程的干扰。

为了减少一个控制器上的队列争用，库支持多个 I/O queue pair，并以轮询方式选择队列。同步 API 会一直等到对应 CQE 出现，再返回传输结果。

## 四、为什么用户态 NVMe 能降低延迟

uGDS 的性能优势主要不是来自某条神奇指令，而是来自路径缩短和专用化。

### 1. 去掉通用软件栈

传统内核路径需要兼顾大量设备、文件系统、进程和调度策略。uGDS 面向明确的 NVMe 到 GPU 场景，可以直接操作队列，减少中间层。

### 2. 避免每次 I/O 的系统调用

队列、doorbell 和 DMA 映射准备好以后，命令提交与完成处理都在当前进程中完成，不必为每个请求进入内核。

### 3. 轮询比中断更适合低延迟

中断可以节省 CPU，但会引入中断投递、调度和唤醒延迟。对于持续高负载或 4KB 小 I/O，专用 CPU 核轮询 CQ 往往能得到更稳定的尾延迟。

### 4. 数据直接到达最终位置

SSD 直接 DMA 到 GPU 显存，省去了主机内存中转和额外复制，也减少了内存带宽消耗。

项目 README 给出的测试结果中，在 A100 40GB 与 Samsung 990 PRO 的环境下，uGDS 相比 NVIDIA GDS 展示了更高的带宽和明显更低的 4KB 延迟。需要注意，这些数字来自项目方的特定硬件、拓扑和测试方法，不能直接外推到所有机器；PCIe 拓扑、SSD 型号、GPU BAR、NUMA 位置、IOMMU 配置和队列深度都会影响结果。

## 五、轮询模式与中断模式

Busy polling 的代价是会持续占用 CPU。为了兼顾大块 I/O 和低 CPU 利用率，uGDS 还提供可选的中断模式：

```bash
export UGDS_INTERRUPT_MODE=1
```

内核模块支持将 NVMe MSI-X vector 绑定到 `eventfd`。用户态可以等待 `eventfd`，收到通知后再处理 CQ。

两种模式没有绝对优劣：

| 模式 | 优点 | 代价 | 适合场景 |
|---|---|---|---|
| Busy polling | 延迟低、抖动小 | 占用 CPU 核 | 高频小 I/O、追求尾延迟 |
| MSI-X + eventfd | CPU 占用低 | 唤醒路径更长 | 大块 I/O、吞吐优先、请求稀疏 |

在生产系统中，常见做法是把轮询线程固定到独立 CPU 核，并确保它与 GPU、NVMe 位于合适的 NUMA 节点。

## 六、CUDA 与 ROCm 双后端如何统一

uGDS 同时支持 NVIDIA CUDA 与 AMD HIP/ROCm，但两者获取 GPU DMA 映射的机制并不完全相同。

### CUDA 路径

CUDA 后端要求 NVIDIA 开放内核模块，并通过 NVIDIA P2P 页面固定和映射能力，让 NVMe 控制器访问 GPU 内存。项目也支持将合适的 CUDA 缓冲区导出为 DMA-buf。

### HIP/ROCm 路径

AMD 后端以 Linux DMA-buf 为核心：

1. 用户态通过 HSA Runtime 导出 GPU 内存的 DMA-buf；
2. 内核模块使用标准 DMA-buf attach、pin 和 map API；
3. 获得 NVMe 控制器可访问的 DMA 地址；
4. 用户态继续使用相同的 uGDS I/O 接口。

这层抽象让上层 API 保持一致，但部署时必须保证内核模块和用户态库启用了相同后端。双后端环境中，还应使用显式后端参数注册缓冲区，避免仅凭指针自动判断带来的歧义。

## 七、DMA-buf 与 RDMA：让一块显存连接更多设备

uGDS 不只服务 NVMe。注册的 GPU 缓冲区还可以通过 `uGDSExportDmabuf()` 导出 DMA-buf 文件描述符，交给支持 DMA-buf 的其他子系统，例如 RDMA 或 DPDK。

这意味着一块 GPU 缓冲区可能同时处于以下数据路径中：

```text
NVMe SSD ─┐
          ├── PCIe / DMA-buf ── GPU VRAM
RDMA NIC ─┘
```

它为分布式推理中的“SSD 缓存层 + 网络缓存层 + GPU 计算层”提供了很有价值的组合空间。不过，零拷贝不会自动解决同步问题。

可以把访问者分成两类：

- **Producer**：向显存写数据，例如 NVMe Read、RDMA Recv、GPU Kernel Write；
- **Consumer**：从显存读数据，例如 NVMe Write、RDMA Send、GPU Kernel Read。

两个 Consumer 可以并发读取同一区域，但 Producer 与任何其他访问者发生重叠时，都可能形成数据竞争。应用必须通过 NVMe completion、RDMA CQ 或 CUDA/HIP stream barrier 明确建立先后关系。

例如，SSD 把一批 KV Cache 读入 GPU 后，不能仅凭“命令已经提交”就启动依赖这些数据的 Kernel，而要等到对应 I/O 完成；同样，GPU 刚写完的缓冲区要落盘，也需要先同步相关 stream。

## 八、同步、异步与批量 API

uGDS 提供的不只是同步 `uGDSRead()` 和 `uGDSWrite()`。源码中还包含异步、批量和多 stream 相关实现与测试。

这些 API 的价值在于把三类工作重叠起来：

```text
时间 ──────────────────────────────────────────>

CPU:   构造下一批命令 ───── 构造下一批命令
SSD:       DMA Batch A ─────── DMA Batch B
GPU:              Compute A ─────── Compute B
```

同步接口简单，但每个调用都要等完成；批量接口可以一次提交多条命令，摊薄 doorbell 和软件开销；异步接口则允许存储 I/O 与 GPU Kernel 重叠。

对大模型系统而言，这种重叠通常比单纯追求某个微基准峰值更重要。只有把预取、计算和回写组织成流水线，额外的存储带宽才能真正转化为 token throughput 或训练吞吐。

## 九、uGDS 的工程边界与风险

用户态存储栈获得性能的同时，也接管了原本由内核负责的大量工作。

### 1. 它操作的是块设备，不是普通文件

示例打开的是 `/dev/ugds_drv0`。I/O 使用设备偏移，不会替你解析目录、inode、extent 或文件增长。应用若需要“按文件读取”，必须自己管理数据布局和元数据，或者由上层缓存系统提供这层抽象。

### 2. NVMe 设备不能同时由内核驱动使用

同一控制器通常不能一边交给常规内核 NVMe 驱动挂载文件系统，一边由 uGDS 直接接管。部署前必须明确设备归属，并防止误挂载或其他进程访问。

### 3. 错误处理与隔离更困难

用户态驱动需要正确处理队列深度、超时、控制器错误、并发访问和进程异常。普通文件系统提供的权限、崩溃恢复和一致性语义，在裸块设备路径中都不是免费获得的。

### 4. 硬件拓扑决定 P2P 是否成立

GPU 与 NVMe 最好位于同一 PCIe Root Complex。某些 PCIe Switch、ACS 配置、IOMMU 模式或虚拟化环境会阻断或绕行 P2P DMA。如果平台不支持，数据可能无法直达，甚至无法建立映射。

### 5. GPU 内存必须正确注册和同步

缓冲区注销前必须确保没有在途 I/O，也不能仍被 RDMA MR 引用。越过这些生命周期约束，轻则返回错误，重则可能造成 DMA 访问已失效内存。

### 6. 当前项目仍在快速演进

截至 2026 年 8 月 20 日，uGDS 的公开路线图仍包含更多 cuFile API、更多 NVMe 能力与进一步性能优化。适合评估和集成的团队应固定版本、补充故障测试，并在目标硬件上验证，而不是只根据 README 的峰值数字做容量规划。

## 十、安装与部署前需要检查什么

uGDS 对环境要求较高。以 CUDA 后端为例，至少需要：

- Linux 内核头文件与正在运行的内核匹配；
- NVIDIA 开放内核模块，而不是闭源内核模块；
- CUDA 12 或更新版本；
- CMake 3.18 或更新版本；
- 支持 C++17 的编译器。

AMD 后端还依赖较新的 ROCm、HSA DMA-buf 导出能力、Large BAR，以及内核中的 PCI P2PDMA 和 DMA-buf move notify 配置。

真正部署前，建议按下面的顺序验证：

1. 用 `lspci -tv` 确认 GPU 与 NVMe 的 PCIe 拓扑；
2. 检查 IOMMU、ACS、Resizable BAR 或 Large BAR 配置；
3. 确认目标 NVMe 没有挂载文件系统且未被内核业务使用；
4. 构建并加载与 GPU 后端匹配的 `ugds_drv.ko`；
5. 先运行项目功能测试，再运行性能测试；
6. 分别测试 4KB、小队列深度、大块顺序 I/O 和真实业务流水线；
7. 监控 CPU 核占用、PCIe 带宽、SSD 温度与持续写入降速。

## 十一、它适合哪些场景

uGDS 最适合同时具备以下特征的系统：

- 数据最终消费位置就是 GPU 显存；
- 数据布局可控，能够使用裸设备或专用分区；
- 对 4KB 延迟、尾延迟或 CPU 开销敏感；
- 愿意为固定硬件平台做拓扑和驱动调优；
- 上层已经有缓存目录、对象索引或元数据服务。

典型场景包括：

- 大模型 KV Cache 的 SSD 分层与预取；
- 模型权重按需加载；
- GPU 数据处理流水线；
- 向量检索或图计算的数据分页；
- GPU、NVMe 与 RDMA NIC 之间的零拷贝数据通路。

如果应用主要读写普通文件、强调通用性与运维简单，或者硬件 P2P 条件不可控，那么成熟文件系统加 GDS、io_uring 或常规 Direct I/O 可能是更稳妥的方案。

## 十二、总结

uGDS 的关键价值不只是“SSD 可以直接把数据写进显存”，而是把 NVMe 命令提交与完成处理也搬到了用户态：

1. 内核模块只负责设备映射、GPU 页固定和中断等特权控制面；
2. 用户态库直接管理 NVMe SQ/CQ 并敲 doorbell；
3. NVMe 控制器通过 PCIe P2P DMA 直接访问 GPU 显存；
4. CUDA、ROCm、DMA-buf 和 RDMA 被统一到同一套缓冲区生命周期中；
5. 应用用更多 CPU 核、部署约束和工程复杂度，换取更短的数据路径与更可控的延迟。

从系统设计角度看，uGDS 很好地展示了一条趋势：当 GPU 成为主要计算中心后，存储系统不再只是“把数据读进内存”，而需要围绕 GPU 的地址空间、执行流和 PCIe 拓扑重新设计。

## 系列导航

- [GDS、GIDS 与 uGDS 有什么区别：从数据直达、GPU 发起到用户态 NVMe](/posts/gds-gids-ugds-comparison/)
- [NVIDIA GPUDirect Storage（GDS）详解](/posts/nvidia-gpudirect-storage-gds/)
- [NVIDIA GPU-Initiated Data Storage（GIDS）详解](/posts/nvidia-gpu-initiated-data-storage-gids/)
- [uGDS 原理解析](/posts/ugds-userspace-gpu-direct-storage/)

## 参考资料

- [ScaleX-IO/uGDS](https://github.com/ScaleX-IO/uGDS)
- [CoPilotIO: CPU as a Co-pilot for GPU I/O to Free GPU Compute](https://www.usenix.org/conference/osdi26/presentation/chen-guanyi)
- [NVIDIA GPUDirect Storage](https://docs.nvidia.com/gpudirect-storage/)
- [Linux PCI Peer-to-Peer DMA Support](https://docs.kernel.org/driver-api/pci/p2pdma.html)
- [Linux DMA-BUF](https://docs.kernel.org/driver-api/dma-buf.html)
