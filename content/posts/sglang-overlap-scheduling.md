---
title: "SGLang 双 Stream 重叠调度：如何把 CPU 后处理藏到 GPU 计算背后"
date: 2026-08-30T23:09:10+08:00
draft: false
tags: ["SGLang", "LLM", "推理系统", "CUDA", "调度优化"]
categories: ["源码阅读"]
---

大模型自回归推理看起来是纯 GPU 工作：每一步做一次模型前向，采样一个 token，再把 token 喂给下一步。然而在高性能推理系统里，GPU 计算只是流水线的一部分。采样结果回传、EOS 判断、请求回收、KV Cache 管理、下一批组装和元数据准备，都可能在两次前向之间制造空洞。

SGLang 的 Overlap Scheduling 解决的正是这个问题。它不是简单地“多开一条 CUDA stream”，而是同时完成两件事：

1. 让当前步产生的 token 留在 GPU，直接作为下一步输入，解除下一次 forward 对 CPU 读值的依赖；
2. 用 scheduler stream 和 engine stream 构造软件流水线，让 GPU 计算当前批次时，调度器处理上一批次的结果。

本文以 Mini-SGLang 的 `python/minisgl/scheduler/scheduler.py` 为线索，解释 `overlap_loop`、`_forward` 和 `_process_last_data` 背后的依赖重排。

## 先看结论：优化的不是“提交速度”，而是依赖关系

CUDA kernel 的提交本来就是异步的。CPU 把工作放进 stream 后，不必等待 GPU 完成。因此一个自然的问题是：只用一条 stream，只要 CPU 提交得足够快，不也能让 GPU 连续工作吗？

如果系统只有计算任务，这个判断基本成立。但自回归推理存在一个控制依赖：调度器通常要知道采样出的 token，才能判断请求是否命中 EOS、是否应该释放资源，以及下一轮有哪些请求仍应留在 batch 中。

串行执行会形成如下链条：

```text
GPU:  forward(B1) ── idle ── forward(B2) ── idle ── forward(B3)
CPU:               处理 B1                 处理 B2
                   读 token                读 token
                   判 EOS                  判 EOS
                   组 B2                   组 B3
```

GPU 完成 `B1` 后，CPU 才读取结果并决定 `B2`。此时 GPU 没有可执行的新工作，只能等待。这不是 CPU 调用 CUDA API 太慢，而是 `B2` 在逻辑上依赖 `B1` 的结果。

Overlap Scheduling 的关键，是继续追问：**下一步模型计算真的需要 CPU 理解 token 的值吗？**

答案是否定的。模型只需要 token ID 对应的 GPU 数据；只有终止判断和文本输出才需要 CPU 把它解释成整数。只要把这两类依赖拆开，流水线就能继续向前。

## 两种依赖：计算依赖与控制依赖

一个新生成的 token 有两种用途。

| 用途 | 消费方 | 是否需要 CPU 知道具体值 |
|---|---|---|
| 作为下一步模型输入 | GPU | 不需要 |
| EOS 判断、请求回收、detokenize | CPU / 前端 | 需要 |

传统直觉容易把它们绑在一起：GPU 采样 token，复制回 CPU，CPU 读取整数，再把它作为下一步输入传回 GPU。这样会产生 D2H、`.item()` 和下一轮 H2D 的同步链。

Mini-SGLang 则让 token 沿两条路径流动：

```text
                    ┌── GPU → GPU：写入 token_pool，供下一步 forward 使用
采样 token（GPU）───┤
                    └── GPU → CPU：异步回传，稍后用于判停和输出
```

GPU 计算路径不再等待 CPU 控制路径。CPU 仍然会读取 token，但读取被推迟到下一轮，并尽量与当前批次的 GPU 计算重叠。

## 双 Stream 的职责划分

调度器初始化时创建自己的 CUDA stream，并保留 engine 已有的计算 stream：

