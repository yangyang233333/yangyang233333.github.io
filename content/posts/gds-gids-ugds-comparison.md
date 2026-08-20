---
title: "GDS、GIDS 与 uGDS 有什么区别：从数据直达、GPU 发起到用户态 NVMe"
date: 2026-08-20T14:05:00+08:00
draft: false
tags: ["GDS", "GIDS", "uGDS", "GPUDirect Storage", "NVMe", "GPU 存储"]
categories: ["技术"]
---

GDS、GIDS 和 uGDS 的名字非常接近，也都在讨论 GPU 与存储之间的数据通路，因此很容易被理解成同一项技术的三个版本。

实际上，它们解决的是三个不同层面的问题：

- **GDS** 关注数据路径：怎样让存储数据不经 CPU 内存中转，直接进入 GPU 显存；
- **GIDS** 关注控制路径：怎样让 GPU Kernel 自己决定并发起存储请求，减少 GPU 与 CPU 往返；
- **uGDS** 关注软件栈：怎样让 CPU 在用户态直接管理 NVMe 队列，绕过内核 NVMe 驱动与文件系统。

可以先用一句话概括：

> GDS 是“数据直达 GPU”，GIDS 是“GPU 主动要数据”，uGDS 是“用户态 CPU 直接驱动 NVMe 把数据送到 GPU”。

本文从请求由谁发起、数据经过哪里、NVMe 命令由谁构造、是否保留文件系统语义等维度，分析三者的区别与联系。更深入的原理可以继续阅读本系列的三篇文章：

- [NVIDIA GPUDirect Storage（GDS）详解：让存储数据绕过 CPU 直达 GPU](/posts/nvidia-gpudirect-storage-gds/)
- [NVIDIA GPU-Initiated Data Storage（GIDS）详解：让 GPU Kernel 主动访问存储](/posts/nvidia-gpu-initiated-data-storage-gids/)
- [uGDS 原理解析：在用户态打通 NVMe SSD 与 GPU 显存](/posts/ugds-userspace-gpu-direct-storage/)

## 一、先区分数据路径与控制路径

理解这三项技术的关键，是不要把“数据经过哪里”和“谁发起 I/O”混为一谈。

### 数据路径

数据路径描述有效载荷如何移动。例如：

```text
传统路径：SSD → CPU 内存 → GPU 显存
直接路径：SSD ──────────→ GPU 显存
```

GDS、GIDS 和 uGDS 都希望建立 SSD 与 GPU 显存之间的 DMA 路径，减少 CPU bounce buffer。但“数据不经过 CPU 内存”并不代表“CPU 没有参与”。

### 控制路径

控制路径描述谁决定读取什么数据，以及谁生成和提交 I/O 请求：

```text
CPU 发起：Application CPU Thread → I/O Stack → SSD
GPU 发起：GPU Kernel → Device-side Queue → I/O Service → SSD
```

经典 GDS 和 uGDS 仍由 CPU 侧代码发起 I/O；GIDS 的核心目标则是让 GPU Kernel 成为请求生产者。

### 软件栈路径

软件栈路径描述请求是否经过文件系统、内核 NVMe 驱动和通用块层：

```text
GDS：  Application → cuFile → 文件系统/内核存储栈 → NVMe
uGDS： Application → libugds → 用户态 NVMe SQ/CQ → NVMe
```

uGDS 的主要差异就在这一层。它不仅优化 SSD 到 GPU 的数据路径，还把 NVMe 命令构造、提交队列和完成队列管理移到了用户态。

## 二、GDS：优化存储到 GPU 的数据路径

NVIDIA GPUDirect Storage，简称 GDS，是一套面向 CUDA 应用的直接存储访问技术。应用在 CPU 上调用 `cuFileRead`、`cuFileWrite` 等接口，把文件数据读入 GPU 缓冲区或从 GPU 缓冲区写回文件。

典型路径如下：

