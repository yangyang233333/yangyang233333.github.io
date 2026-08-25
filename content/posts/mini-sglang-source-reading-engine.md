---
title: "Mini-SGLang 源码阅读（四）：Engine、Attention Backend 与 CUDA Graph"
date: 2026-08-25T21:00:00+08:00
draft: false
tags: ["Mini-SGLang", "Attention", "CUDA Graph", "源码阅读"]
categories: ["大模型推理"]
description: "从 GPU 推理的 kernel 启动开销和 Prefill/Decode 差异出发，分析 Mini-SGLang Engine 的完整执行路径。"
---

Scheduler 决定本轮执行哪些请求，Engine 则负责把这个决定变成 GPU 计算。它连接模型、KV Cache、Attention backend、CUDA Graph、Tensor Parallel 和采样器，是 Mini-SGLang 的执行核心。

本文先解释 Attention backend 与 CUDA Graph 的基本原理，再分析一次 `Engine.forward_batch()`。源码基于提交 `9a91cfa`。

## 一、GPU 推理不只有矩阵乘法

一次 Transformer forward 包含许多 GPU 操作：

```text
Embedding
 -> RMSNorm
 -> QKV Linear
 -> RoPE
 -> 写入 KV Cache
 -> Attention
 -> Output Linear
 -> Residual
 -> MLP / MoE
 -> LM Head
 -> Sampling
```

大模型 Prefill 中矩阵较大，GPU 容易被计算填满；Decode 中每个请求通常只有一个 query token，大量小 kernel 的 CPU launch 开销会变得突出。

因此高性能 Engine 不仅要选快 kernel，还要减少 Python 和 CUDA runtime 在每轮 Decode 中的固定成本。

## 二、Engine 初始化做了什么

`Engine.__init__()` 的主要步骤是：

```text
绑定当前 TP rank 的 CUDA device
 -> 初始化 torch.distributed
 -> 创建模型结构
 -> 加载并切分权重
 -> 估算剩余显存
 -> 分配 KV Cache
 -> 创建 Attention backend
 -> 创建 MoE backend
 -> 创建 Sampler
 -> 捕获 CUDA Graph
```

顺序不能随意改变。只有模型权重加载后，Engine 才知道剩余显存能分配多少 KV pages；只有 KV Cache 和 Attention backend 就绪后，才能捕获真实执行图。

## 三、KV Cache 容量如何估算

Engine 在模型加载前后分别测量空闲显存，估算模型实际占用。单页 KV 成本由下式决定：

```text
2 × num_layers × local_kv_heads × head_dim
  × page_size × dtype_size
```

再根据 `memory_ratio` 决定多少显存可以用于缓存：

```text
available = memory_ratio × initial_free - model_memory
num_pages = available / cache_per_page
```

多 TP rank 会同步空闲显存并取较小值，确保每个 rank 创建相同容量。若各 GPU 可用显存差距过大，Engine 直接报错，而不是让某个 rank 在运行中先 OOM。

## 四、Attention backend 的统一接口

`attention/base.py` 定义 `BaseAttnBackend`。具体后端主要负责两件事：

1. 根据 Batch 和页表构建 metadata；
2. 执行 Prefill 或 Decode Attention。

当前实现包括：

- FlashAttention；
- FlashInfer；
- TensorRT-LLM FMHA。

模型中的 `AttentionLayer` 不关心具体库。它完成 QKV 投影和 RoPE 后，把 Query、Key、Value 交给全局 Context 中的 backend。

这种接口隔离使模型定义和 kernel 选择彼此独立。

## 五、Prefill 和 Decode 为什么适合不同后端

Prefill 的 query 长度通常大于 1，多个请求长度不等，适合支持变长 packed sequence 的高吞吐 kernel。

Decode 的 query 长度固定为 1，但每个请求的 KV 长度不同，并且需要高效读取 paged cache。此时针对单 token decode 优化的 kernel 更有优势。

`HybridBackend` 因而允许分别选择：

```bash
--attn fa,fi
```

表示 Prefill 使用 FlashAttention，Decode 使用 FlashInfer。自动模式会根据 GPU 架构选择组合，例如 Hopper 默认倾向 FlashAttention + FlashInfer，更新架构可选择 TensorRT-LLM backend。

## 六、Attention metadata 的作用

Scheduler 传给模型的是扁平 token Tensor，但 Attention kernel 需要知道：

- 每个请求的 query 从哪里开始；
- 每个请求有多少 query token；
- 历史 KV 长度是多少；
- 页表位于哪一行；
- 新 K、V 写到哪些物理位置。