```python
self.device = self.engine.device
self.stream = torch.cuda.Stream(device=self.device)
self.engine_stream_ctx = torch.cuda.stream(self.engine.stream)
torch.cuda.set_stream(self.stream)
```

两条 stream 的职责并不对称。

| Stream | 主要职责 | 工作特征 |
|---|---|---|
| `self.stream` | batch 组装、mapping/position、元数据准备、拷贝和结果收尾 | 小而碎，夹杂 CPU 控制逻辑 |
| `self.engine.stream` | 模型 forward、attention、采样 | 计算密集，持续时间较长 |

`engine_stream_ctx` 不是第三条 stream，只是一个上下文管理器。进入 `with self.engine_stream_ctx` 后，相关 CUDA 操作会被提交到 `self.engine.stream`。

为什么不把所有 GPU 操作都放进 engine stream？因为同一 stream 内严格有序。如果元数据准备所需的小 kernel 或拷贝被排在一个大 forward 后面，它们即使资源需求很低，也必须等前面的任务结束。独立的 scheduler stream 让这些轻量操作可以更早执行，或者与不冲突的计算重叠。

但多 stream 并不自动保证正确。不同 stream 没有隐含顺序；如果 forward 读取的 metadata 尚未准备完成，就可能读到未就绪的数据。SGLang 因而只在真实数据依赖处建立同步：

```python
with self.engine_stream_ctx:
    self.engine.stream.wait_stream(self.stream)
    ongoing_data = (forward_input, self._forward(forward_input))
```

`wait_stream` 的含义是：engine stream 在继续执行前，等待 scheduler stream 此前提交的工作完成。它把同步限制在 metadata → forward 这条边上，而不是粗暴地同步整个设备。

## `_forward`：让 token 留在显存闭环

Overlap Scheduling 能成立的支点位于 `_forward`：

```python
batch.input_ids = self.token_pool[input_mapping]
forward_output = self.engine.forward_batch(batch, ...)
self.token_pool[output_mapping] = forward_output.next_tokens_gpu
```

这三步构成了 token 的 GPU 闭环：

1. 根据 `input_mapping` 从常驻显存的 `token_pool` gather 当前输入；
2. engine 执行模型前向与采样，得到 `next_tokens_gpu`；
3. 根据 `output_mapping` 把采样结果写回 `token_pool` 对应位置。

调度器在安排下一轮时只需要维护“哪个请求对应 token_pool 的哪个位置”。它不需要先把 token 读回 CPU，再重新上传。换句话说，CPU 负责索引和生命周期，GPU buffer 保存真正参与下一步计算的数据。

这里容易出现一个误解：CPU 不读取 token，如何知道下一批由哪些请求组成？

答案是允许短暂的滞后一轮。当前批次开始计算时，上一批次的 CPU 后处理才完成。已经结束的请求可能在一个受控窗口内进入下一轮，因此实现还需要 `finished_reqs` 一类机制避免重复释放。这是一种有意设计的流水线状态，而不是忽略终止条件。

## `overlap_loop`：把一轮拆成四个阶段

核心循环可以概括为：

```python
def overlap_loop(self, last_data):
    for msg in self.receive_msg(blocking=...):
        self._process_one_msg(msg)

    forward_input = self._schedule_next_batch()

    ongoing_data = None
    if forward_input is not None:
        with self.engine_stream_ctx:
            self.engine.stream.wait_stream(self.stream)
            ongoing_data = (forward_input, self._forward(forward_input))

    self._process_last_data(last_data)
    return ongoing_data
```

### 1. 接收新请求

调度器先读取前端消息，把新请求加入 pending 或 decode 管理结构。是否阻塞取决于当前是否还有可推进的工作。

### 2. 组装下一批并准备元数据

`_schedule_next_batch()` 选择请求、分配资源并准备 attention metadata、position 和 mapping。这些工作发生在 scheduler stream 的语义下。

这里的“下一批”并不要求上一批的 token 已经被 CPU 完整消费，因为其模型输入已经由 `token_pool` 在 GPU 上保存。

