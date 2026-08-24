---
title: "FMS 2026 深度解读：AI 正在重构内存与存储层级"
date: 2026-08-24T15:14:44+08:00
draft: false
tags: ["FMS 2026", "HBM", "HBF", "CXL", "PCIe 6.0", "NVMe", "AI 基础设施"]
categories: ["存储系统"]
summary: "深入解读 FMS 2026 的五条核心技术主线：High Bandwidth Flash、3D 内存封装、CXL 内存池、PCIe 6.0 液冷 SSD，以及面向 AI 的 NVMe 可运维基础设施。"
---

2026 年 8 月 4 日至 6 日，Future of Memory and Storage（FMS）在美国圣克拉拉举行。作为这场会议的 20 周年节点，FMS 2026 展示出的变化并不只是 NAND 层数继续增加、SSD 带宽继续翻倍，而是整个行业开始重新回答一个基础问题：

> 当 AI 加速器的计算能力增长速度远高于内存容量、内存带宽和数据装载速度时，系统应该如何重新组织 HBM、DRAM、CXL 内存、闪存与远程存储？

从 SK hynix 与 Sandisk 联合推动的 High Bandwidth Flash（HBF），到 Samsung 的 zHBM/zNAND-O 概念，再到 CXL 内存池、PCIe 6.0 企业 SSD和液冷设计，FMS 2026 的主线可以概括为：

```text
存储不再只是计算完成后的持久化终点，
而正在成为 AI 加速器旁边可调度的数据与容量层。
```

本文将 FMS 2026 的核心进展分为五条技术主线，并区分哪些已经形成标准或产品，哪些仍处于概念和路线图阶段。

## 一、为什么 AI 迫使内存和存储重新分层

传统服务器的层级大致是：

```text
CPU Cache -> DRAM -> 本地 SSD -> 网络存储
```

GPU 时代在最前端加入了 HBM：

```text
GPU SRAM/Cache -> HBM -> CPU DRAM -> SSD -> 网络存储
```

问题是，这个层级中存在两个越来越大的断层。

### 容量断层

HBM 带宽极高，但容量小、价格高、封装面积有限。大模型参数、Embedding、KV Cache 和训练状态的增长速度远快于单个加速器的 HBM 容量。

例如，一个拥有数百亿到数千亿参数的模型，即使量化后也可能无法完整放入单卡 HBM。推理系统还要为并发请求保留 KV Cache，训练系统则要保存优化器状态、激活值和 checkpoint。

### 带宽断层

NAND SSD 容量大、单位成本低，但访问方式和并行度仍按传统块设备设计。即使 PCIe 链路继续升级，单个请求的延迟和面向 AI tensor 的访问粒度仍远不如 HBM。

因此，AI 基础设施需要的不是简单地“扩大内存”或“加快 SSD”，而是增加新的中间层，并让软件按照数据热度和访问模式迁移数据。

FMS 2026 展示的 HBF、CXL 内存池和新型封装，本质上都在填补这些断层。

# 核心进展一：HBF——把 NAND 做成 HBM 的容量伙伴

## 1. HBF 是什么

SK hynix 与 Sandisk 在 FMS 2026 发布首个 High Bandwidth Flash 架构规范，并将其提交至 Open Compute Project。公开规范版本为 OCP HBF Architecture Specification v0.7。

HBF 的目标不是用 NAND 取代 HBM，而是设计一种：

- 容量远高于 HBM；
- 并行度远高于传统 SSD；
- 能够紧邻 AI 加速器部署；
- 软件上更像内存层而非独立块设备；
- 成本与功耗低于用 HBM 承载全部冷数据；

的新层级。

可以把 HBF 放在以下位置理解：

```text
GPU Cache / SRAM
        |
       HBM           极高带宽，低容量，高成本
        |
       HBF           高并行闪存，较大容量
        |
 CXL Memory / SSD    更大容量，延迟更高
```

## 2. 为什么普通 SSD 不够

传统 NVMe SSD 的并行度被封装在控制器内部，对主机暴露的是队列和逻辑块地址。AI 加速器要访问一批 tensor、Embedding 向量或 KV block，需要经历：

