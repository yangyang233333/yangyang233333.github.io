---
title: "Mini-SGLang 源码阅读（二）：Scheduler、Continuous Batching 与 Chunked Prefill"
date: 2026-08-25T20:40:00+08:00
draft: false
tags: ["Mini-SGLang", "Scheduler", "Continuous Batching", "源码阅读"]
categories: ["大模型推理"]
description: "先解释在线推理调度的资源约束，再分析 Mini-SGLang 如何组织 Prefill、Decode 和 CPU/GPU 重叠执行。"
---

在线 LLM 调度器要回答的不是“下一个运行哪个进程”，而是：**这一轮把哪些请求、哪些 token 放入同一个 GPU batch，并确保显存、KV Cache 和计算预算都不超限。**

本文先建立 Continuous Batching、Chunked Prefill 与 Overlap Scheduling 的理论模型，再阅读 Mini-SGLang 的 Scheduler 实现。源码基于提交 `9a91cfa`。

## 一、调度器面对三种资源

LLM 请求至少消耗三类资源。

第一是计算量。Prefill 的计算大致随输入 token 数增长，Decode 每个请求每轮只增加一个 token。

第二是 KV Cache。一个请求即使本轮只计算一个 token，也必须继续持有全部历史 K、V。

第三是 batch 槽位和 CUDA Graph 形状。请求数、总 token 数和序列长度都会限制可执行 batch。

因此 Scheduler 需要同时维护两个预算：

```text
本轮计算预算：最多处理多少新 token
长期缓存预算：这些请求最多还会占多少 KV 页
```

只看当前空闲页是不够的。如果 Prefill 阶段把缓存全部吃完，已经进入 Decode 的请求可能无法继续生成。

## 二、Continuous Batching 的状态划分

Mini-SGLang 将请求分成两个主要集合：

```text
waiting queue：尚未完成 Prefill，等待进入 GPU
running batch：已经 Prefill，正在逐 token Decode
```

传统静态 batch 的生命周期以“整批”为单位；Continuous Batching 的生命周期以“请求”为单位。某个请求完成后立即离开，空出的槽位可以接纳新请求。

每轮调度的基本选择是：

```text
如果能接纳 Prefill：组织 Prefill batch
否则：让 running batch 做一轮 Decode
```

实际判断还必须考虑 Chunked Prefill、缓存容量和 GPU 上一轮是否完成。

## 三、Scheduler 的组成

`Scheduler` 组合了几个职责明确的对象：

- `Engine`：执行模型前向；
- `CacheManager`：分配和回收 KV 页；
- `PrefillManager`：从等待队列选择 Prefill 请求；
- `DecodeManager`：维护运行中的 Decode 请求；
- `TableManager`：分配请求页表槽位；
- `SchedulerIOMixin`：收发 ZMQ 与 TP 广播消息。

这种拆分很重要。调度策略不直接操作模型权重，Engine 也不决定请求优先级。

## 四、主循环的流水线结构

`Scheduler.run_forever()` 不断调用一次调度 step。其逻辑可以抽象成：

```text
CPU：接收请求
CPU：处理上一轮输出
CPU：选择并准备下一批
GPU：执行下一批
CPU：继续处理控制工作
```

Engine 返回的 `ForwardOutput` 不只有 token，还包含一个 CUDA Event：

```python
class ForwardOutput(NamedTuple):
    next_tokens_gpu: torch.Tensor
    next_tokens_cpu: torch.Tensor
    copy_done_event: torch.cuda.Event
```

GPU 计算完成后，token 被异步复制到 CPU。Scheduler 不必立刻全局同步，而是在真正读取结果前等待 Event。这样可以把一部分 Python 调度开销隐藏在 GPU 工作之后。

## 五、请求如何进入等待队列

Rank 0 从 Tokenizer 收取后端消息，再广播给其他 TP rank。每个 rank 都将同一请求转换为 `PendingReq`。

`PendingReq` 尚未拥有完整的 GPU 运行状态。只有被 PrefillManager 选中后，它才会获得：

- `table_idx`；
- Prefix Cache handle；
- 已命中的 `cached_len`；
- 分配的 KV Cache 页；
- 对应的 `Req` 对象。

这种延迟分配避免等待队列中的大量请求提前占用 GPU 资源。

## 六、PrefillAdder 如何做资源判断

`PrefillAdder` 是 Prefill 调度的核心辅助对象。它尝试逐个加入请求，并检查两个方向的容量。

首先是本轮 token 数：

```text
extend_len = 输入长度 - 已缓存前缀长度
```

若请求命中 Radix Cache，只需计算未命中的后缀。

其次是缓存容量。新请求需要为未命中的 token 和未来生成保留空间。调度器结合空闲页、可驱逐前缀和运行请求占用情况判断能否接纳。