### 3. 在 engine stream 发起当前批次

调度器切到 engine stream，先等待本轮 metadata 就绪，再提交 `_forward`。CPU 提交完成后即可继续执行，不必等待模型前向真正结束。

返回的 `ongoing_data` 保存本轮的输入和异步输出对象，在下一次循环中变成 `last_data`。

### 4. 处理上一批结果

当前批次已经在 engine stream 上运行，此时 scheduler 开始处理 `last_data`。于是形成关键重叠：

```text
engine.stream:     forward(Bn)
                         ║ 并行
scheduler / CPU:   process(Bn-1)
```

注意，这里的“scheduler stream 处理结果”并不意味着所有逻辑都在 GPU 上。它包含 CPU 控制逻辑，也包含 D2H 拷贝和少量 GPU 操作。优化目标是让整段后处理处在当前 forward 的时间窗口内。

## `_process_last_data`：同步仍然存在，只是被移到了流水线里

Overlap Scheduling 没有消灭同步。CPU 最终仍要得到 token 的具体值：

```python
copy_done.synchronize()
req.append_host(next_token)
next_token = int(next_token.item())

finished = not req.can_decode
if not req.sampling_params.ignore_eos:
    finished |= next_token == self.eos_token_id
```

`copy_done.synchronize()` 等待上一批 token 的异步 D2H 拷贝完成，`.item()` 将张量值转换成 CPU 整数。随后调度器才能执行：

- 追加请求的 host 端 token；
- 判断 EOS、长度上限和其他停止条件；
- 从 decode manager 移除已完成请求；
- 释放 request table、KV Cache 页等资源；
- 为未结束的 prefill 请求更新或缓存前缀；
- 把 `DetokenizeMsg` 发给前端。

区别在于，CPU 等待的是 `Bn-1` 的结果，而 engine stream 已经在计算 `Bn`。只要当前 forward 足够长，上一批结果的同步和控制开销就能隐藏在它后面。

Chunked Prefill 请求通常不在每个 chunk 后采样，因此收尾逻辑会跳过尚未完成的 chunk；`finished_reqs` 等状态则用于防止流水线滞后一轮带来的重复回收。

## 展开三轮：流水线如何填满

把循环展开后，执行关系更直观：

```text
时间              t1                    t2                    t3

engine.stream     forward(B1)           forward(B2)           forward(B3)
                     │                     │                     │
scheduler/CPU     无上一批              处理 B1               处理 B2
                                        判 EOS                判 EOS
                  组装 B1               回收资源              回收资源
                                        组装 B2               组装 B3
```

第一轮只有 forward，没有可处理的上一批结果，这是流水线的填充阶段。从第二轮开始，GPU 计算 `Bn`，CPU 处理 `Bn-1`。停止时同样会有一次排空阶段。

理想情况下，每轮耗时近似从串行的：

```text
T_serial ≈ T_schedule + T_forward + T_postprocess
```

变成稳定流水线中的：

```text
T_overlap ≈ max(T_forward, T_schedule + T_postprocess) + T_sync
```

其中 `T_sync` 表示无法隐藏的真实依赖和流水线边界成本。这个公式不是精确性能模型，但能说明收益上限：只有可并行部分才能被隐藏。

## 为什么模型越小，收益往往越明显

当模型很大时，单步 forward 很长，CPU 后处理占总时延的比例较小。即使完全隐藏后处理，端到端提升也有限。

小模型或高性能 GPU 上的 decode 则相反：单步 kernel 很快，Python 调度、token 回读、请求状态更新和 metadata 准备开始占据显著比例。此时每一步留下几十或几百微秒空洞，累积到长序列后会明显降低 GPU 利用率，Overlap Scheduling 的价值更高。

收益还受以下因素影响：

- batch 大小和请求到达模式；
- prefill 与 decode 的混合比例；
- CPU 单核性能及 Python 开销；
- D2H/H2D 拷贝是否真正异步；
- metadata kernel 能否与模型 kernel 并发；
- GPU 是否已有足够高的计算或带宽占用；
- 终止请求带来的滞后一轮额外计算与资源占用。

