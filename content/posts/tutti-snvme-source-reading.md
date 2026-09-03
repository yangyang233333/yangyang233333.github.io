---
title: "Tutti 源码阅读（二）：GPU 如何直接驱动 NVMe 队列"
date: 2026-09-03T17:40:00+08:00
draft: false
tags: ["Tutti", "源码阅读", "NVMe", "GPU I/O", "SNVMe"]
categories: ["技术"]
---

Tutti 最具辨识度的代码位于运行时以下：自定义 `snvme` 内核模块建立受控 NVMe queue pair，`libnvm` 管理设备资源，GPU kernel 批量写 SQE、更新 doorbell 并轮询 CQE。数据通过 PCIe P2P DMA 在 SSD 与 HBM 之间移动，CPU 不再逐条提交 I/O。

本文沿一次读请求下潜，分析为什么主线 Linux NVMe 驱动不够、控制面与数据面如何分离、GPU 如何构造命令，以及条带化和 layer-wise overlap 怎样落实到代码结构。

项目源码：[xPU-IO/Tutti](https://github.com/xPU-IO/Tutti)。

## 一、为什么需要自定义 `snvme`

NVMe 控制器的数据面并不复杂：主机写 Submission Queue，更新 MMIO doorbell，设备执行 DMA，再把 Completion Queue Entry 写回内存。

问题是 Linux 主线 NVMe 驱动没有向普通应用提供一套稳定机制，用来同时完成：

- 创建应用独占或受控的 I/O queue pair；
- 将 SQ/CQ 放入可被应用或 GPU 访问的固定内存；
- 把对应 doorbell BAR 页安全映射给应用；
- 将 GPU HBM 注册成 NVMe 可 DMA 的 peer memory；
- 允许应用自行轮询 CQ，而不经过每请求系统调用。

传统 `read`、`io_uring` 或 GDS 可以缩短部分路径，但 NVMe 队列仍主要由内核或 CPU 侧库控制。Tutti 要让 GPU 成为请求生产者，因此需要一个负责授权与建图的内核控制面。

`snvme` 的思想与 RDMA uverbs 很接近：

| RDMA | Tutti `snvme` |
|---|---|
| 内核创建 QP | 内核创建 NVMe SQ/CQ |
| 注册 MR | 注册 HBM/host memory |
| mmap user doorbell | mmap NVMe doorbell |
| 用户态写 WQE | GPU 写 NVMe SQE |
| 用户态轮询 CQE | GPU 轮询 NVMe CQE |

它不是让 GPU 任意操作控制器，而是由内核先创建隔离资源、限制 DMA 范围，再开放一个经过授权的快路径。

## 二、daemon 拥有控制器

Tutti 当前采用 daemon-only 拓扑。`tutti_daemon` 负责设备 bring-up、队列预算、mount/view 和租约；应用作为客户端 attach。

```text
                 ┌────────────────────┐
                 │ tutti_daemon       │
                 │ controller owner   │
                 │ queue allocation   │
                 └─────────┬──────────┘
                           │ lease / attach
          ┌────────────────┴───────────────┐
          │                                │
   Application A                    Application B
   Runtime + GPU                    Runtime + GPU
```

集中所有权解决了两个底层问题：

- 多进程不能无协调地修改同一个控制器和队列状态；
- 进程异常退出后，需要统一回收 queue、DMA mapping 和租约。

应用配置只声明想要的逻辑资源，daemon 根据真实设备和 ACL 返回运行时事实。队列深度也以控制器能力为准，而不是相信应用随意填写的数值。

## 三、初始化慢路径

在真正提交 I/O 前，系统要完成大量不能放进快路径的工作：

```text
发现 NVMe controller
      ↓
创建 I/O SQ / CQ
      ↓
固定 queue memory
      ↓
映射 BAR / doorbell
      ↓
注册 GPU peer memory
      ↓
建立 DMA 地址与 PRP 元数据
      ↓
把设备视图交给 GPU runtime
```

这些步骤需要内核权限和厂商 peer-memory API。NVIDIA 路径依靠相应的 peer-memory backend 固定 HBM 页面并为 NVMe 设备建立 DMA 映射；代码还为其他 GPU 厂商保留了对称 backend 接口。

注册完成后，GPU kernel 使用的是设备可见地址、queue 指针、doorbell 地址和预计算元数据，而不是 Linux 文件描述符。

## 四、一次 GPU NVMe Read

读请求可拆成六步。

### 1. CPU 准备批描述符

应用通过 `StorageRuntime::submit` 提交一批逻辑请求。CPU 侧完成句柄校验与分组，并把紧凑描述符复制到 GPU 可见内存。一次 kernel launch 覆盖整批请求，而不是一条 I/O 启动一次 kernel。

### 2. GPU 解析逻辑范围

每个线程或线程组处理一个请求，根据 target payload 计算：

- 请求属于哪块 NVMe；
- 起始 LBA 与 block 数；
- 目标 GPU buffer 的 DMA 地址；
- 请求在 queue 中的位置。

条带化后端也在这一步把连续逻辑范围映射到不同成员盘。

### 3. GPU 构造 SQE

NVMe Read SQE 至少要包含 opcode、namespace、LBA、长度、command id 和数据指针。数据指针通常由 PRP 描述。

```text
NVMe SQE
├── opcode = READ
├── command id
├── namespace id
├── PRP1 / PRP2
├── starting LBA
└── number of logical blocks
```

GPU buffer 可能跨多个不连续 DMA 页，因此注册阶段预建或缓存 PRP 信息很关键。否则每次请求都重新遍历页映射，会把慢路径成本带回 I/O kernel。

### 4. 发布 SQ 并敲 doorbell

并行写 SQE 后，需要保证命令内容在 doorbell 更新前对设备可见。kernel 必须正确处理线程同步和内存顺序，再由指定线程更新 SQ tail 对应的 MMIO doorbell。

```text
write SQEs
   ↓ memory ordering
publish new tail
   ↓
MMIO doorbell store
```

doorbell 写入通知控制器读取新命令。相比 CPU 逐条提交，GPU 可以在一次 kernel 中填充很深的队列。

### 5. SSD 直接 DMA 到 HBM

控制器读取 SQE 后，按照 PRP 指向的 DMA 地址把数据写入 GPU HBM：

```text
NVMe NAND → controller → PCIe P2P → GPU HBM
```

数据不经过用户态 host buffer，也不需要随后执行一次 H2D copy。

### 6. GPU 轮询 CQE

设备完成后写 CQE。GPU 线程检查 command id、status 和 phase bit，识别 ring wrap-around，并把结果写回批 operation 状态。完成后更新 CQ head doorbell，使设备可以复用队列项。

同步 API 最终由 `wait` 汇总状态，但每条请求是否成功仍独立可见。

## 五、GPU 提交为什么能提高小 I/O 吞吐

单条 NVMe 命令很小，CPU 也能快速构造。真正差异来自规模：长上下文 KV Cache 会同时产生数百到数千个 block 请求。

CPU-centric 路径的固定工作近似随请求数增长：

```text
总控制成本 ≈ 请求数 × 单请求解析/提交/完成成本
```

Tutti 把对象解析、SQE 生成和 CQE 检查映射到 GPU 线程，并把 kernel launch 和描述符传输摊销到整个批次。只要批量足够大，系统更容易维持高 queue depth，发挥多块 SSD 的总带宽。

这并不意味着 GPU 对所有 I/O 都更合适。小批量、低频请求可能不值得支付 kernel launch 和 GPU 资源占用；Tutti 针对的是大规模、结构稳定的 KV Cache 工作负载。

## 六、多盘条带化：一个 kernel 扇出多个控制器

项目的 `striped_local_nvme` 最多支持四块盘。逻辑对象按配置的 stripe unit 映射到成员设备：

```text
逻辑地址： [0][1][2][3][4][5][6][7]
设备编号：  D0 D1 D2 D3 D0 D1 D2 D3
```

传统实现可能在 CPU 上把请求分成四组，再分别调用四个后端。Tutti 将设备选择和 SQE 生成融合进同一个 GPU kernel。不同线程面向不同控制器写各自队列，最后统一处理完成。

条带单位应匹配 KV tensor 或常见 I/O 粒度。过小会增加命令数，过大则可能造成设备负载不均。论文和示例采用按层 K/V tensor 组织的工作负载，使单次流水能够同时利用多盘。

## 七、Layer-wise overlap 如何进入执行路径

仓库提供 `layerwise_kv_overlap` 示例，模拟 80 层、每个 K/V tensor 512 KiB 的工作负载。它展示的不是单纯峰值读带宽，而是三条流水的并行：

```text
Load KV of next layer
          ∥
Compute current layer
          ∥
Store evicted/produced KV
```

实现上，CPU 按层将 I/O kernel 放入 GPU stream，并通过 event 或依赖关系约束“数据必须在该层消费前完成”。调度器要在两个目标之间平衡：

- 足够早地预取，避免计算等待 SSD；
- 不让 I/O kernel 过度占用 SM 和内存系统。

所以 Tutti 所谓 GPU-centric 并不等于“所有 GPU 线程一直轮询”。真正有效的实现需要控制 kernel 粒度、并发度和发射时机。

## 八、`cuda_like` 与厂商可移植性

Tutti 把 GPU 相关接口拆成三层：公共 `cuda_like` API、厂商运行时实现，以及供内核消费的 P2P backend。这样上层 DataPath 可以使用统一的 stream、event、内存和 kernel launch 语义。

当前 CUDA 路径经过生产验证；HOST profile 主要用于契约测试，MUSA/MACA 提供构建框架但尚不能等价看作完整硬件验证。

这类抽象最难的不是函数名映射，而是语义一致：

- stream 顺序保证是否相同；
- event 是否能表达相同依赖；
- peer-memory 页固定与 DMA 地址生命周期是否一致；
- MMIO 映射和内存栅栏在不同 GPU 上是否成立。

因此 `cuda_like` 能降低上层耦合，但不能消除每个厂商后端的验证工作。

## 九、源码中的风险边界

这种设计绕过了通用内核 I/O 快路径，也承担了更多系统责任：

### 队列正确性

SQ tail、CQ head、phase bit、command id 回收和并发发布任何一处错误，都可能导致超时或数据错配。

### DMA 生命周期

I/O 完成前不能释放或重新映射 GPU buffer。异常退出时 daemon 和内核模块必须可靠撤销映射。

### 文件 extent 稳定性

如果 Resolver 依赖 FIEMAP 得到物理 extent，文件在运行期间不能被 truncate、重写或发生不可控重分配，否则逻辑对象可能指向旧 LBA。

### 拓扑与隔离

P2P 是否可达取决于 PCIe hierarchy、IOMMU 和平台配置。多租户环境还必须确保应用只能访问自己的队列和注册内存。

### GPU 资源竞争

I/O kernel 不是免费的。轮询线程块、寄存器和显存访问可能影响推理 kernel，需要通过 profiling 调整并发与 slack window。

## 十、总结

Tutti 的底层实现可以浓缩为一句话：

> 内核负责授权与建图，daemon 负责资源所有权，GPU 负责稳态 NVMe 数据面。

一次读请求的关键路径是：

```text
batch descriptors
      ↓
GPU resolve device/LBA/PRP
      ↓
GPU write SQE + ring doorbell
      ↓
NVMe P2P DMA to HBM
      ↓
GPU poll CQE
```

它把 RDMA 世界成熟的“用户队列、doorbell、注册内存、轮询完成”模式迁移到 NVMe，并进一步让 GPU 成为队列生产者。对长上下文 KV Cache 来说，这种高并行、可流水化的数据面，才是 SSD 从廉价容量层变成在线缓存层的关键。
