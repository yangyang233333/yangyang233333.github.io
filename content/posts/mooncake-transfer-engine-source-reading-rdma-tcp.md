---
title: "Mooncake Transfer Engine 源码阅读（三）：RDMA、TCP 与请求完成状态机"
date: 2026-08-21T11:17:00+08:00
draft: false
tags: ["Mooncake", "Transfer Engine", "RDMA", "TCP", "异步 I/O", "源码阅读"]
categories: ["技术"]
---

Transfer Engine 的公共 API 很统一，但 RDMA 和 TCP 的实现差异很大。RDMA 需要 MR、QP、WR 和 CQ；TCP 需要连接、lane、消息 framing 和接收端主动拷贝。本文沿源码比较两条路径，并分析 Batch 状态如何完成。

阅读版本：

```text
Mooncake commit: 777cc7782417b6e554cf7c2d53210d0d8f89f5cc
```

## 一、Transport 抽象

`Transport` 基类统一定义：

```text
install / uninstall
registerLocalMemory / unregisterLocalMemory
submitTransfer
getTransferStatus
allocateBatchID / freeBatchID
```

它还定义 `TransferRequest`、`TransferStatus` 和内部 `BufferEntry`。

公共抽象要求每个后端回答三个问题：

1. 本地内存如何准备为可传输状态；
2. 请求怎样排队和执行；
3. 如何查询每个 task 的最终状态和字节数。

具体连接模型不属于公共 API。

## 二、RDMA 初始化

RDMA 后端主要位于：

```text
rdma_transport.cpp
rdma_context.cpp
rdma_endpoint.cpp
endpoint_store.cpp
worker_pool.cpp
```

安装阶段通常完成：

```text
枚举 RDMA devices / ports / GID
  -> 创建每张 HCA 的 context
  -> 建立 PD、CQ 等资源
  -> 启动 worker / poller
  -> 发布 NIC endpoint metadata
  -> 准备 endpoint store
```

Mooncake 支持多 HCA、多端口和 GPU 内存，因此一个逻辑请求可能被切到多个 rail 上并行发送。

## 三、RDMA 内存注册

CPU 内存一般通过 verbs 注册为 MR：

```text
addr + length
  -> ibv_reg_mr
  -> lkey：本地 SGE 使用
  -> rkey：远端 RDMA READ/WRITE 使用
```

GPU 内存则依赖 CUDA peer memory、DMA-BUF 或平台对应机制，最终也需要产生 HCA 可访问的 MR。

每张 HCA 可能需要独立注册，因此一个 BufferEntry 可能保存多份 MR 信息。注册结果发布到 SegmentDesc 后，远端才能选择对应 rail 的 rkey。

这解释了为何注册大块内存池比频繁注册小对象更高效：MR 注册涉及 pin page、驱动交互和页表建立，不适合位于每次 KV block 传输的热路径。

## 四、RDMA 提交链

简化调用链为：

```text
TransferEngineImpl::submitTransfer
  -> MultiTransport::selectTransport
  -> RdmaTransport::submitTransfer
  -> 验证本地与远端注册区间
  -> 取得或创建 RdmaEndpoint
  -> 将 request 切成 Slice
  -> WorkerPool 选择 rail / worker
  -> 构造 ibv_send_wr 与 ibv_sge
  -> ibv_post_send
  -> CQ poller 获得 completion
  -> 更新 task / batch 状态
```

切片的原因包括：

- 单个 WR 或 SGE 的长度限制；
- 跨越不同 MR 区间；
- 多 NIC striping；
- 拥塞和 worker 队列负载；
- 重试与 failover 粒度。

源码还处理“按顺序推进 slice”的场景。它用 continuation/trampoline 避免同步失败或同步完成导致递归层层增长，体现了异步状态机的工程细节。

## 五、Endpoint 生命周期

`RdmaEndpoint` 代表与远端某条 rail 的连接状态，负责 QP 建立、提交和错误处理。`EndpointStore` 缓存 endpoint，避免每次传输重新握手。

典型状态问题包括：

```text
首次连接尚未完成
远端重启导致旧 QP 失效
CQ 返回 transport error
某条 rail 不可用
多个线程同时请求重连
shutdown 时仍有连接任务
```

源码和测试包含 endpoint reestablish、state、async event drain、GID probe 等场景，说明 TE 把 RDMA 故障恢复视为核心能力，而不是只实现 happy path benchmark。

Endpoint cache 必须与 Segment metadata 版本协同：远端重启后，即使名称相同，旧 endpoint 和旧 rkey 都可能无效。

## 六、WorkerPool 与多轨并行

`WorkerPool` 把提交任务分配给不同 worker 和 NIC rail。设计目标是：

- 避免所有请求竞争单个提交锁；
- 利用多张 HCA 聚合带宽；
- 将 CQ polling 与提交分散到 CPU core；
- 统计 NIC load；
- 在 rail 故障时停止或迁移流量。

