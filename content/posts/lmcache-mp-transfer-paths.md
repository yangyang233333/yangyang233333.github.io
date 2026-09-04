---
title: "LMCache MP 模式详解：CUDA IPC、共享内存与 Pickle 如何传输 KV Cache"
date: 2026-09-04T20:30:00+08:00
draft: false
tags: ["LMCache", "vLLM", "KV Cache", "CUDA IPC", "共享内存", "大模型推理"]
categories: ["大模型推理"]
summary: "从 PagedAttention 的 Gather/Scatter 出发，解释 LMCache 多进程模式中 CUDA IPC、SHM 和 Pickle 三条 KV Cache 传输路径的工作原理、复制成本与适用场景。"
---

大模型推理引擎通常把 KV Cache 保存在 Worker 进程内部。这样实现简单，却让缓存生命周期与推理进程绑定：Worker 重启或崩溃时，已计算的缓存也可能随之丢失；多个 Worker 想共享缓存，也需要更复杂的协调机制。

LMCache 的 **MP（Multiprocess）模式**把缓存管理拆成独立服务：vLLM Worker 专注模型执行，LMCache MP Server 负责 KV Cache 的存储、检索和复用。新的问题随之出现：两个进程不共享普通地址空间，体积巨大的 KV 张量如何高效跨越进程边界？

LMCache 提供三条主要路径：

- **CUDA IPC**：传递 GPU 内存句柄，由服务端访问 Worker 的显存；
- **SHM**：双方映射同一块共享内存；
- **Pickle + ZMQ**：序列化 KV 数据并通过消息通道发送。

三者的本质差异，是跨进程边界时传递的究竟是**内存访问权、共享内存位置，还是数据本身**。

# 一、MP 模式解决什么问题

传统部署中，模型执行和缓存管理位于同一个进程：

```text
vLLM Worker
├── 请求调度
├── 模型执行
└── KV Cache
```

MP 模式将其拆开：

```text
┌─────────────────┐             ┌────────────────────┐
│ vLLM Worker     │             │ LMCache MP Server  │
│                 │  控制与数据  │                    │
│ 模型推理         │◄───────────►│ KV Cache 管理       │
│ Paged KV Cache  │             │ L1 / L2 / 远端后端  │
└─────────────────┘             └────────────────────┘
```

这种解耦带来三点价值：

1. 缓存不再完全依附于单个推理 Worker 的生命周期；
2. 缓存分层、淘汰和远端存储可以由独立服务统一管理；
3. Worker 重启或多个 Worker 复用前缀时，缓存管理边界更清晰。

每条传输路径都支持两个方向：

- **Store**：Worker → LMCache Server；
- **Retrieve**：LMCache Server → Worker。

# 二、为什么传输前需要 Gather

vLLM 使用 PagedAttention 管理 KV Cache。一个请求的 KV 不一定连续存放，而是分散在多个固定大小的 GPU Block 中，Block Table 记录逻辑 token 与物理 Block 的对应关系。

```text
一个请求的逻辑 KV
        │
        ▼
Block Table: [17, 4, 91, 28]
        │
        ▼
GPU 中分散的物理块
[Block 17] [Block 4] [Block 91] [Block 28]
```

外部缓存通常需要连续的数据表示，因此 Store 前要执行 **Gather**：根据 Block Table，把分散的分页 KV 收集到连续 Buffer。Retrieve 时执行相反的 **Scatter**，把连续 KV 写回 vLLM 分配的分页 Block。

```text
Store:    Paged KV ── Gather ──► Contiguous Buffer
Retrieve: Paged KV ◄─ Scatter ── Contiguous Buffer
```

所以一条完整的 Transfer Path 包含两个问题：

1. 如何在分页 KV 和连续 Buffer 之间 Gather/Scatter；
2. 如何让 Buffer 或它的访问权跨越进程边界。

“使用 IPC”并不意味着整个过程零拷贝。即使跨进程阶段不搬运 Payload，分页布局转换和 GPU/CPU 层级迁移仍可能发生。

# 三、CUDA IPC：传句柄，不传 KV 字节

普通 GPU 指针只在创建它的进程中有效。另一个进程即使拿到相同数值，也不能直接访问对应显存。CUDA IPC 允许内存所有者导出一个 IPC Handle，另一个进程导入后获得对同一底层 GPU Allocation 的有效映射。