1. 构造 NVMe 命令；
2. 经 PCIe 传给 SSD 控制器；
3. 控制器进行 FTL 地址转换；
4. 调度 NAND channel、die 和 plane；
5. 数据通过相对较窄的 PCIe 接口返回。

SSD 内部可能有很多 NAND die，但这些并行度并未以 HBM 式宽接口直接暴露给加速器。

HBF 的基本思路是改变这一点：通过更多独立通道、宽接口和高密度堆叠，让大量 NAND die 可以并发服务请求。它试图把 NAND 从“一个拥有很多内部芯片的 I/O 设备”变为“一个可被大规模并行访问的容量层”。

## 3. HBF 最适合什么数据

HBF 并不适合所有 GPU 数据。它最适合以下特征：

- 容量远大于 HBM；
- 对带宽敏感，但允许比 HBM 更高的延迟；
- 读取多于写入；
- 数据能够以较大块或可预测模式预取；
- 热度低于当前计算工作集。

典型对象包括：

### 模型权重

推理时，并非每一层、每个专家或每组权重都需要长期驻留 HBM。Mixture-of-Experts 模型尤其适合把不活跃专家放在大容量层中，在路由确定后预取。

### KV Cache

长上下文和高并发会快速耗尽 HBM。活跃 token 的 KV block 留在 HBM，较老或低优先级会话可以下沉到 HBF。

### Embedding Table

推荐系统和检索系统的 Embedding 容量巨大，但访问高度稀疏。HBF 的容量和并行读取能力比昂贵 HBM 更匹配。

### Checkpoint 与训练状态

HBF 可以成为训练节点中的快速 checkpoint 层，减轻每次保存都穿过主机网络和远程存储的压力。

## 4. HBF 真正困难的地方

HBF 面临的挑战不只是做出更宽的闪存接口。

### NAND 延迟不会因为换名字消失

NAND 读取延迟仍显著高于 DRAM/HBM。更多通道只能提高并行吞吐，不能从物理上把单次访问变成纳秒级。

因此 HBF 必须依靠：

- 大规模并发；
- 请求合并；
- 软件预取；
- 数据布局优化；
- HBM Cache；
- 热度预测。

如果工作负载是强依赖链上的随机小读，HBF 仍可能让 GPU 等待。

### 写入耐久性与尾延迟

KV Cache 和 checkpoint 都可能包含写入。NAND 的擦写、垃圾回收和磨损均衡会引入尾延迟。若 HBF 想表现得更像内存，控制器必须把这些存储特性隐藏得更好，或让软件感知介质约束。

### 编程模型尚未定型

HBF 应该暴露成内存地址、块设备、对象空间，还是由 GPU runtime 管理的专用层？

不同选择会影响：

- 一致性；
- 页故障处理；
- 数据持久性语义；
- 多进程共享；
- 隔离与安全；
- 操作系统是否参与调度。

当前 HBF 已出现架构规范，但它仍是早期生态。规范不等于商业产品已经普及，更不意味着软件栈已经成熟。

## 5. 对产业格局的影响

HBF 最值得关注的不是一款产品，而是 HBM 厂商和 NAND 厂商第一次更明确地争夺同一段 AI 数据层级。

过去二者分工清晰：HBM 服务计算，NAND 服务持久化。HBF 试图建立新的产品类别，让 NAND 进入加速器封装附近。

如果成功，未来 AI 节点可能按以下方式配置：

```text
数百 GB HBM
数 TB HBF
数 TB CXL DRAM
数十 TB 本地 SSD
PB 级共享存储
```

这将改变 GPU 服务器的 BOM、封装设计、内存控制器、运行时和集群调度器。

# 核心进展二：Samsung zHBM 与 zNAND-O——从平面互连走向垂直系统

Samsung 在 FMS 2026 提出了 zHBM 与 zNAND-O 等概念，并展示 400 层以上 V10 BV-NAND、HBM4E/HBM5 和 LPDDR5X-PIM 路线。

这些命名背后体现的是同一个方向：**通过 3D 集成缩短计算与数据之间的物理距离。**