```text
CPU Application
      │ cuFileRead
      ▼
cuFile / nvidia-fs / 文件系统
      │
      ▼
NVMe 或并行文件系统 ──DMA──> GPU VRAM
```

GDS 改变的是数据搬运路径：

- 数据可以绕过 CPU 内存中的中转缓冲区；
- CPU 内存带宽和 Cache 压力降低；
- 应用不再需要手动维护 host staging buffer；
- 存储读取可以与 GPU 计算形成流水线。

但 GDS 没有改变请求发起者。通常仍然是 CPU 线程计算文件偏移、调用 cuFile API，并等待或轮询完成。

因此，GDS 最适合以下情况：

- 数据以文件形式管理；
- 请求可以由 CPU 提前规划和批量提交；
- 需要兼容成熟文件系统、并行文件系统和存储语义；
- 希望降低数据复制成本，但不想自己接管 NVMe 控制器。

详细原理、API 与部署方式参见：[NVIDIA GPUDirect Storage（GDS）详解](/posts/nvidia-gpudirect-storage-gds/)。

## 三、GIDS：优化 GPU 与存储之间的控制路径

GPU-Initiated Data Storage，简称 GIDS，关注的问题是：如果下一次访问的数据只有 GPU Kernel 在运行时才知道，为什么还要把索引交给 CPU，再由 CPU 发起 I/O？

经典 CPU 发起流程可能是：

```text
GPU Kernel 生成索引
        │ synchronize
        ▼
CPU 读取索引并构造 I/O
        │
        ▼
数据进入 GPU
        │ launch
        ▼
下一个 GPU Kernel
```

GIDS 希望把它变成：

```text
GPU Kernel
   │ 生成 I/O 请求
   ▼
Device-side Request Queue
   │
   ▼
存储服务层 ──DMA──> GPU VRAM
   │
   ▼
GPU Kernel 继续消费数据
```

它改变的是请求生产者和控制路径：

- GPU 线程能够产生存储请求；
- 减少 Kernel、CPU 线程和存储服务之间的同步往返；
- 支持图遍历、稀疏计算、向量检索等数据依赖型访问；
- 更适合运行期间才知道访问地址的细粒度 I/O。

GIDS 不等于“异步 GDS”。异步 GDS 仍是 CPU 调用异步 API，只是调用后不阻塞；GIDS 则是请求本身由 GPU Kernel 产生。

GIDS 也不是简单地让每个 GPU 线程直接操作 NVMe doorbell。GPU 可能同时产生海量小请求，需要设备侧队列、请求合并、缓存、背压、完成通知和错误处理。真正的系统通常会在 GPU 请求与底层存储之间设置软件或硬件服务层。

因此，GIDS 更适合：

- 图计算中的指针追踪和邻接表加载；
- 向量数据库中的数据依赖型候选扩展；
- 超大 Embedding 表与稀疏模型；
- GPU 主导的 out-of-core 算法；
- 无法由 CPU 提前预测的细粒度访问。

详细架构与工程挑战参见：[NVIDIA GPU-Initiated Data Storage（GIDS）详解](/posts/nvidia-gpu-initiated-data-storage-gids/)。

## 四、uGDS：优化 CPU 到 NVMe 的软件栈路径

uGDS 是 ScaleX-IO 开源的用户态 GPU Direct Storage 库。它同样让 NVMe SSD 通过 PCIe P2P DMA 直接读写 GPU 显存，但请求仍由 CPU 侧应用发起。

它与经典 GDS 最重要的不同是：uGDS 绕过了内核 NVMe 驱动和文件系统，由用户态库直接管理 NVMe SQ/CQ。

```text
CPU Application
      │ uGDSRead / uGDSWrite
      ▼
libugds.so
      │ 构造 NVMe Command
      │ 更新 SQ Tail
      │ 写 Doorbell
      │ 轮询 CQ
      ▼
NVMe SSD ──PCIe P2P DMA──> GPU VRAM
```

uGDS 仍然需要一个小型内核模块完成特权控制面工作，例如：