这些信息被封装为 backend 特定的 `BaseAttnMetadata`。例如 Prefill 可以使用 cumulative sequence lengths，Decode 可以使用 batch indices、sequence lengths 和 page table。

将 metadata 构建放在 CPU 调度流上，也为 Overlap Scheduling 提供了空间：CPU 准备下一批 metadata 时，GPU 可以执行当前批。

## 七、一次普通 forward

Scheduler 完成 Batch 准备后调用 Engine：

```python
with self.ctx.forward_batch(batch):
    logits = self.model.forward()
```

`Context.forward_batch()` 暂时把 Batch 设为当前上下文。模型层随后通过 `get_global_ctx()` 访问：

- `ctx.batch.positions`；
- `ctx.batch.out_loc`；
- `ctx.batch.attn_metadata`；
- `ctx.kv_cache`；
- `ctx.attn_backend`。

这是一个刻意的工程折中：全局 Context 降低了模型接口复杂度，但也意味着同一进程不能嵌套或并发执行两个 Batch。每个 GPU 一个 Engine 进程正好满足这一约束。

## 八、Attention Layer 如何使用缓存

Attention Layer 的逻辑可以概括为：

```text
hidden_states
 -> QKV projection
 -> 按本 rank 的 heads 拆分
 -> 应用 RoPE
 -> 将新 K/V 写入 ctx.kv_cache[out_loc]
 -> backend.forward(q, k, v, metadata)
 -> 合并输出
```

Prefill 和 Decode 都经过同一层，但 backend 根据 `batch.phase` 选择不同实现。分页、变长序列和缓存读取细节被隐藏在 backend 内部。

## 九、CUDA Graph 解决什么问题

普通 PyTorch 每轮都由 CPU 逐个发起 kernel：

```text
Python -> launch norm
Python -> launch GEMM
Python -> launch RoPE
Python -> launch attention
...
```

Decode 的单个 kernel 很短，CPU launch 间隙可能占据显著比例。CUDA Graph 可以先捕获固定执行序列，之后一次 replay 整张图：

```text
首次：分配静态输入 -> capture kernels
之后：复制新输入 -> graph.replay()
```

它减少 launch 开销，但要求地址和主要形状稳定。

## 十、GraphRunner 如何处理动态 batch

在线 batch size 持续变化，而 CUDA Graph 通常针对固定 batch size 捕获。`GraphRunner` 预先选择一组可覆盖的 batch size，并为每个尺寸建立 Graph。

实际 batch 较小时，Scheduler 用 dummy request 补齐到最近的已捕获尺寸：

```text
实际 batch size = 13
可用 graph size = 16
补 3 个 dummy request
只返回前 13 个请求结果
```

`GraphCaptureBuffer` 保存固定地址的输入 token、position、页表索引和其他 metadata。Replay 前把本轮数据复制进去，图中的 kernel 始终引用同一组地址。

超出最大捕获 batch、处于不支持的 Prefill 形态，或配置关闭 CUDA Graph 时，Engine 回退到普通 eager forward。

## 十一、Sampler 如何生成 token

模型输出 logits 后，`Sampler` 根据每个请求的参数执行：

- greedy；
- temperature scaling；
- top-k；
- top-p。

批内请求可以有不同 sampling 参数，因此 `BatchSamplingArgs` 将参数整理成 GPU Tensor。Greedy 请求可以直接 `argmax`，随机采样请求则经过过滤与 multinomial。

生成的 token 保留一份 GPU Tensor，用于下一轮 Decode 输入；同时异步复制一份到 CPU，供 Scheduler 判断 EOS 和发送给 Detokenizer。

## 十二、Engine 与 Scheduler 的异步边界

Engine 在自己的 CUDA stream 上执行，并记录 `copy_done_event`。Scheduler 可以在另一个 stream 和 CPU 上准备工作，直到必须读取输出 token 时才同步。

这条边界是 Overlap Scheduling 的关键：

```text
GPU stream：模型 forward -> sampling -> D2H copy
CPU/调度流：收消息 -> 更新队列 -> 准备 metadata
```

如果每轮 `torch.cuda.synchronize()`，所有 CPU 工作都会排在 GPU 之后，重叠机会就消失了。

下一篇进入模型和分布式层，分析 Column/Row Parallel Linear、Embedding、权重切分与 NCCL collective 如何组成 Tensor Parallel 模型。

## 参考源码

- `python/minisgl/engine/engine.py`
- `python/minisgl/engine/graph.py`
- `python/minisgl/engine/sample.py`
- `python/minisgl/attention/base.py`
- `python/minisgl/attention/fa.py`
- `python/minisgl/attention/fi.py`
- `python/minisgl/attention/trtllm.py`
- `python/minisgl/layers/attention.py`