```text
Worker GPU Allocation
        │ export
        ▼
CUDA IPC Handle ─────────► LMCache Server
                                │ import
                                ▼
                         同一块 GPU 内存
```

LMCache 使用 `CudaIPCWrapper` 封装 GPU Tensor。除了 IPC Handle，它还要携带 Shape、Dtype、布局等元数据，使 Server 知道如何解释内存。CUDA Event 则用于同步两个进程的 GPU 操作，避免一方尚未写完，另一方就开始读取。

## Store 路径

```text
vLLM 分页 KV（GPU）
        │ Server 通过 CUDA IPC 访问
        ▼
连续 GPU Staging Buffer
        │ GPU → CPU
        ▼
LMCache L1（CPU RAM）
```

这一过程通常包含两次主要复制：

1. 分散的 GPU KV Block Gather 到连续 GPU Staging Buffer；
2. Staging Buffer 从 GPU 复制到 LMCache 的 CPU L1。

ZMQ 可以传递控制消息、IPC Handle 和元数据，但不会承载完整 KV Payload。

## Retrieve 路径

Retrieve 的方向相反：LMCache 从 L1 取出连续 KV，写入可由 IPC 访问的 GPU Buffer，再 Scatter 到 vLLM 的分页 KV Block。

CUDA IPC 路径适合 NVIDIA GPU 同机部署，优势是避免 CPU 序列化和 Socket Payload 复制，并且可以结合 CUDA Stream/Event 实现异步流水。但它通常要求两个进程能够访问同一块 GPU，且必须严格管理 Allocation 生命周期和同步关系；它不是跨机器传输协议。

# 四、SHM：双方映射同一块 CPU 内存

在非 CUDA 环境中，可以利用 Linux Shared Memory。Worker 与 Server 的虚拟地址不必相同，只要它们映射到同一组物理页，就能共同访问一个 Buffer。

```text
Worker 虚拟地址 ─┐
                  ├──► Shared Physical Pages (/dev/shm)
Server 虚拟地址 ─┘
```

理想的 Store 路径是：

```text
vLLM 分页 KV
      │ Gather
      ▼
共享 L1 Buffer（/dev/shm）
```

Gather 的目标就是 LMCache 使用的共享 L1 Buffer，因此 Server 不需要再从 Worker 私有内存复制一遍。使用设备专用 C Ops 时，文章将其概括为一次主要复制；默认 Python Fallback 可能需要两次。

因此，SHM 的准确理解不是“完全零拷贝”，而是：

> 消除两个进程私有内存之间的额外 Payload 搬运，但 Gather/Scatter 本身仍然存在。

SHM 适合 CPU-only 测试以及能够接入共享内存路径的非 CUDA 平台。部署时需要关注：

- `/dev/shm` 是否足够大；
- 容器的 IPC Namespace 是否允许双方共享；
- Buffer 槽位分配、并发访问和生命周期管理；
- NUMA 位置是否让 Worker 和 Server 产生远端内存访问；
- Store 是否同步阻塞 Worker。

例如 Docker 默认的共享内存容量通常不适合大型 KV Buffer，应按模型和并发量显式规划 `--shm-size`，而不是机械使用固定值。

# 五、Pickle + ZMQ：直接传输 KV Payload

当 CUDA IPC 不可用，双方又不能共享 L1 Buffer 时，LMCache 使用通用回退路径：先把 KV Gather 到 Worker 的 CPU Chunk，再序列化并通过 ZMQ 发送。

```text
Paged KV
   │ Gather
   ▼
Worker CPU Chunk
   │ pickle.dumps
   ▼
Serialized Bytes
   │ ZMQ
   ▼
Server Deserialize
   │ Write
   ▼
LMCache Private L1
```

文章将 Store 路径概括为四个阶段：

1. Gather；
2. Serialize；
3. Deserialize；
4. Write to L1。

与前两条路径不同，Pickle 路径中完整 KV 字节确实经过 ZMQ。这使它不依赖 CUDA IPC 或共享内存映射，能作为 CPU、Intel XPU、Habana HPU 等环境的通用基础路径，但代价是更多 CPU 拷贝、序列化开销和消息通道压力。

还要注意安全边界：Python Pickle 不适合反序列化不可信来源的数据。该路径应运行在受信任的 Worker/Server 部署域中，不能把 ZMQ 接口直接暴露给不可信客户端。

# 六、ZMQ 是控制面，还是数据面

三条路径都可能使用 ZMQ，但用途并不完全相同。