- 映射 NVMe PCI BAR；
- 固定 GPU 页面并获取 DMA 地址；
- 建立 DMA-buf 映射；
- 注册 MSI-X 与 eventfd 中断。

但完成初始化后，稳态读写不必逐次经过系统调用、文件系统、块层和内核 NVMe 快路径。

这种设计能够降低固定软件延迟，尤其适合 4KB 小 I/O、专用裸设备和可控硬件拓扑，但代价也很明显：

- 应用面对的是块偏移，而不是普通文件；
- NVMe 控制器通常需要由 uGDS 独占；
- 数据布局、元数据、隔离和恢复需要上层负责；
- PCIe P2P、IOMMU、ACS、Large BAR 等平台条件必须满足；
- 用户态系统需要自行处理队列、错误、超时和生命周期。

uGDS 当前同时支持 CUDA 与 AMD HIP/ROCm 后端，还能导出 DMA-buf 给 RDMA 等设备使用。

详细源码分析参见：[uGDS 原理解析：在用户态打通 NVMe SSD 与 GPU 显存](/posts/ugds-userspace-gpu-direct-storage/)。

## 五、三者的核心区别

| 对比维度 | GDS | GIDS | uGDS |
|---|---|---|---|
| 核心目标 | 数据绕过 CPU 内存 | GPU Kernel 主动产生 I/O | 绕过内核 NVMe 与文件系统 |
| 请求发起者 | CPU 应用线程 | GPU Kernel | CPU 应用线程 |
| 数据终点/起点 | GPU 显存 | GPU 显存 | GPU 显存 |
| 数据是否必须经 CPU 内存 | 通常不需要 | 通常不需要 | 不需要 |
| NVMe 命令主要由谁管理 | 内核与 GDS 软件栈 | 取决于具体实现 | 用户态 CPU 库 |
| 是否经过文件系统 | 可以，且常见 | 取决于实现 | 不经过 |
| 对外接口层次 | 文件 I/O API | 设备侧请求 API | 块设备偏移 API |
| CPU 是否参与控制 | 是 | 尽量减少 | 是 |
| 是否属于 NVIDIA 专有技术方向 | 是 | NVIDIA 推动的技术方向 | 否，BSD 开源项目 |
| GPU 支持 | NVIDIA CUDA | 主要面向 NVIDIA GPU | NVIDIA CUDA、AMD ROCm |
| 主要优势 | 成熟文件语义与生态 | 消除细粒度 CPU 控制往返 | 极短用户态 NVMe 路径 |
| 主要代价 | 仍有 CPU 控制开销 | 系统复杂、缓存与调度困难 | 裸设备、独占与运维复杂 |

这张表中最值得记住的是：**uGDS 并不是 GIDS。**

uGDS 名字里的 `u` 指的是 user space。它让 CPU 在用户态发起 NVMe I/O；GIDS 则让 GPU 发起 I/O。两者优化的是不同方向。

## 六、用三条路径理解三者

### GDS：CPU 发起，内核存储栈执行，数据直达 GPU

```text
控制路径：CPU → cuFile → Kernel Storage Stack → NVMe
数据路径：NVMe ───────────────────────────→ GPU
```

关键词是：**CPU 发起、文件语义、直接数据路径。**

### GIDS：GPU 发起，服务层调度，数据返回 GPU

```text
控制路径：GPU Kernel → Device Queue → I/O Service → NVMe
数据路径：NVMe ───────────────────────────────→ GPU
```

关键词是：**GPU 发起、动态访问、减少控制往返。**

### uGDS：CPU 发起，用户态直接驱动 NVMe，数据直达 GPU

```text
控制路径：CPU → libugds → NVMe SQ/CQ
数据路径：NVMe ─────────────────→ GPU
```

关键词是：**CPU 发起、用户态 NVMe、裸块设备。**

## 七、它们是替代关系还是组合关系

