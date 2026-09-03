---
title: "Tutti 源码阅读（一）：StorageRuntime 如何组织 KV Cache I/O"
date: 2026-09-03T17:35:00+08:00
draft: false
tags: ["Tutti", "源码阅读", "StorageRuntime", "KV Cache", "vLLM"]
categories: ["技术"]
---

Tutti 的论文强调 GPU 主导 I/O，但应用首先接触的不是 NVMe doorbell，而是一套稳定的 `StorageRuntime` API。它负责把 URI、文件 extent、GPU 内存、后端实例和异步请求组装起来，让上层无需知道底层究竟是单盘、条带化 NVMe，还是未来的其他传输路径。

本文阅读 Tutti v0.1.1 的运行时源码与设计文档，聚焦五个问题：公开 API 长什么样、`open` 如何解析对象、`register_memory` 为什么独立存在、`submit/wait` 如何路由，以及 vLLM connector 如何接入。

项目源码：[xPU-IO/Tutti](https://github.com/xPU-IO/Tutti)。

## 一、先看整体分层

当前代码把系统拆成以下层次：

```text
Application / vLLM Connector
             │
             ▼
      StorageRuntime
 open · register · submit · wait
             │
      ┌──────┴──────┐
      ▼             ▼
   Resolver       DataPath
 URI → target     IO → backend
      │             │
      └──── Binding ┘
             │
      local / striped NVMe
```

`StorageRuntime` 是唯一稳定的应用接口；Resolver、Binding 和 DataPath 是内部 SPI。这样做避免上层依赖特定 NVMe 驱动或 GPU 厂商实现。

代码中的核心概念如下：

| 概念 | 职责 |
|---|---|
| `StorageRuntime` | 装配组件并管理句柄生命周期 |
| `StorageTargetResolver` | 把 URI、offset、size 解析成后端可用 target |
| `DataPath` | 注册内存、提交 I/O、等待完成 |
| Binding | 校验 Resolver 与 DataPath 的私有 payload 契约 |
| Handle | 向应用隐藏内部对象、设备与队列身份 |

这是一套“控制面接口稳定、数据面实现可替换”的设计。

## 二、配置不是参数列表，而是组件图

Tutti 当前把配置明确分成两类：

- daemon 配置描述真实设备、控制器、队列预算和访问权限；
- application runtime 配置描述逻辑资源、Resolver、DataPath 和 Binding 的组合。

应用配置表达的是一张依赖图，而不是把 `/dev/nvme0n1` 一类事实硬编码进业务：

```yaml
storage:
  resources:
    - id: kv_store
      resolver: striped_file
      datapath: striped_local_nvme
```

运行时启动后向 daemon 请求资源，真实设备路径、BAR 大小、namespace 和队列授予以 daemon 响应为准。这样可以避免两个常见错误：

1. 应用配置与机器实际拓扑不一致；
2. 多个进程误以为自己独占同一控制器队列。

源码在 2026 年 8 月移除了旧式兼容字段，选择对混合配置 fail closed。这种做法牺牲平滑升级，但能防止存储映射被静默误解。

## 三、`open`：从 URI 到不可伪造的 TargetHandle

应用通常以 URI、偏移和长度描述持久化对象。`open` 的工作不是像 POSIX `open` 一样只返回文件描述符，而是建立一条经过验证的后端路由。

逻辑流程为：

```text
open(uri, range)
      │
      ▼
Resolver 解析文件 extent / stripe
      │
      ▼
生成 pair-private payload
      │
      ▼
Binding 校验 type-id + version
      │
      ▼
DataPath 接受并创建 target
      │
      ▼
返回 opaque TargetHandle
```

为什么需要 Binding？Resolver 与 DataPath 之间通常要传递比公共接口更具体的信息，例如物理 LBA、namespace、条带成员和 extent。若所有实现共享一个不断膨胀的公共结构，新增后端会污染整个 API。

Tutti 让配对组件使用私有 payload，但要求携带类型标识和版本。Binding 在装配阶段检查两端契约，不匹配就拒绝启动。这兼顾了扩展性与类型安全。

`open_batch` 则是 KV Cache 场景的重要优化。长上下文会打开大量对象，逐个解析会产生重复锁、分配与路径遍历；批量接口让 Resolver 一次处理整个对象集合，并为后续 GPU I/O 建立足够大的批次。

## 四、`register_memory`：把昂贵工作移出快路径

NVMe 控制器不能直接理解 CUDA 虚拟地址。GPU buffer 在参与 DMA 前，需要被固定并转换为设备可访问的地址；长 buffer 还要生成 PRP 映射。

因此 Tutti 把内存注册设计为独立生命周期：

```text
cudaMalloc / framework KV allocation
               │
               ▼
register_memory(buffer, size)
               │
      pin GPU pages + DMA map
               │
      cache PRP / registration
               │
               ▼
       MemoryHandle
```

一次注册可以服务多次 I/O。`submit` 只携带句柄、偏移和长度，不在每次请求中重新 pin 页和建立映射。

该设计对性能和安全都重要：

- 注册缓存降低重复映射成本；
- handle 记录内存归属和有效范围；
- 提交时可以检查越界、后端归属和设备一致性；
- 注销时有明确位置等待未完成请求并释放资源。

## 五、`submit`：按句柄身份路由，而不是让调用者选后端

一个运行时可以同时装配多个 DataPath。应用提交 I/O 时，不应该再次提供字符串后端名，否则 target、memory 和 backend 很容易被错误组合。

Tutti 采用 opaque identity：句柄由运行时创建，内部携带所有者和路由身份。提交过程可概括为：

```text
submit(requests)
      │
      ├─ 校验 target handle
      ├─ 校验 memory handle
      ├─ 检查范围与操作类型
      ├─ 按 DataPath 分组
      └─ 交给对应 backend 批量执行
```

这种“由对象身份决定路由”的方式比调用者传 `backend="nvme0"` 更可靠。它也使单盘与条带化后端拥有相同的公共调用形式。

请求是异步的。`submit` 返回 operation handle，`wait` 再观察完成状态。公开接口隐藏具体完成机制：某个 DataPath 可以由 GPU 轮询 NVMe CQ，另一个未来实现也可以使用 CPU 或网络完成队列。

## 六、失败语义：批量执行也要逐请求可见

批量 I/O 最危险的实现方式是只返回一个总成功值。只要一个 extent 越界、设备错误或映射失效，调用方就无法知道哪些 KV block 可用。

Tutti 的运行时强调 per-request status 和 fail-closed：

- 解析或 binding 不匹配时拒绝创建有效 handle；
- 提交前检查句柄所属 runtime 与 DataPath；
- 批内每个请求保留独立状态；
- 无法确认成功的请求不能被当作有效缓存命中。

对 KV Cache 来说，错误数据通常比 miss 更危险。miss 还能回退到 prefill，错误 KV 则可能造成不可见的推理结果污染。因此 fail closed 是正确取舍。

## 七、条带化如何藏在统一 API 后面

`striped_local_nvme` 可以把一个逻辑对象按 tensor-sized stripe 分布到最多四块 NVMe。上层仍提交连续的逻辑区间：

```text
logical object
[ stripe 0 ][ stripe 1 ][ stripe 2 ][ stripe 3 ]
      │           │           │           │
    NVMe 0      NVMe 1      NVMe 2      NVMe 3
```

Resolver 负责把文件或逻辑范围解析成各成员设备的 extent；DataPath 在一个 fused GPU kernel 中完成 fan-out。应用不需要手动拆成四组请求，也不需要分别等待四个设备。

这是运行时抽象的实际价值：条带策略进入后端，KV 管理层仍只看对象和逻辑 offset。

## 八、vLLM Connector 位于哪里

仓库中的 vLLM 集成大致分成三层：

```text
vLLM KV Connector adapter
            │
        Python engine
 chunk index · backend · lifecycle
            │
      Python/C++ binding
            │
       StorageRuntime
```

Connector 处理的是推理框架语义：哪些 token block 可保存、哪些 block 命中、何时触发 load/save。Engine 将 KV block 编排成 Tutti 可提交的对象与内存范围，C++ binding 再调用运行时。

GPU KV layout 与持久化 layout 往往并不一致，因此集成中还包含 CUDA kernels，用于显存中的 gather/scatter 和格式转换。Tutti 的 NVMe runtime 负责搬运，但“哪些字节构成某个 vLLM block”仍由 connector 层解释。

这条边界很重要：

- vLLM adapter 不应该知道 NVMe SQE；
- DataPath 不应该知道 token 或 attention block；
- 中间 engine 把两种对象模型连接起来。

## 九、源码设计的优点与代价

运行时层最值得借鉴的设计有三点：

1. **公开 API 小**：`open`、`register`、`submit`、`wait` 覆盖完整生命周期；
2. **SPI 明确**：Resolver 负责命名，DataPath 负责移动，Binding 负责私有契约；
3. **身份驱动路由**：调用者不能随意拼接后端参数，错误尽早暴露。

代价也很明显：

- 配置图、daemon 分配和 Binding 增加了理解成本；
- handle 生命周期必须和 GPU stream、KV block 生命周期严格协调；
- 文件 extent 变化会破坏持久 target，部署需限制 truncate、重写和重分配；
- Python connector、C++ runtime、GPU kernel 和内核驱动形成较长的调试链。

## 十、总结

Tutti 的 `StorageRuntime` 并不直接等于论文中的 GPU I/O kernel。它承担的是更基础的职责：把不稳定的设备、文件布局和后端实现，封装成可验证、可批量、可异步的对象生命周期。

一次请求的主线是：

```text
URI → Resolver → Binding → TargetHandle
GPU Buffer → register → MemoryHandle
Target + Memory → submit → DataPath → OperationHandle → wait
```

理解这条链后，底层 GPU 如何真正写 SQE、敲 doorbell 和等待 CQE 就有了清晰入口。下一篇将继续下潜到 `snvme`、`libnvm`、PRP 映射和多盘 fused kernel。
