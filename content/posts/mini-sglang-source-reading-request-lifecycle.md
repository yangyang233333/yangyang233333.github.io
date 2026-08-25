---
title: "Mini-SGLang 源码阅读（一）：一次 LLM 请求如何穿过推理引擎"
date: 2026-08-25T20:30:00+08:00
draft: false
tags: ["Mini-SGLang", "LLM Serving", "推理引擎", "源码阅读"]
categories: ["大模型推理"]
description: "从 Prefill、Decode、Continuous Batching 和多进程流水线出发，梳理 Mini-SGLang 一次请求的完整执行流程。"
---

大模型推理框架并不只是执行一次 `model.forward()`。在线服务面对的是持续到达、长度不同、生成进度不同的请求，它必须同时解决文本编解码、动态批处理、KV Cache、GPU 执行和流式返回。

Mini-SGLang 把这些问题压缩在一套相对紧凑的代码中。本文先不进入具体优化，而是建立阅读后续源码所需的整体模型：**一次请求如何从 HTTP 文本进入系统，经过 Prefill 和多轮 Decode，最后以流式文本返回。**

本文基于 Mini-SGLang 提交 `9a91cfa`。

## 一、推理为何分成 Prefill 和 Decode

输入 prompt 包含多个 token。模型第一次执行时，需要同时处理全部输入 token，并为每一层生成 Key、Value。这一阶段称为 Prefill。

```text
输入： [t0, t1, t2, t3]
计算： 同时处理多个位置
产物： 最后位置的 logits + 四个位置的 KV Cache
```

接下来每轮只生成一个 token。已有 token 的 K、V 不应重复计算，只需读取缓存并计算新 token：

```text
已有： [t0, t1, t2, t3]
第 1 轮 Decode：输入 t3，生成 t4
第 2 轮 Decode：输入 t4，生成 t5
第 3 轮 Decode：输入 t5，生成 t6
```

因此两阶段的计算形态不同：Prefill 计算量大、输入长度不等；Decode 单次计算小，但会循环很多轮。现代推理引擎通常分别为两者设计调度与 Attention kernel。

## 二、为什么不能简单逐请求运行

如果每个请求独占 GPU，短请求之间会留下大量空闲时间。静态批处理也不适合在线服务：必须等待整批请求到齐，并且整批通常要等最长请求结束。

Continuous Batching 的做法是每一轮重新组织 batch：

```text
时刻 0：A、B 做 Prefill
时刻 1：A、B 做 Decode，C 到达
时刻 2：A、B 做 Decode；C 做 Prefill
时刻 3：A 完成，B、C 做 Decode，D 加入
```

请求完成后立即退出，等待请求可以随时进入。Scheduler 因此成为推理引擎真正的控制中心。

## 三、Mini-SGLang 的进程拓扑

`server/launch.py` 启动四类角色：

```text
                     ZeroMQ
用户 -> API Server ----------> Tokenizer × N
                                 |
                                 v
                          Scheduler Rank 0
                           /      |      \
                      Rank 1   Rank 2   Rank 3
                           \      |      /
                         Tensor Parallel
                                 |
                                 v
                            Detokenizer
                                 |
                                 v
                           API Server -> 用户
```

- API Server 接收 HTTP 请求并维护流式响应。
- Tokenizer 把字符串编码为 token IDs。
- 每个 TP rank 有一个 Scheduler 和一个 Engine。
- Rank 0 接收请求并同步给其他 rank。
- Detokenizer 把输出 token 增量还原为文本。

控制消息走 ZeroMQ；多 GPU 的张量通信走 NCCL 或 PyNCCL。

## 四、进程如何启动

入口 `python/minisgl/__main__.py` 调用 `launch_server()`。`server/launch.py` 先解析参数，再为每个 TP rank 创建 Scheduler 进程：

```python
for rank in range(world_size):
    rank_args = replace(
        server_args,
        tp_info=DistributedInfo(rank, world_size),
    )
    mp.Process(target=_run_scheduler, args=(rank_args, ack_queue)).start()
```

随后启动一个 Detokenizer 和多个 Tokenizer。主进程通过 `ack_queue` 等待后端全部就绪，再让 FastAPI 对外服务。

这里有两个值得注意的选择。

第一，使用 `spawn` 而不是 `fork`。CUDA 运行时不适合在初始化后直接 fork，独立启动进程更安全。

第二，每个 GPU 都有完整 Scheduler，而不是一个中央 Scheduler 远程控制多个 GPU。Rank 0 广播相同请求后，各 rank 独立构造相同 batch，模型层只同步必要张量。

## 五、API Server 如何跟踪流式请求

`server/api_server.py` 中的 `FrontendManager` 保存：

- 请求 ID 计数器；
- 每个请求对应的异步队列；
- 发往 Tokenizer 的 ZMQ 队列；
- 来自 Detokenizer 的回复；
- 后端健康状态。

用户请求到达后，Frontend 创建 `UserMsg`，并不断从对应队列读取 `UserReply`。OpenAI 流式接口再把它包装为 SSE chunk。

