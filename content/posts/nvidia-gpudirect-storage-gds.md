---
title: "NVIDIA GPUDirect Storage（GDS）详解：让存储数据绕过 CPU 直达 GPU"
date: 2026-08-19T14:00:00+08:00
draft: false
tags: ["NVIDIA", "GPUDirect Storage", "GDS", "GPU", "存储", "CUDA"]
categories: ["技术"]
---

在 AI 训练、科学计算和数据分析系统中，GPU 的算力越来越强，但数据从存储设备进入 GPU 的路径却可能成为瓶颈。传统 I/O 通常要先把数据读入 CPU 内存，再复制到 GPU 显存，不仅增加内存带宽消耗，还让 CPU 承担大量数据搬运工作。

NVIDIA GPUDirect Storage，简称 **GDS**，解决的正是这个问题：它在存储设备与 GPU 显存之间建立更直接的数据路径，使应用能够通过 `cuFile` API 将文件数据读入 GPU 缓冲区，减少 CPU bounce buffer 和不必要的数据复制。

本文介绍 GDS 的工作原理、软件栈、典型使用方式、适用场景以及它与 GPU-Initiated Data Storage（GIDS）的关系。

## 一、传统 GPU I/O 为什么效率不高

传统文件读取到 GPU 的路径大致如下：

```text
NVMe / 文件系统
       │
       ▼
CPU 内存缓冲区
       │ cudaMemcpy
       ▼
GPU 显存
```

应用通常先调用 `read`、`pread` 或异步 I/O 接口，把文件内容读入主机内存，然后再调用 CUDA memcpy 将数据复制到 GPU。

这条路径存在几个问题：

- 同一份数据先经过 CPU 内存，再进入 GPU 显存；
- 存储流量和 GPU 传输流量竞争 CPU 内存带宽；
- CPU 需要提交、管理和完成数据搬运；
- 大规模 GPU 系统中，CPU 和内存通道容易成为共享瓶颈；
- 应用需要维护主机端 staging buffer，并处理双重缓冲。

当 GPU 计算越来越快、单机挂载更多 NVMe 或更高速的并行文件系统后，这些额外开销会更加明显。

## 二、GDS 是什么

GDS 是 NVIDIA GPUDirect 技术家族面向存储 I/O 的组成部分。它允许兼容的存储和文件系统通过 DMA 路径与 GPU 显存交换数据，而不必总是经过 CPU 内存中的中转缓冲区。

典型数据路径变为：

```text
NVMe / 文件系统
       │ DMA
       ▼
GPU 显存
```

需要注意，“绕过 CPU”主要是指**数据路径**。在经典 GDS 模型中，CPU 仍然负责：

- 运行应用控制逻辑；
- 调用 `cuFileRead`、`cuFileWrite` 等 API；
- 向存储栈提交 I/O；
- 处理文件描述符、错误和完成状态。

因此 GDS 的核心可以概括为：

> CPU 发起 I/O，但数据尽可能不经过 CPU 内存，而是直接进入或离开 GPU 显存。

这也是它与 GIDS 最关键的区别。

## 三、GDS 的软件栈

GDS 并不是只安装一个 CUDA 库就能使用，它依赖 GPU、驱动、文件系统和存储设备共同提供一条可用的数据路径。

从应用到硬件可以抽象为：

```text
GPU 应用
   │
   ▼
cuFile API / libcufile
   │
   ▼
NVIDIA 驱动与 nvidia-fs
   │
   ▼
兼容文件系统 / 块设备
   │
   ▼
NVMe、本地阵列或并行存储
```

### 1. cuFile API

`cuFile` 是 GDS 面向应用的主要 API。应用注册文件句柄和 GPU 缓冲区后，可调用同步或异步接口读写数据。

常见调用过程为：

1. 使用普通文件 API 打开文件；
2. 将文件描述符注册为 `CUfileHandle_t`；
3. 分配 GPU 缓冲区；
4. 视需要注册 GPU 缓冲区；
5. 调用 `cuFileRead` 或 `cuFileWrite`；
6. 注销缓冲区和文件句柄。

简化后的伪代码如下：

