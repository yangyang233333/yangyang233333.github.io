---
title: "Tutti 论文阅读：让 SSD KV Cache 真正服务长上下文推理"
date: 2026-09-03T17:30:00+08:00
draft: false
tags: ["Tutti", "KV Cache", "LLM Serving", "NVMe", "GPU Direct Storage"]
categories: ["技术"]
---

长上下文推理会生成大量 KV Cache。HBM 放不下，DRAM 成本高，NVMe SSD 容量大却经常因为 I/O 控制开销而无法进入关键路径。Tutti 的结论是：问题不只是“数据能否从 SSD 直达显存”，而是**谁负责把数以千计的 KV 块转成设备请求、提交队列并处理完成**。

Tutti 将这条控制路径交给 GPU：CPU 按层启动 I/O kernel，GPU 批量解析 KV 对象、生成 NVMe 命令、敲 doorbell 并轮询完成，同时利用 Transformer 层之间的计算空隙隐藏读取与写回。论文报告，在其测试环境中，相比 SSD-backed LMCache，严格 SLO 下 TTFT 降低 78.3%，可承载请求率提高 2 倍，服务成本降低 27%。

本文阅读论文 [Tutti: Making SSD-Backed KV Cache Practical for Long-Context LLM Serving](https://arxiv.org/abs/2605.03375)，重点解释它为什么需要 GPU-centric I/O、系统如何工作，以及实验结论应该怎样理解。

## 一、SSD KV Cache 为什么难用

自回归模型在 prefill 阶段为每层生成 Key 和 Value。后续请求若共享前缀，可以直接载入已有 KV Cache，省去重复计算。

```text
请求 A：系统提示词 + 文档 + 问题 A
请求 B：系统提示词 + 文档 + 问题 B
                     └── 可复用前缀 ──┘
```

KV Cache 容量随上下文长度、层数、并发和 KV head 数增长。分层存储因此很自然：

```text
GPU HBM  →  CPU DRAM  →  NVMe SSD
低延迟       中等容量       大容量、低成本
```

但 SSD 方案面临一个结构性矛盾。推理框架按 block 管理 KV Cache，一个长前缀会展开成大量小块；SSD 擅长保持较深队列下的并行吞吐，而 CPU 逐块解析、提交和回收请求会形成串行控制瓶颈。

GPUDirect Storage 可以让数据绕过主机内存，通过 DMA 在 NVMe 和 HBM 之间移动，但它并不自动消除 CPU 上的请求组织工作。换言之：

```text
GDS 主要缩短数据路径
Tutti 进一步重构控制路径
```

当加载缓存所需时间不低于重新 prefill，SSD 缓存即使命中也没有意义。Tutti 要解决的是这个盈亏平衡点。

## 二、核心思想：CPU 启动，GPU 执行 I/O

传统 CPU-centric 路径可以简化为：

```text
GPU 请求 KV
    ↓
CPU 遍历块映射、生成 I/O
    ↓
CPU 提交请求并处理完成
    ↓
NVMe DMA 到 HBM
```

Tutti 改为：

```text
CPU 准备批描述符并启动 GPU kernel
                  ↓
GPU 解析对象、生成 SQE、敲 NVMe doorbell
                  ↓
NVMe DMA ↔ HBM，GPU 轮询 CQE
```

CPU 没有彻底消失。它仍负责框架控制、元数据准备和 kernel launch，但不再介入每一个 NVMe 请求。一个 kernel 可以处理成千上万个 I/O，GPU 的线程并行度被用于填满 NVMe 队列。

论文的设计可以概括为三个部分。

## 三、GPU-native KV 对象

存储设备只理解地址、长度和 LBA，推理框架理解的是请求、层、K/V tensor 和 block。若直接暴露块接口，CPU 需要不断把上层语义翻译成碎片化请求。

Tutti 为 GPU 提供 KV 对象描述：一个逻辑对象可对应多个离散显存区域和 SSD extent。GPU kernel 批量读取对象元数据，再生成底层 I/O。

```text
KV Object
├── Layer 17 / Key
├── logical offset and length
├── GPU page fragments
└── SSD extents
        ↓ GPU 批处理
   多条 NVMe 命令
```

对象抽象有两个价值：

- 上层按 KV tensor 或层思考，不必管理每个块的设备命令；
- 下层获得足够大的批次，可以并行解析、合并并提交请求。

这里的关键不是把 KV Cache 伪装成普通文件，而是让存储运行时保留 LLM 数据布局的语义。

## 四、GPU 侧异步 NVMe 队列

NVMe 的 SQ/CQ 本来就是共享内存环。提交者写入 Submission Queue Entry，更新 doorbell；控制器执行 DMA 后写回 Completion Queue Entry。

Tutti 配合自定义 `snvme` 内核模块，在初始化阶段完成特权操作：创建队列、固定内存、建立 DMA 映射，并把受控的 doorbell 和队列暴露给运行时。稳态数据面由 GPU kernel 完成：

1. 根据对象元数据计算设备、LBA 和目标显存地址；
2. 并行写入多条 SQE；
3. 更新 NVMe doorbell；
4. 轮询 CQ phase bit；
5. 将完成状态归并到上层请求。

它类似 RDMA 中“内核建 QP、用户态写 WQE、用户态敲 doorbell”的模式，只是请求生产者从 CPU 用户线程变成了 GPU 线程。

## 五、利用层间空隙隐藏 I/O

仅仅让 GPU 提交 I/O 仍不够。I/O kernel 会与模型计算争用 SM、寄存器、显存带宽和 PCIe 资源。若调度粗糙，存储加速可能反过来拖慢推理。

Transformer 的执行具有稳定的逐层结构。Tutti 对层级执行进行 profiling，把下一层读取和上一层写回安排到资源较空闲的窗口：

```text
时间 ───────────────────────────────────▶
计算： [Layer N          ][Layer N+1        ]
读取：       [Load KV N+1]
写回：          [Store KV N]
```

理想情况下，应用看到的 I/O 延迟不是完整设备延迟，而是：

```text
可见 I/O 延迟 ≈ max(0, I/O 时间 - 可重叠计算时间)
```

这也解释了“性能接近 DRAM”的准确含义：SSD 的介质延迟并未变成 DRAM 延迟，而是并行提交与计算重叠减少了暴露给请求的等待时间。

## 六、论文实验怎么看

论文报告的主要结果包括：

| 指标 | 论文报告结果 |
|---|---:|
| 严格 SLO 下 TTFT | 相比 SSD-backed LMCache 降低 78.3% |
| 可承载请求率 | 提高 2 倍 |
| Serving cost | 降低 27% |
| KV 检索带宽 | 最高约 25.9 GB/s |

这些数字支持了两个判断：

- 多盘 NVMe 的介质带宽可以服务 KV Cache，真正困难的是软件栈能否提供足够并行度；
- 端到端收益来自对象批处理、GPU 提交和 overlap 的组合，而不是某一个独立优化。

但结果不能直接外推到所有机器。实际表现高度依赖：

- GPU、SSD 和 PCIe switch 的拓扑；
- SSD 数量、型号、温度与持续读写能力；
- Prefix 命中率和共享长度；
- Prompt 长度与模型 prefill 算力；
- I/O 与计算是否确实存在可重叠窗口；
- 内核模块、IOMMU 和 peer-memory 支持。

当请求前缀很短、几乎不共享，或 SSD 加载成本高于重算，Tutti 不会创造收益。

## 七、Tutti 与其他分层 KV 系统的关系

Tutti 与 LMCache、Mooncake 一类系统关注点相邻，但切入层不同。

| 系统能力 | 关注问题 |
|---|---|
| Prefix cache 管理 | 哪些 KV 可以复用、如何索引与淘汰 |
| 分布式 KV Cache | KV 在节点间如何传输和共享 |
| GPUDirect Storage | 数据如何绕过 CPU 内存直达 HBM |
| Tutti | GPU 如何批量控制 SSD I/O，并与逐层计算重叠 |

因此 Tutti 更适合作为推理系统下方的存储运行时，而不是独立完成请求路由、全局缓存策略和分布式调度。

## 八、工程代价

论文方案快，但并非无条件易用：

- 需要自定义 NVMe 内核模块和 peer-memory 支持；
- 需要应用进程连接拥有控制器的 daemon；
- SSD 文件布局要能解析到稳定的物理 extent；
- 部署必须关注 GPU 与 NVMe 的 PCIe 可达性；
- GPU kernel 轮询和提交本身也消耗 GPU 资源；
- 故障隔离、队列租约和进程退出清理比普通文件 I/O 更复杂。

它本质上是在用专用系统软件和更强部署约束，换取通用内核路径难以提供的低控制开销。

## 九、总结

Tutti 的重要性不在于首次提出“把 KV Cache 放进 SSD”，而在于指出 SSD-backed KV Cache 的瓶颈已经从数据复制转移到 I/O 控制：

1. 用 GPU-native object 保留 KV Cache 的上层语义；
2. 用 GPU 并行生成和提交 NVMe 请求；
3. 用 layer-wise scheduling 把 I/O 藏进模型计算；
4. 让大容量 SSD 从离线存储变成推理关键路径上的缓存层。

从更广的系统趋势看，GPU 正从计算加速器变成主动的数据面处理器：它不仅消费数据，也开始直接驱动存储与网络设备。Tutti 是这一趋势在 LLM KV Cache 场景中的代表实现。

下一篇将进入源码，分析 `StorageRuntime`、Resolver、Binding 与 DataPath 如何把 URI、显存和 NVMe 后端组装成一次异步 I/O。