这种准入控制保证 Scheduler 不会生成一个 Engine 无法实际执行的 batch。

## 七、为什么需要 Chunked Prefill

假设一个 100K token 的长请求和几十个 Decode 请求同时存在。如果长请求一次完成 Prefill，它可能长时间占据 GPU，并产生很大的中间张量，其他用户的单 token 解码延迟会明显升高。

Chunked Prefill 把它切成多段：

```text
100K prompt
  -> chunk 0：0～8191
  -> chunk 1：8192～16383
  -> ...
  -> final chunk
```

`ChunkedReq` 继承 `Req`，但只暴露当前 chunk 的逻辑边界。完成一个 chunk 后，请求回到等待状态；最终 chunk 完成后才进入 DecodeManager。

Chunking 带来三点收益：

- 限制单轮 token 数和临时显存；
- 让 Decode 请求在长 Prefill 之间获得执行机会；
- 让 GPU batch 形状更可控。

代价是同一 prompt 需要多轮调度，且 chunk 边界会增加少量元数据处理。

## 八、Prefill batch 如何构造

PrefillManager 的流程可以概括为：

```text
遍历 waiting queue
 -> 查询最长可复用前缀
 -> 计算 extend_len
 -> 检查 token/cache budget
 -> 必要时裁剪成 ChunkedReq
 -> 分配 table slot 和 KV pages
 -> 组成 Batch(phase="prefill")
```

Prefill 的 `input_ids` 不是完整 prompt，而是所有请求尚未缓存的后缀拼接：

```text
请求 A：缓存 4，输入 7 -> 提交 3 token
请求 B：缓存 0，输入 2 -> 提交 2 token
Batch input_ids = A[4:7] + B[0:2]
```

`positions` 则分别从各请求的 `cached_len` 开始生成。Attention metadata 记录每个请求的 query 长度和 KV 长度，kernel 才能解释这段扁平 token 数组。

## 九、Decode batch 为什么更规则

完成 Prefill 的请求进入 `DecodeManager`. 每个请求每轮只提交最新 token，因此普通 Decode batch 的输入长度等于请求数：

```text
A 最新 token
B 最新 token
C 最新 token
```

但每个请求要读取的历史 KV 长度不同，因此仍需要 page table 和 sequence length metadata。

DecodeManager 还要保证不同 TP rank 的请求顺序稳定。若 rank 间 batch 顺序不同，Tensor Parallel collective 会把不同请求的中间结果混合，轻则结果错误，重则通信死锁。当前仓库最新提交正是加强了这一稳定性。

## 十、Prefill 与 Decode 如何取舍

调度器不能永远优先 Prefill，否则已有请求的首 token 之后会长时间停顿；也不能永远优先 Decode，否则新请求永远无法获得首 token。

Mini-SGLang 的策略是受预算约束地尝试加入 Prefill，并在不可加入时执行 Decode。Chunked Prefill 进一步限制单次 Prefill 体量，使两类工作能够交错。

从用户体验看，这对应两个指标：

- TTFT：Time To First Token，主要受排队和 Prefill 影响；
- ITL：Inter-Token Latency，主要受 Decode 调度影响。

调度策略本质上是在吞吐、TTFT 和 ITL 之间取舍。

## 十一、请求完成后的处理

Scheduler 等待 `copy_done_event` 后读取 CPU token，逐个更新请求：

```text
将 token 追加到 host token 序列
 -> cached_len/device_len 前进
 -> 判断 EOS 或长度上限
 -> 未完成：保留在 DecodeManager
 -> 已完成：发送结束消息并释放资源
```

释放并不一定意味着立即删除所有 KV。启用 Radix Cache 时，可复用的完整前缀会进入前缀树，未被请求引用的页变成可驱逐缓存。

## 十二、调度层的边界

Scheduler 决定“做什么”，Engine 决定“怎样在 GPU 上执行”。这一边界可以用下表概括：

| 问题 | 负责模块 |
|---|---|
| 哪些请求进入本轮 | Scheduler |
| 本轮是 Prefill 还是 Decode | Scheduler |
| KV 页分配给谁 | CacheManager |
| Attention metadata 如何组织 | Scheduler + Backend |
| 模型权重如何执行 | Engine + Model |
| 下一个 token 如何采样 | Sampler |

下一篇将深入 KV Cache：为什么需要 Paged KV Cache、页表如何工作，以及 Radix Tree 怎样实现跨请求前缀复用。

## 参考源码

- `python/minisgl/scheduler/scheduler.py`
- `python/minisgl/scheduler/prefill.py`
- `python/minisgl/scheduler/decode.py`
- `python/minisgl/scheduler/cache.py`
- `python/minisgl/scheduler/table.py`
- `python/minisgl/scheduler/io.py`
- `python/minisgl/core.py`