```cpp
int file_descriptor = open(path, O_RDONLY | O_DIRECT);

CUfileDescr_t descriptor = {};
descriptor.handle.fd = file_descriptor;
descriptor.type = CU_FILE_HANDLE_TYPE_OPAQUE_FD;

CUfileHandle_t handle;
cuFileHandleRegister(&handle, &descriptor);

void* device_buffer = nullptr;
cudaMalloc(&device_buffer, size);
cuFileBufRegister(device_buffer, size, 0);

ssize_t bytes_read = cuFileRead(
    handle,
    device_buffer,
    size,
    file_offset,
    0
);
```

实际程序还需要完整检查 CUDA、文件系统和 cuFile 的返回值，并满足设备、内存地址、文件偏移和 I/O 大小的对齐要求。

### 2. 内核模块与驱动

GDS 使用 NVIDIA 驱动提供 GPU 内存映射和 DMA 能力。`nvidia-fs` 模块在经典架构中负责连接 GPU 内存与文件系统、块设备路径。

不同驱动、CUDA 和 Linux 内核组合支持的能力可能不同，因此部署时应以当前 NVIDIA GDS 文档中的支持矩阵为准，而不能只检查 GPU 型号。

### 3. 文件系统与存储

GDS 是否真正走直接路径，取决于文件系统和存储后端是否受支持。常见部署包括：

- 本地 NVMe；
- 软件定义的本地存储；
- 支持 GDS 的并行文件系统；
- 支持 RDMA 和 GPU Direct 路径的远程存储系统。

如果某条路径不满足条件，`libcufile` 可能使用兼容模式或回退路径。程序仍可能正常读取，但性能特征会与真正的 GDS direct path 不同。

## 四、GDS 如何提升性能

### 1. 减少数据复制

传统路径通常包含“存储到主机内存”和“主机内存到 GPU”两个阶段。GDS 尽可能将其合并为存储与 GPU 之间的一次 DMA 数据传输。

### 2. 降低 CPU 内存带宽压力

CPU 内存带宽是整机共享资源。数据集越大、GPU 越多，主机内存中转造成的压力越明显。GDS 把部分数据流量移出 CPU 内存路径，可以让 CPU 和内存资源留给数据预处理、网络协议或其他任务。

### 3. 降低 CPU 开销

GDS 不代表 CPU 完全不参与，但可以减少 CPU 执行 memcpy、管理 staging buffer 和驱动额外复制的开销。

### 4. 改善流水线并行

结合 CUDA Stream、异步 cuFile API 和双缓冲，应用可以构建如下流水线：

```text
读取批次 N+1  ───────────────┐
                              │ 并行
计算批次 N    ────────────────┘
```

数据读取与 GPU 计算重叠后，端到端吞吐量往往比单次 I/O 延迟更重要。

## 五、同步与异步 GDS

经典 `cuFileRead` 和 `cuFileWrite` 是主机线程发起的同步调用。应用也可以使用批量或异步能力，将请求与 CUDA Stream 配合。

异步 GDS 的价值主要在于：

- 减少主机线程阻塞；
- 批量提交多个 I/O；
- 让 I/O 与 kernel 执行重叠；
- 在多个 GPU 或多个文件分片之间建立流水线。

但即使使用异步接口，请求通常仍由 CPU 侧代码准备和提交。**异步 GDS 不等于 GIDS**：前者改变等待与调度方式，后者进一步让 GPU kernel 自己发起存储请求。

## 六、GDS 的典型应用场景

### 1. AI 训练数据加载

大规模训练需要持续读取样本、特征、embedding 或 checkpoint。对于已经预处理好、可直接供 GPU 消费的数据，GDS 可以减少 CPU staging 开销。

如果训练管线包含大量 CPU 解码、随机增强或复杂解析，瓶颈可能仍在 CPU 预处理而非数据复制。此时需要先测量，再判断 GDS 能带来多少收益。

### 2. 推理与向量检索

当模型权重、KV 数据、向量索引或 embedding 无法全部驻留显存时，系统需要按需从高速存储加载数据。GDS 能为 GPU 缓冲区提供更直接的数据入口。

### 3. 科学计算

地震分析、气象模拟、计算流体力学和基因分析往往需要在大型文件与 GPU 数组之间传输数据，适合使用 GDS 减少主机内存中转。

### 4. Checkpoint