## 1. zHBM：把 HBM 放到加速器上方

当前 HBM 通常与 GPU/AI ASIC 并排放在硅中介层上：

```text
[HBM] [HBM] [GPU] [HBM] [HBM]
        Silicon Interposer
```

这种 2.5D 封装已经提供很宽的接口，但中介层面积、信号距离、封装良率与散热逐渐成为限制。

zHBM 概念尝试把 HBM 垂直堆叠到逻辑芯片上方：

```text
      HBM Stack
         |
  Vertical Interconnect
         |
     AI Accelerator
```

潜在收益包括：

- 更短互连；
- 更高带宽密度；
- 更小封装占地；
- 更多内存堆叠位置；
- 更低每 bit 传输能耗。

## 2. 垂直堆叠的真正瓶颈是热

AI 加速器本身可能达到数百瓦甚至上千瓦。HBM 也会产生大量热。如果将内存直接堆在计算芯片上方，热流必须穿过多个层级排出。

因此 zHBM 是否可行，不仅取决于 TSV、hybrid bonding 或微凸点，还取决于：

- 背面供电；
- 层间散热结构；
- 微流体冷却；
- 热点感知布局；
- 逻辑层与存储层的功率调度；
- 堆叠后的测试与修复能力。

3D 封装会把“内存墙”转化为“热墙”和“良率墙”。

## 3. zNAND-O：NAND 也要进入垂直 AI 封装

zNAND-O 代表将高性能 NAND 与 AI 逻辑更紧密集成的设想。相比 HBF 更偏向接口和存储层级标准，zNAND-O 更强调封装形态和物理集成。

其目标可能包括：

- 在加速器附近提供更大容量；
- 缩短 NAND 到计算逻辑的通路；
- 让封装内互连替代部分 PCIe 往返；
- 为推理权重、KV Cache 和 Embedding 提供本地容量层。

但截至 FMS 2026，它更接近技术愿景，而非可直接采购的标准产品。评价这类发布时，需要把“概念展示”“路线图”“工程样品”和“量产”严格区分。

## 4. 400 层以上 NAND：密度仍在推进，但不再是唯一指标

Samsung 展示 V10 BV-NAND，层数超过 400，采用 wafer bonding。NAND 厂商继续通过键合方式分别制造外围逻辑与存储阵列，再把晶圆连接起来。

这能改善：

- 单位面积密度；
- 外围逻辑工艺选择；
- I/O 速度；
- 阵列与控制逻辑的独立优化。

但 FMS 2026 释放的信号是：AI 市场不再只问“多少层、多少 TB”，而是同时追问：

- 能否提供稳定低尾延迟？
- 能否与 GPU 直接互连？
- 能否高并行读取？
- 能否被内存分层软件有效管理？

# 核心进展三：CXL 从扩容卡走向内存池与可组合基础设施

CXL 早期最容易理解的卖点是：给 CPU 增加更多内存。但 FMS 2026 的讨论重点已经转向更大的系统问题——如何让一组服务器和加速器共享、分配和迁移内存容量。

## 1. CXL 的价值不只是“慢一点的 DRAM”

单机内存扩展的结构是：

```text
CPU / Accelerator
      |
   CXL Link
      |
 CXL Memory Expander
```

而内存池化结构更接近：

```text
Host A ----\
Host B ----- CXL Switch ---- Shared Memory Pool
Host C ----/                  DRAM / SCM / Flash-backed tier
```

资源可以按工作负载动态分配，而不是在服务器采购时永久固定。

## 2. 为什么 AI 需要池化

AI 集群的内存需求经常不均衡：

- 训练作业在 checkpoint 或数据预处理阶段突然需要大量主机内存；
- 推理实例的 KV Cache 随并发和上下文长度波动；
- Embedding 或模型权重在不同节点间重复缓存；
- GPU 数量固定，但 CPU DRAM 容量可能成为调度约束；
- 某些节点内存空闲，另一些节点因容量不足无法启动任务。

CXL 池化允许调度器把内存视为集群资源，而不是主板上的固定部件。

## 3. CXL 需要真正的软件控制面