三者并不完全处于同一层，因此不能简单地说谁“替代”谁。

### GDS 与 uGDS：同类数据目标，不同软件栈选择

二者都可由 CPU 发起 SSD 与 GPU 显存之间的直接传输，但工程定位不同：

- GDS 更强调文件系统、CUDA 生态和通用部署；
- uGDS 更强调用户态 NVMe、裸设备和极低固定延迟。

如果系统需要读取普通文件、共享存储或并行文件系统，GDS 更自然；如果 SSD 是专用缓存盘，上层能自行管理对象到 LBA 的映射，uGDS 更有发挥空间。

### GIDS 可以构建在不同数据面之上

GIDS 描述的是 GPU 发起控制模型，不强制规定底层一定使用哪一种存储数据面。理论上，设备侧请求经过聚合和调度后，可以落到：

- GDS 或类似的文件数据路径；
- 用户态 NVMe 数据路径；
- GPU 文件系统或专用存储服务；
- 带缓存的分层存储运行时。

因此可以把 GIDS 看成更靠近应用和 GPU Kernel 的控制面，而 GDS、uGDS 更靠近具体数据搬运与存储栈。

### CPU 仍可能是 GPU I/O 的协处理器

即使采用 GPU 发起模式，也不意味着 CPU 必须完全消失。CPU 很擅长：

- 合并大量细粒度请求；
- 管理 NVMe 队列与完成项；
- 处理异常和复杂控制流；
- 执行缓存替换与元数据操作。

一种实际架构是 GPU 产生请求，CPU 用户态服务线程负责批处理和提交，SSD 再直接 DMA 到 GPU。这同时吸收了 GIDS 的请求模型和 uGDS 的用户态 NVMe 路径。

## 八、性能对比不能只看带宽

选择技术时，不能只比较顺序读带宽。至少需要观察四类指标。

### 1. 数据带宽

大块顺序 I/O 主要受 SSD、PCIe 链路和 GPU DMA 能力限制。只要能建立直接路径，三种架构都可能接近硬件上限。

### 2. 单次 I/O 延迟

uGDS 通过用户态队列和 busy polling 减少软件路径，可能在小 I/O 上取得明显优势。GDS 的延迟还包含文件系统与内核存储栈成本。

### 3. 请求生成与控制往返

当访问地址由 GPU 动态产生时，GIDS 的优势不一定表现为单条 NVMe 延迟更低，而是减少 GPU—CPU 同步、Kernel relaunch 和控制信息搬运。

### 4. 端到端计算吞吐

最重要的指标通常是业务吞吐：训练 step time、推理 token throughput、图遍历速度或向量查询 QPS。存储微基准更快，不代表计算流水线一定更快；预取距离、缓存命中率、批量大小和计算重叠都会决定最终结果。

## 九、怎样选择

### 选择 GDS，如果你需要

- 使用普通文件或成熟并行文件系统；
- 在 CUDA 生态中获得生产级直接存储路径；
- 由 CPU 提前规划和批量提交 I/O；
- 保留现有权限、文件布局和运维方式；
- 避免自行接管裸 NVMe 控制器。

### 研究或采用 GIDS，如果你需要

- GPU Kernel 运行期间才知道下一次访问位置；
- 处理图、稀疏模型、Embedding 或向量检索等动态访问；
- 减少高频 GPU—CPU 控制往返；
- 构建 GPU 主导的 out-of-core 执行模型；
- 能够接受设备侧队列、缓存和调度系统的复杂度。

### 选择 uGDS，如果你需要

- 在专用 NVMe 上追求低延迟或高小 I/O 吞吐；
- 上层已经有对象索引、缓存目录或元数据服务；
- 能够使用裸设备并管理 LBA 布局；
- 希望同时支持 CUDA 与 ROCm；
- 能控制 PCIe 拓扑、IOMMU、CPU 绑核和设备归属。

## 十、一个大模型 KV Cache 分层示例

假设推理系统把冷 KV Cache 保存到本地 NVMe，热数据保留在 GPU：