这说明流式返回不是一次 HTTP 请求等待最终字符串，而是贯穿整个后端的数据流：

```text
生成一个 token
 -> Rank 0 发给 Detokenizer
 -> 得到新增文本
 -> FrontendManager 找到请求队列
 -> SSE 立即返回客户端
```

如果客户端断开，Frontend 发送 Abort 消息，Scheduler 释放该请求占用的槽位和 KV Cache。

## 六、Tokenizer 为什么独立成进程

Tokenizer 的工作主要发生在 CPU：解析消息、应用 chat template、编码字符串。高并发下，它可能与 Python API Server 争抢 CPU 和 GIL，因此 Mini-SGLang 允许启动多个 Tokenizer worker。

`tokenizer/server.py` 的 `tokenize_worker()` 同时支持编码和解码角色。编码路径将 `UserMsg` 转成 `TokenizeMsg`；解码路径维护每个请求此前已产生的 token，并执行增量 decode，避免反复解码完整序列。

独立 Detokenizer 还有一个好处：Scheduler 只处理整数 token，不需要知道 Unicode 拼接、特殊 token 和文本停止条件。

## 七、请求进入 Scheduler 后发生什么

Tokenizer 产生的消息包含：

- 请求 ID；
- 输入 token IDs；
- 最大输出长度；
- temperature、top-k、top-p；
- 是否忽略 EOS。

Scheduler 将其转换为 `PendingReq`，先放入等待队列。之后每次调度循环依次处理：

```text
接收新消息
 -> 回收已经完成的 GPU 结果
 -> 更新请求状态
 -> 尝试组织 Prefill batch
 -> 否则组织 Decode batch
 -> 准备 page table 和 Attention metadata
 -> Engine.forward_batch()
 -> 异步等待下一轮结果
```

这就是 Overlap Scheduling 的基础：CPU 在处理上一轮 GPU 输出并准备下一批元数据时，GPU 可以继续计算另一批任务。

## 八、Req、Batch 与 Context

`core.py` 定义了三个贯穿全系统的对象。

`Req` 表示单个请求，记录输入 token、已缓存长度、当前长度、最大长度、采样参数和缓存句柄。最关键的三个长度是：

```text
cached_len：已有多少 token 的 KV 可以直接复用
device_len：GPU 逻辑上已经处理到哪里
max_device_len：输入长度 + 最大输出长度
```

`Batch` 是一次模型执行的单位，分为 `prefill` 和 `decode`。Scheduler 为它补充输入 token、position、KV 写入位置和 Attention metadata。

`Context` 保存当前 Batch、Attention backend、KV Cache 和 GPU page table。模型层通过全局 Context 获取运行时信息，因此模型定义不需要把十几个调度参数逐层传递。

## 九、Engine 执行一次 Batch

`engine/engine.py` 的 `forward_batch()` 可以概括为：

```python
with self.ctx.forward_batch(batch):
    if self.graph_runner.can_use_cuda_graph(batch):
        logits = self.graph_runner.replay(batch)
    else:
        logits = self.model.forward()

next_tokens = self.sampler.sample(logits[: batch.size], args)
```

模型执行前，Scheduler 已经完成了三件事：

1. 指定本轮真正输入哪些 token；
2. 给新产生的 K、V 分配物理位置；
3. 构造查询历史 K、V 所需的页表和 metadata。

Engine 因而专注于 GPU 执行：模型前向、CUDA Graph 和采样。

## 十、请求何时结束

每轮生成后，Scheduler 检查：

- 是否生成 EOS；
- 是否达到 `max_tokens`；
- 是否收到 Abort；
- 是否发生错误。

完成的请求从 Decode batch 移除，其前缀和 KV Cache 根据缓存策略处理。使用 Radix Cache 时，可复用前缀继续留在缓存树中；不能保留的物理页返回空闲池。

## 十一、阅读主线

理解整体流程后，推荐按以下顺序进入源码：

1. `python/minisgl/core.py`：请求、Batch 和运行时上下文；
2. `python/minisgl/scheduler/scheduler.py`：主调度循环；
3. `python/minisgl/scheduler/prefill.py`：Chunked Prefill；
4. `python/minisgl/scheduler/cache.py`：页表和缓存分配；
5. `python/minisgl/kvcache/radix_cache.py`：前缀复用；
6. `python/minisgl/engine/engine.py`：GPU 执行；
7. `python/minisgl/attention`：Attention 后端；
8. `python/minisgl/layers`：Tensor Parallel 模型层。

下一篇从 Scheduler 主循环出发，分析 Continuous Batching、Prefill/Decode 选择和 Overlap Scheduling 如何落到代码中。

## 参考源码

- `docs/structures.md`
- `python/minisgl/server/launch.py`
- `python/minisgl/server/api_server.py`
- `python/minisgl/tokenizer/server.py`
- `python/minisgl/scheduler/scheduler.py`
- `python/minisgl/core.py`
- `python/minisgl/engine/engine.py`