硬件能把远端内存映射到地址空间，只解决了“能访问”。要在生产环境中使用，还需要解决：

- 哪些页面放在本地 DRAM，哪些放在 CXL；
- 何时迁移；
- 多租户隔离；
- 带宽与延迟 QoS；
- 故障域与热拔插；
- NUMA 拓扑感知；
- 可观测性；
- 内存池碎片整理；
- 与 Kubernetes、虚拟机和 AI 调度器集成。

因此 FMS 2026 的 CXL 展示越来越强调“全栈”：控制器、交换芯片、设备固件、内核、管理软件和遥测，而不是单独一张扩展卡。

## 4. CXL 与 HBF 不是替代关系

两者解决不同问题：

- HBF 追求靠近加速器的闪存级大容量和高并行带宽；
- CXL 更强调一致性语义、资源池化和跨主机可组合；
- HBM 仍负责最热、延迟最敏感的工作集；
- NVMe SSD 继续承担高容量持久化。

未来软件可能管理如下层级：

```text
HBM：当前 kernel/tensor 工作集
HBF：冷权重、较冷 KV、Embedding
CXL DRAM：可共享的扩展内存
NVMe SSD：checkpoint、本地持久数据
NVMe-oF/Object：集群共享数据
```

真正困难的是跨层迁移策略，而不是单个介质的峰值指标。

# 核心进展四：PCIe 6.0 SSD 进入产品期，液冷成为设计条件

FMS 2026 上，PCIe 6.0 企业 SSD 不再只是控制器演示，而开始形成明确产品线。相较 PCIe 5.0，PCIe 6.0 采用 64 GT/s、PAM4 信号和 FLIT 模式，x4 链路理论双向能力再次提升。

## 1. 带宽增长服务于 AI 数据供应

AI 节点需要同时执行：

- checkpoint 写入；
- 模型权重加载；
- 数据集流式读取；
- KV Cache 卸载；
- 向 GPU Direct Storage 提供数据；
- 为多个 GPU 或租户共享 SSD。

PCIe 6.0 SSD 的意义不是让单线程 `read()` 自动翻倍，而是为高并发队列、多 GPU 和存储池提供更高链路上限。

## 2. SSD 的瓶颈正在从 NAND 转向整机功耗与散热

高性能企业 SSD 集成更多 NAND channel、更强控制器和更高速 SerDes，功耗随之增长。在密集 GPU 服务器中，前端 GPU 已经消耗大部分散热预算，SSD 很难继续依赖低速风冷。

因此 FMS 2026 上液冷 SSD、EDSFF 形态和面向液冷服务器的热设计受到更多关注。

这意味着 SSD 设计指标正在增加：

- 峰值带宽；
- 稳态性能；
- P99/P999 延迟；
- 每瓦 IOPS；
- 冷板接触与热阻；
- 固件温控降频曲线；
- 多盘同时满载时的机架热密度。

## 3. PCIe 6.0 不能自动解决端到端问题

即使 SSD 链路达到数十 GB/s，应用仍可能受限于：

- 文件系统锁；
- 页缓存或 direct I/O 对齐；
- CPU 提交开销；
- IOMMU 映射；
- GPU buffer 注册；
- PCIe 拓扑绕行；
- 单个 NAND die 延迟；
- 写放大和垃圾回收；
- 网络存储后端。

因此 PCIe 6.0 必须与 GDS、P2P-DMA、`io_uring`、SPDK、NVMe-oF 和计算存储结合，才能转化为 AI 应用可见的收益。

## 4. 对软件的直接影响

更快的设备会让软件固定开销更显眼。例如，若设备完成一次小 I/O 只需几十微秒，buffer 注册、系统调用、队列锁和完成处理可能占据大部分端到端延迟。

这与 Phoenix 等 GDS 重构工作的观察一致：硬件越快，传统软件栈中的“辅助步骤”越可能成为主要瓶颈。

# 核心进展五：NVMe 的重点从性能协议转向可运维 AI 基础设施

NVMe 已经完成从单盘协议到存储网络基础的演进。FMS 2026 上更值得注意的是，行业开始强调如何让 NVMe 在 AI 集群中变得可组合、可观测、可隔离和可持续运行。

