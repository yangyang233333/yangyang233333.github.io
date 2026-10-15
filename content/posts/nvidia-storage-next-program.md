---
title: "[译]NVIDIA Storage-Next Program"
date: 2026-10-15T15:12:00+08:00
draft: false
description: "逐句翻译 NVIDIA Storage-Next Program 官方项目说明，覆盖项目定义、为什么重要、与 SCADA 的关系、范围、贡献领域、参与方式和标准化路径。"
tags: ["NVIDIA", "AI 存储", "SCADA", "GPU"]
categories: ["翻译"]
---

> 译者注：以下按原文标题、段落和列表顺序翻译。PDF 页脚、页码和文档编号不逐字重复；第 4 页的 lorem ipsum 占位句和版权/商标声明保留在文末。

## 项目概览

## NVIDIA Storage-Next Program

### 定义面向 AI 的下一代存储

NVIDIA Storage-Next™ 是一个由 NVIDIA 主导并管理的生态合作项目，目标是为那些需要对远超系统内存的数据集进行细粒度、高吞吐访问的 AI 工作负载，定义存储架构、设备与软件接口。该计划汇聚存储提供商、介质、控制器、OEM、超大规模云厂商以及软件生态贡献者，共同推动技术成熟、验证需求，并为未来的标准化做好合适准备。

Storage-Next 项目的参与者正在提供反馈，并帮助定义下一代 AI 存储基础设施。

### 为什么重要

AI 基础设施正越来越受内存容量、数据搬运、细粒度访问模式和总拥有成本制约。Storage-Next 通过帮助生态围绕能更高效服务 AI 工作负载的存储系统达成一致，来缓解这些压力：

- 访问超过 GPU 和 CPU 内存容量一到两个数量级的数据集。
- 图等新数据结构，以及近似最近邻（ANN）搜索等新算法，可以尽量降低“从存储读取的字节数”与“最终结果中的字节数”（例如搜索结果）之比，帮助缓解计算与无限增长数据之间不断扩大的 IO 瓶颈。
- 为细粒度 AI 数据访问提供更高的有效 IOPS（每秒输入/输出操作数）、带宽和并发。
- 通过降低功耗、空间、BOM（物料清单）成本和不必要的数据搬运，改善 TCO。
- 为向量搜索、图分析、embedding 表以及未来的 AI 内存层级，提供扩展性更强的存储架构。
- 让 GPU、SSD、存储服务器、文件系统、存储协议、编排和标准组织之间的生态协作更清晰。

### 新兴 AI 工作负载的关键挑战

- 容量：过去能放进内存的数据集，如今正增长到内存容量的 100 倍乃至更高。
- 存储 IO 瓶颈：在存储与计算之间搬运数据，已成为新的瓶颈。
- 新使用模式：向量数据库索引与搜索，以及在欺诈检测和电商中使用 GNN，都是正在发展的新兴应用，其基础设施仍在建设中。
- 新基础设施：这些新需求催生新的存储架构，目标是让 SSD 在每瓦每美元下的 IOPS 提升 10 倍。存储产业正在联合推出更高性能、更省电的设计。

## Storage-Next 与 SCADA 的关系

这项计划的立足点，是大型 AI 数据集的加速数据访问。NVIDIA SCADA™（Scaled, Accelerated Data Access）充分利用 GPU 的高并发、高带宽内存和可容忍延迟的执行能力，使 AI 访问模式成为可能。SCADA 由可编入应用的 header SCADA 客户端库组成，用于 cuGraphGNN 或 cuVS 这类中间件框架。它还包含一个拥有特权的 SCADA 服务端组件；该组件可以在同一块 GPU 或另一块 GPU 上，既可收敛在同一节点，也可通过网络解耦部署，并安全访问客户端内存位置和存储。

Storage-Next 把这个基础扩展到更完整的系统架构：存储架构、软件与部署基础设施、存储提供商、SSD、存储控制器、存储服务器、外形规格、热设计、散热、编排、中间件、遥测、数据保护，以及基于标准的接口。

Storage-Next 目前是一项由 NVIDIA 推动、受 NDA 约束的合作。NVIDIA 的意图是先与直接贡献者一起开发和审查技术，再把成熟部分贡献给合适的开放行业标准论坛（例如 NVMe、PCI-SIG、SNIA、OCP 和 JEDEC）。

## NVIDIA Storage-Next Program 范围

Storage-Next 项目的覆盖范围包括：