训练过程既要将参数写入存储，也要在故障恢复时快速读取。GDS 同时支持读写，可用于改善 checkpoint 保存和恢复路径。

## 七、部署与诊断

部署 GDS 时不应只看应用是否成功返回，还要确认实际数据路径。

常用检查思路包括：

1. 查看 NVIDIA 驱动、CUDA Toolkit 和 GDS 组件版本；
2. 使用 `nvidia-smi` 检查 GPU 和驱动状态；
3. 使用 `gdscheck` 检查系统配置；
4. 使用 `gdsio` 测量 GPU 缓冲区 I/O；
5. 检查挂载参数、IOMMU、PCIe 拓扑和设备亲和性；
6. 查看 cuFile 日志，确认是否发生兼容模式或 POSIX 回退；
7. 将 GDS 与普通 `pread + cudaMemcpy` 的端到端吞吐量对比。

PCIe 拓扑尤其重要。即使软件支持 GDS，如果 NVMe 和 GPU 跨越多个 PCIe Root Complex 或 NUMA 节点，实际路径也可能绕行，影响带宽与延迟。

## 八、GDS 的限制

GDS 并不是所有场景下都一定更快。

- 小而离散的 I/O 可能被提交和元数据开销主导；
- 数据需要 CPU 解压、解析或增强时，仍离不开主机处理；
- 不兼容的文件系统可能触发回退；
- 不合理的文件布局和随机访问会限制存储吞吐；
- GPU 与存储设备的 PCIe 拓扑会影响结果；
- 多进程、多 GPU 并发可能让瓶颈转移到文件系统或 NVMe 队列。

因此，评估 GDS 时应该测量完整工作负载，而不仅仅看理想条件下的大块顺序读取带宽。

## 九、GDS 与 GIDS 的区别

两者都希望缩短存储到 GPU 的路径，但改变的是不同层次。

| 对比项 | GDS | GIDS |
| --- | --- | --- |
| 全称 | GPUDirect Storage | GPU-Initiated Data Storage |
| 请求发起者 | CPU 主机代码 | GPU kernel / GPU 线程 |
| 数据目标 | GPU 显存 | GPU 显存 |
| 主要 API | 主机侧 cuFile API | 设备侧存储 API |
| CPU 数据中转 | 尽可能避免 | 尽可能避免 |
| CPU 控制路径 | 仍然存在 | 从细粒度 I/O 控制路径中移除 |
| 适合模式 | 大块、批量、可预知的 I/O | 依赖 GPU 计算结果的细粒度动态 I/O |
| 成熟度 | 已广泛用于生产环境 | 更前沿，依赖新软件与硬件支持 |

一句话总结：

> GDS 是“CPU 下命令，数据直达 GPU”；GIDS 是“GPU 自己下命令，数据也直达 GPU”。

## 十、总结

GDS 的价值不是简单地把文件读得更快，而是重构 GPU 应用的数据通路：

- 减少 CPU 内存中转和数据复制；
- 降低 CPU 与主机内存带宽压力；
- 支持存储 I/O 与 GPU 计算并行；
- 为 AI、HPC 和 GPU 数据分析提供更可扩展的 I/O 基础。

如果应用的 I/O 可以由 CPU 预先规划，并且主要以较大块、批量方式传输，GDS 通常是更成熟、直接的选择。若 GPU 需要根据 kernel 内部计算结果动态访问海量存储对象，则应继续关注 GIDS。

## 系列导航

- [GDS、GIDS 与 uGDS 有什么区别：从数据直达、GPU 发起到用户态 NVMe](/posts/gds-gids-ugds-comparison/)
- [NVIDIA GPUDirect Storage（GDS）详解](/posts/nvidia-gpudirect-storage-gds/)
- [NVIDIA GPU-Initiated Data Storage（GIDS）详解](/posts/nvidia-gpu-initiated-data-storage-gids/)
- [uGDS 原理解析](/posts/ugds-userspace-gpu-direct-storage/)

## 参考资料

- NVIDIA GPUDirect Storage Overview Guide
- NVIDIA GPUDirect Storage Design Guide
- NVIDIA GPUDirect Storage cuFile API Reference Guide
- NVIDIA GPUDirect Storage Best Practices Guide