## 1. AI 需要的是共享数据面

大型 AI 集群不能让每个 GPU 节点都保存完整数据副本。它们需要：

- NVMe-oF 共享高性能存储；
- 多路径与故障切换；
- GPU Direct RDMA；
- Namespace 与租户隔离；
- 端到端遥测；
- 计算存储或近数据处理；
- 对象存储与块存储协作。

NVMe 在这里承担的是从本地设备到 Fabric 数据面的统一协议角色。

## 2. 可管理性比峰值 IOPS 更重要

AI 作业可能持续数小时或数周，一次尾延迟抖动、路径故障或 SSD 热降频都可能拖慢整个同步训练集群。

因此企业真正关心：

- 是否能快速定位慢盘和慢路径；
- 是否支持细粒度健康数据；
- 固件升级是否不中断业务；
- 多路径是否能按负载动态切换；
- 是否能对训练、推理和 checkpoint 流量做 QoS；
- 故障是否会扩散到整个 GPU Pod。

NVMe 的竞争正在从“设备能跑多快”转向“数千设备是否能长期一致地快”。

## 3. 计算存储会以专用功能而非万能计算回归

把 CPU/加速器放进 SSD 的计算存储概念已经出现多年。AI 时代重新提供了适合近数据处理的任务：

- 解压缩；
- 校验和与加密；
- 数据过滤；
- 格式转换；
- 向量索引扫描；
- checkpoint 去重与压缩；
- 数据集预处理。

但通用计算存储面临开发、隔离和调度困难。更可能落地的形式是固定功能或受限可编程的数据处理单元，而不是让每块 SSD 变成通用服务器。

# 六、如何理解这五条进展之间的关系

FMS 2026 的发布看似分散，实际可以放进同一张数据层级图：

```text
┌──────────────────────────────────────┐
│ GPU/AI ASIC                          │
│ SRAM / Cache                         │
├──────────────────────────────────────┤
│ HBM / zHBM                           │  最热工作集
├──────────────────────────────────────┤
│ HBF / zNAND-O                        │  大容量近加速器层
├──────────────────────────────────────┤
│ CXL DRAM Pool                        │  可组合扩展内存
├──────────────────────────────────────┤
│ PCIe 6.0 NVMe SSD                    │  本地持久化与缓存
├──────────────────────────────────────┤
│ NVMe-oF / Distributed / Object Store │  集群共享容量
└──────────────────────────────────────┘
```

它们不是相互替代，而是在重新切分“速度、容量、成本、持久性和共享范围”。

## 数据应如何分布

| 数据 | 更可能的层级 |
| --- | --- |
| 当前计算 tile、激活值 | HBM |
| 活跃 KV Cache | HBM |
| 冷 KV Cache | HBF / CXL / SSD |
| 活跃专家权重 | HBM |
| 非活跃 MoE 专家 | HBF / CXL |
| 超大 Embedding | HBF / CXL / SSD |
| 最近 checkpoint | 本地 PCIe 6.0 SSD |
| 长期 checkpoint 和数据集 | NVMe-oF / 对象存储 |

## 软件将成为决定因素

硬件层级越多，错误放置数据的成本越高：

- 把冷数据长期放 HBM，浪费昂贵容量；
- 把热数据放 NAND，GPU 因等待而空闲；
- 迁移过于频繁，会消耗互连带宽；
- 预取错误会放大写入与缓存污染；
- 多租户争抢共享层会产生严重尾延迟。

未来关键软件包括：

- GPU runtime 的统一虚拟地址与页迁移；
- KV Cache 调度器；
- MoE 权重预取；
- CXL 内存 tiering；
- GDS/P2P-DMA 数据路径；
- 跨层 admission control；
- 基于模型语义的数据放置；
- 端到端遥测和反馈控制。

# 七、哪些已经成熟，哪些仍需谨慎