### 使用 GDS

CPU 调度器判断需要恢复哪些 KV block，通过文件偏移调用 cuFile，把数据读回 GPU。优点是能使用文件与成熟存储生态，系统集成简单。

### 使用 uGDS

CPU 调度器维护 KV block 到 SSD LBA 的映射，通过用户态 NVMe 队列直接把数据 DMA 到 GPU。它可能降低小块读取延迟，但需要专用 SSD 和自定义元数据层。

### 使用 GIDS

如果 GPU 侧推理 Kernel 在执行中才能确定下一批稀疏 KV block，Kernel 可以直接产生缺页或读取请求。后端服务将请求合并后，再通过某种直接数据路径取回。这里的底层数据面既可能类似 GDS，也可能使用 uGDS 式用户态 NVMe。

这说明三者的关系可以表达为：

```text
GIDS：谁产生请求、怎样减少控制往返
  │
  ├── GDS 数据面：文件系统 + 直接 GPU DMA
  │
  └── uGDS 数据面：用户态 NVMe + 直接 GPU DMA
```

这是一种架构层次上的理解，并不代表现有产品已经把任意两者直接拼装成统一 API。

## 十一、常见误解

### 误解一：数据绕过 CPU，就等于 CPU 不参与

错误。GDS 与 uGDS 的数据不经过 CPU 内存，但请求仍由 CPU 生成和提交。数据路径与控制路径必须分别讨论。

### 误解二：uGDS 是 GIDS 的开源实现

错误。uGDS 的 `u` 是 user space，不是 GPU initiated。它的 I/O 由 CPU 用户态代码发起。

### 误解三：GIDS 一定比 GDS 快

不一定。对于大块、连续、可预取的文件读取，CPU 批量提交 GDS 已经很高效。GIDS 的主要收益出现在细粒度、动态、数据依赖型请求。

### 误解四：uGDS 只是把 cuFile 改了名字

错误。虽然 API 风格接近，但 uGDS 直接管理 NVMe SQ/CQ，并绕过文件系统与内核 NVMe 驱动，部署和语义差异很大。

### 误解五：直接 DMA 自动保证数据一致性

错误。NVMe、RDMA 和 GPU Kernel 访问同一块显存时，应用仍需用 I/O completion、CUDA/HIP stream 同步和 RDMA CQ 建立正确的生产者—消费者顺序。

## 十二、总结

GDS、GIDS 和 uGDS 分别优化 GPU 存储系统的不同维度：

1. **GDS 优化数据路径**：CPU 发起文件 I/O，存储数据绕过 CPU 内存直达 GPU；
2. **GIDS 优化控制路径**：GPU Kernel 产生存储请求，减少 GPU 与 CPU 的细粒度往返；
3. **uGDS 优化软件栈路径**：CPU 在用户态直接管理 NVMe 队列，绕过文件系统和内核 NVMe 快路径。

如果只记住一张图，可以记住：

```text
                 请求发起者       存储软件栈             数据路径
GDS              CPU             文件系统/内核/GDS      SSD → GPU
GIDS             GPU             取决于具体实现          SSD → GPU
uGDS             CPU 用户态      用户态 NVMe             SSD → GPU
```

它们不是简单的版本递进关系，而是三个可以独立比较、也可能在未来系统中组合的设计轴：**数据怎样移动、请求由谁产生、存储命令在哪里执行。**

## 系列文章

- [NVIDIA GPUDirect Storage（GDS）详解：让存储数据绕过 CPU 直达 GPU](/posts/nvidia-gpudirect-storage-gds/)
- [NVIDIA GPU-Initiated Data Storage（GIDS）详解：让 GPU Kernel 主动访问存储](/posts/nvidia-gpu-initiated-data-storage-gids/)
- [uGDS 原理解析：在用户态打通 NVMe SSD 与 GPU 显存](/posts/ugds-userspace-gpu-direct-storage/)