- 向量数据库、图神经网络、关系图、大规模 embedding 数据、推理上下文内存及相关 GPU 中心化数据访问模式的 AI 工作负载需求。
- 面向大规模细粒度、高吞吐、稀疏访问的存储架构。
- SSD 与介质需求，包括每瓦 IOPS、每美元 IOPS、功耗、耐久性、外形规格、页大小行为、缓冲和机密计算。
- 存储控制器与存储服务器参考概念，包括基于 PCIe 交换芯片的设计、本地 NVMe 访问、远端存储访问和解耦存储模型。
- 面向规模化加速数据访问的软件接口与中间件，以及编排、遥测、性能剖析，和与 Kubernetes、Slurm、文件系统、存储供应商的集成。
- 多租户隔离、保护和与机密计算相关的存储机制，包括 IDE、TDISP、访问控制与可信设备交互。
- 面向标准的工作，包括可能成熟为 NVMe、PCI-SIG、SNIA、OCP 或相关行业论坛讨论的议题。

## 对企业与超大规模云客户的价值

- Storage-Next 参与者正在提供反馈，帮助塑造新的存储架构、存储基础设施、组件，以及与现有产品之间的互操作性。
- 这些直接反馈让产品方案落在真实使用模型和现实约束上。

## Storage-Next 不包含的内容

- NVIDIA 或参与公司对产品作出背书或产品承诺。
- 在公开参考架构或标准出现之前，公开宣称某家伙伴“实现了 Storage-Next”。
- 在已批准的 Storage-Next 协作渠道之外披露受 NDA 保护的材料。
- 在现有方案已经满足客户与工作负载需求时，替换现有存储接口。

## 贡献领域

Storage-Next 参与者贡献技术输入、实验、实现经验、需求和生态视角：

- 工作负载与使用模型：参与者帮助细化 AI 工作负载需求，包括向量数据库、图神经网络、embedding 数据、推理上下文内存和大规模稀疏数据访问。
- 存储架构：评估与多种存储产品相关的拓扑，重点看互操作性、连接性和性能。
- 介质需求：介质、SSD 和控制器贡献者帮助定义容量、IOPS、功耗、耐久性、页粒度、外形规格及数据管理增强方面的需求与可行性。
- 保护与多租户：贡献者帮助评估访问控制、机密计算、遥测和安全共享模型。
- 标准：一旦技术内容足够成熟、可进入公共论坛工作，参与者会帮助把使用场景、需求和实现结论发展并整理为候选标准议题。

## Storage-Next 生态

Storage-Next 参与者是 AI 存储生态各环节的直接贡献者。

| 类别 | 参与者 |
| --- | --- |
| 应用 | Institute of Science Tokyo、Promotech、Taiwan National Tsing Hua University |
| 组件 | Astera Labs、Broadcom、Microchip |
| 超大规模云厂商 | CoreWeave、Google、IBM |
| 介质与控制器 | FADU、InnoGrit、KIOXIA、Marvell、Microchip、Micron、Phison、Samsung、Sandisk、ScaleFlux、Silicon Motion、SK Hynix、Smart IOPS、Solidigm |
| OEM | AIC、ASUS、Dell、Giga Computing、H3P、HPE、Supermicro、Wiwynn |
| 存储 | DDN、Dell、Everpure、Hitachi Vantara、HPE、IBM、NetApp、VAST Data、WEKA |

## 参与方式

Storage-Next 参与者可以获得协作材料、技术演讲、实验、表征数据、设计讨论以及与其他参与者互动的机会。参与方式包括 NVIDIA 定期主持的技术讨论、参与者演讲、对需求和架构提案的反馈，以及在专有信息更适合在多方论坛之外处理时进行的私密后续讨论。

参与公司会直接贡献技术洞察、工作负载需求、实现经验或生态视角。

## 标准化路径

Storage-Next 的设计目标，是在公开标准化之前先让技术议题成熟。当想法足够具体后，NVIDIA 和生态贡献者可能会把它们带到合适的开放标准组织或行业论坛。候选领域包括与 NVMe 相关的访问控制和优先级、PCIe 安全与设备接口机制、存储遥测、软件接口，以及更广泛的 AI 存储架构需求。

## 准备开始了吗？

原文此句仍为模板占位内容：To learn more lorem ipsum, visit nvidia.com/loremipsum。

## 版权与商标

© 2026 NVIDIA Corporation。保留所有权利。NVIDIA、NVIDIA logo、Clara、CUDA、DGX、DGX SuperPOD、IndeX 和 Triton 是 NVIDIA Corporation 在美国及其他国家/地区的商标和/或注册商标。其他公司和产品名称可能是其关联公司的商标。所有其他商标均归各自所有者所有。

© 2026 NVIDIA Corporation 及关联公司。保留所有权利。NVIDIA、NVIDIA logo、SCADA 和 Storage-Next 是 NVIDIA Corporation 及关联公司在美国及其他国家/地区的商标和/或注册商标。其他公司和产品名称可能是其各自所有者的商标。

## 参考资料

[NVIDIA Storage-Next Program Overview（官方 PDF）](https://dam-cdn.nvd.orangelogic.com/AssetLink/l3m4pq075l33wyqo425wa2g8f758e23w.pdf)