因此“双 stream”不是恒定倍数的加速开关，而是一种减少 bubble 的调度方法。

## `normal_loop` 为什么仍然必要

Mini-SGLang 仍保留由 `ENV.DISABLE_OVERLAP_SCHEDULING` 控制的串行循环。`normal_loop` 通常按“调度 → forward → 处理结果”的顺序执行，便于建立性能基线和排查并发问题。

它尤其适合诊断以下错误：

- 跨 stream 缺少依赖，导致 metadata 未就绪；
- buffer 生命周期过短，异步 kernel 仍在访问已复用内存；
- D2H copy 的 event 或 synchronize 使用不正确；
- 请求滞后一轮后重复结束、重复释放；
- 某个看似异步的 `.item()` 或内存操作触发全局同步。

如果关闭 overlap 后错误消失，问题通常不在模型数学计算，而在 stream 顺序、事件同步或对象生命周期。

## 常见误解

### 多一条 stream 就一定能并行

不一定。GPU 是否真正并发取决于 kernel 的资源占用、硬件 copy engine、依赖关系和提交时机。若 forward 已占满 SM 或内存带宽，scheduler stream 的小 kernel 可能仍只能穿插执行。但即使物理并发有限，拆分 stream 仍能避免无关任务被同一队列的人为顺序阻塞。

### `wait_stream` 会把优化抵消

不会。问题不在于“是否同步”，而在于同步粒度。metadata 是 forward 的真实前置依赖，必须等待；上一批 token 的 CPU 处理不是当前 forward 的前置依赖，因此不应阻塞 engine stream。精确同步保留正确性，也保留其余并发空间。

### token 留在 GPU 就不需要回传

仍然需要。EOS 判断、停止条件、日志和 detokenize 都需要 host 端 token。优化只是把回传从“下一步计算的必经路径”移到旁路，并让它异步、延迟地完成。

### Overlap Scheduling 等于 CUDA Graph

两者解决的问题不同。CUDA Graph 主要减少 CPU launch 开销和动态提交成本；Overlap Scheduling 重排的是跨步控制依赖，让上一批后处理与当前批计算并行。它们可以同时存在并互补。

## 设计这类流水线时应检查什么

从 Mini-SGLang 的实现可以提炼出一套通用检查清单：

1. 区分数据的计算消费者和控制消费者，不要让 CPU 控制路径阻塞 GPU 数据路径；
2. 为长期跨步使用的数据建立稳定的 GPU buffer，而不是依赖临时 tensor；
3. 用 event 或 stream wait 表达最小必要依赖，避免设备级全局同步；
4. 明确每个异步对象的生命周期，确保 buffer 在消费者完成前不会被复用；
5. 处理流水线填充、排空和滞后一轮带来的重复状态问题；
6. 保留串行模式作为正确性基线，并用 profiler 验证是否真的消除了 bubble。

真正值得借鉴的不是“两条 stream”这个数字，而是依赖图的重构方法：先找出阻塞下一步的边，再判断它是真实计算依赖，还是实现方式制造的控制依赖。

## 总结

SGLang 的双 Stream 重叠调度可以浓缩为一句话：**让 token 沿 GPU 数据路径立即进入下一步，把 CPU 必须完成的判停、回收和输出延迟到旁路处理。**

`token_pool` 切断了下一次 forward 对 CPU 读值的依赖；scheduler stream 与 engine stream 将 metadata/后处理和模型计算分离；`wait_stream` 只保护真实的数据依赖；`last_data` 则把循环变成“计算当前批、收尾上一批”的稳态流水线。

最终被优化掉的不是某个算子，而是 GPU 两次 forward 之间原本无事可做的时间。这也是推理系统优化中最重要的视角之一：当 kernel 已经足够快，下一步往往不是继续打磨算子，而是重新安排数据与控制依赖，让硬件始终有工作可做。