上层提交的是一个 batch，底层可能变成：

```text
Task 0
├── Slice 0 -> HCA 0 / QP A
├── Slice 1 -> HCA 1 / QP B
└── Slice 2 -> HCA 0 / QP C
```

只有所有 slice 都完成，task 才能标记 COMPLETED。任何不可恢复错误都要合并到 task 和 batch 状态。

## 七、Completion 如何回到 Batch

提交成功只表示请求进入后端，不表示数据已经到达。

完成链为：

```text
CQE
  -> 根据 wr_id 找回 Slice/Task 上下文
  -> 检查 wc.status
  -> 累加 transferred_bytes
  -> 减少 pending slice 计数
  -> 最后一个 slice 完成
  -> 设置 task COMPLETED 或 FAILED
  -> 必要时触发 notify / continuation
```

`getTransferStatus(batch_id, task_id)` 查询的正是这个聚合状态。

释放 BatchID 前必须确保所有任务已结束，否则后端完成回调可能写入已释放状态。API 把 allocate/free 显式交给调用者，也是为了让生命周期边界清楚。

## 八、RDMA READ 与 WRITE

从调用者角度：

- WRITE：本地内存写到目标 Segment；
- READ：从目标 Segment 读到本地内存。

RDMA 单边操作的发起方掌握本地 lkey 和远端地址/rkey。远端 CPU 不需要为每次 payload 进入数据路径，但必须事先：

- 注册 MR；
- 发布 rkey；
- 保持内存有效；
- 处理连接和元数据更新。

所以“绕过远端 CPU”是指数据搬运阶段，不是整个系统完全没有远端控制面。

## 九、TCP 后端

TCP 后端遵守同一接口，但数据路径不同：

```text
submitTransfer
  -> 找到或建立 TCP session
  -> 将请求分配到 lane
  -> 发送控制头和 payload / 请求
  -> 对端线程接收
  -> memcpy 到注册目标区间
  -> 返回完成响应
  -> 本地更新 task 状态
```

`TcpTransport::submitTransfer()`、`submitTransferTask()` 和 `submitTransferTaskGroup()` 体现了任务分组与 lane 调度。

TCP 的优势是部署简单、兼容普通以太网；代价是：

- 更多 CPU 协议栈开销；
- 通常需要 CPU memcpy；
- 延迟和尾延迟高于配置良好的 RDMA；
- GPU buffer 可能需要 staging。

但 TCP 是很重要的可靠回退路径，也适用于不具备 RDMA 的开发环境。

## 十、为什么统一 API 仍然有价值

RDMA 和 TCP 后端内部完全不同，但上层仍然使用：

```text
Segment ID + offset
local address + length
READ / WRITE
BatchID
```

统一抽象使 Mooncake Store 可以：

- 在开发环境使用 TCP；
- 在生产集群切换 RDMA；
- 同时安装多种协议；
- 按 Segment 或 Buffer 选择后端；
- 保持 KV Cache 管理逻辑不变。

## 十一、其他后端如何嵌入

仓库中的其他 Transport 延续相同框架：

- NVLink / HIP / NCCL：节点内或 GPU 间传输；
- NVMe-oF：把持久存储包装成可读写 Segment，内部使用 cuFile；
- CXL：共享内存窗口；
- EFA/CXI/UB/Barex：不同云厂商或硬件互连；
- Device transport：由 GPU 侧发起或参与提交。

它们需要实现相同生命周期，但可以有完全不同的 endpoint、队列和完成模型。

## 十二、排障时应观察什么

一次请求卡住时，可以按层定位：

```text
请求是否选对 Transport
  -> 本地地址是否注册
  -> 远端 SegmentDesc 是否新鲜
  -> endpoint 是否连接成功
  -> 请求是否进入 worker queue
  -> post_send / socket send 是否成功
  -> completion 是否到达
  -> task pending count 是否归零
```

不要只看最终 `FAILED`。RDMA 问题常来自 GID、MTU、rkey、GPU MR、PCIe 拓扑或远端重启；TCP 问题则常见连接、lane 和接收端地址校验。

下一篇将阅读 TENT：为什么 Mooncake 在已有 MultiTransport 的基础上又引入 Runtime、SegmentManager、TransportSelector、ProgressWorker、AdmissionQueue 和 QoS contract。

## 参考源码

- `include/transport/transport.h`
- `src/transport/rdma_transport/rdma_transport.cpp`
- `src/transport/rdma_transport/rdma_endpoint.cpp`
- `src/transport/rdma_transport/rdma_context.cpp`
- `src/transport/rdma_transport/worker_pool.cpp`
- `src/transport/rdma_transport/endpoint_store.cpp`
- `src/transport/tcp_transport/tcp_transport.cpp`