| 路径 | 跨进程传递的主要内容 | ZMQ 是否承载完整 KV |
|---|---|---:|
| CUDA IPC | 控制消息、IPC Handle、Tensor 元数据 | 否 |
| SHM | 控制消息、共享区域标识、Tensor 元数据 | 否 |
| Pickle | 控制消息、序列化后的 KV 字节 | 是 |

例如 `REGISTER`、`PREPARE_STORE` 和 `COMMIT_STORE` 等消息属于控制面，用于协商资源、描述操作和确认状态。CUDA IPC 与 SHM 把大数据留在专用 Data Path 中；Pickle 路径则让 ZMQ 同时承担控制面和数据面。

# 七、三条路径如何选择

| 维度 | CUDA IPC | SHM | Pickle + ZMQ |
|---|---|---|---|
| 典型环境 | NVIDIA CUDA | 同机 CPU/非 CUDA | 通用回退 |
| 跨进程对象 | GPU IPC Handle | 共享内存映射 | 完整 KV 字节 |
| 序列化 | 不需要 | 不需要 | 需要 |
| Store 主要复制 | 约 2 次 | 理想约 1 次，Fallback 约 2 次 | 约 4 个阶段 |
| 异步能力 | 较强 | 当前 Store 偏同步 | 受序列化和消息传输限制 |
| 通用性 | 较低 | 中等 | 最高 |
| 预期性能 | 通常最好 | 通常居中 | 通常最慢 |

实际决策可以简化为：

```text
同机 NVIDIA GPU，并且 CUDA IPC 可用？
├── 是：优先 CUDA IPC
└── 否
    ├── Worker 与 Server 能映射同一共享内存？
    │   ├── 是：优先 SHM
    │   └── 否：使用 Pickle + ZMQ
```

复制次数只是理解实现的模型，不等于真实性能结论。最终表现还受 KV Chunk 大小、PCIe/NVLink、Pinned Memory、NUMA、Gather/Scatter Kernel、并发流水和 ZMQ 配置影响，应以目标机器上的吞吐与尾延迟测试为准。

# 八、它与 NVMe、远端缓存是什么关系

CUDA IPC、SHM 和 Pickle 解决的是 **vLLM Worker 到 LMCache MP Server** 的前端传输问题；NVMe、远端对象存储或分布式缓存解决的是 **MP Server 把数据放在哪里**。两者处于不同层次：

```text
vLLM Worker
    │ CUDA IPC / SHM / Pickle
    ▼
LMCache MP Server
    │
    ├── CPU RAM
    ├── Local NVMe
    └── Remote Storage
```

因此，即使 LMCache 后端使用 NVMe，Worker 与 MP Server 之间仍然需要选择一条 Transfer Path。反过来，使用 CUDA IPC 也不代表缓存只能留在 GPU；Server 仍可继续把数据下沉到 CPU、NVMe 或远端后端。

# 九、工程上最值得关注的两个方向

第一是 **异步 SHM Store**。同步 Store 会让 Worker 等待复制完成；如果能像 CUDA 路径一样通过事件和状态机实现“提交后继续执行”，就有机会把缓存写入与模型计算重叠起来。

第二是 **Hybrid KV Layout 的跨平台支持**。不同模型可能具有不同的 KV Cache Group 和布局。CUDA 路径支持某种布局，不代表 SHM 与 Pickle 可以自动复用；Gather/Scatter、元数据描述和 Server 存储格式都需要理解相同的布局语义。

# 总结

LMCache MP 模式把推理执行与 KV Cache 管理解耦，但也引入了跨进程数据搬运问题。理解三条路径时，只需抓住一个核心：

- CUDA IPC 传递 GPU 内存访问句柄；
- SHM 让双方映射同一块 CPU 内存；
- Pickle + ZMQ 直接传递序列化后的 KV 字节。

无论选择哪条路径，PagedAttention 带来的 Gather/Scatter 都是不可忽略的一环。真正的优化目标不是抽象地追求“零拷贝”，而是减少关键路径上的复制、同步和阻塞，并让传输与模型计算尽可能重叠。

参考资料：

- [Understanding LMCache MP Mode Transfer Paths: A Beginner's Guide](https://blog.lmcache.ai/en/2026/06/15/understanding-lmcache-mp-mode-transfer-paths-a-beginners-guide/)
- [LMCache GitHub Repository](https://github.com/LMCache/LMCache)
- [PagedAttention Paper](https://arxiv.org/abs/2309.06180)