| 技术 | FMS 2026 状态判断 | 主要风险 |
| --- | --- | --- |
| PCIe 6.0 企业 SSD | 进入明确产品阶段 | 功耗、散热、平台支持、软件利用率 |
| CXL 内存扩展 | 产品化推进中 | 延迟、交换生态、软件管理与故障语义 |
| CXL 内存池 | 早期部署/生态建设 | 多主机共享、QoS、编排复杂度 |
| HBF | 已有早期 OCP 架构规范 | 控制器、接口、软件模型、耐久性、量产时间 |
| zHBM | 概念与长期封装路线 | 散热、供电、良率、测试、成本 |
| zNAND-O | 概念性架构 | 介质延迟、封装与生态尚未确定 |
| NVMe-oF/GDS | 已部署并持续演进 | 拓扑、尾延迟、运维复杂度 |

FMS 展会信息通常混合标准、原型、路线图与量产产品。理解技术进展时，不能把“发布概念”直接等价为“明年可大规模采购”。

# 八、对 AI 基础设施的几个判断

## 1. HBM 不会被替代，但会被更精细地使用

未来 HBM 更像 CPU 的大容量最后级缓存：只存放正在计算或即将计算的数据。容量型数据会逐渐下沉到 HBF、CXL 和 SSD。

## 2. KV Cache 将成为新存储层级的首批杀手级负载

KV Cache 兼具容量大、访问有阶段性、可按 block 管理、对延迟敏感等特点，天然适合展示多层内存的价值。FMS 2026 的大量技术都能在 KV Cache 分层中找到应用场景。

## 3. 封装、互连和冷却已成为同一个问题

zHBM、HBF 和 PCIe 6.0 SSD 都受到功耗密度限制。未来内存与存储产品不能脱离机架供电和液冷系统单独设计。

## 4. 标准接口会比封闭数据路径更有生命力

AI 系统需要同时连接 GPU、NIC、SSD、CXL 内存和远程存储。能复用 POSIX、NVMe、CXL、RDMA 和开放管理接口的方案，更容易进入复杂生产环境。

## 5. 峰值带宽的重要性会下降，尾延迟与调度效率更重要

训练同步、推理 SLO 和大规模存储共享都对尾延迟极为敏感。单设备 benchmark 中的峰值 GB/s，不能代表上千 GPU 集群中的有效吞吐。

# 总结

FMS 2026 的核心不是某个厂商把 NAND 堆到 400 多层，也不是 PCIe 6.0 SSD 再次刷新顺序读带宽。

真正的变化是：

> AI 正迫使内存与存储从一条固定的硬件层级，演进为一组可由软件动态管理的数据资源池。

HBF 试图用 NAND 填补 HBM 与 SSD 之间的容量带宽空白；zHBM 和 zNAND-O 把竞争推进到垂直封装；CXL 将内存从单机部件变成可组合资源；PCIe 6.0 和液冷 SSD提高本地持久层上限；NVMe 则继续成为连接本地设备、Fabric 与 GPU 数据路径的基础协议。

接下来真正决定成败的，将不只是芯片和介质，而是软件能否理解模型结构、请求生命周期和数据热度，在 HBM、HBF、CXL、SSD 和远程存储之间正确地放置与迁移数据。

FMS 2026 展示的是新硬件层级的起点。下一阶段的竞争，会发生在“谁能把这些层级组合成一个稳定、透明、可运维的 AI 内存系统”。

# 参考资料

- [Future of Memory and Storage 2026](https://futurememorystorage.com/)
- [NVM Express：FMS 2026 活动页面](https://nvmexpress.org/event/future-of-memory-and-storage-fms-2026/)
- [OCP High Bandwidth Flash Architecture Specification v0.7](https://www.opencompute.org/documents/ocp-hbf-architecture-specification-v0-7-0-final-pdf)
- [SK hynix：HBF at FMS 2026](https://news.skhynix.com/en/hbf-at-fms-2026/)
- [Samsung：Next-Generation 3D Memory Vision at FMS 2026](https://news.samsung.com/global/samsung-unveils-next-gen-3d-memory-vision-at-fms-2026-charting-the-future-of-ai-infrastructure)
- [Linux PCI Peer-to-Peer DMA Support](https://docs.kernel.org/driver-api/pci/p2pdma.html)
- [Compute Express Link Consortium](https://computeexpresslink.org/)
