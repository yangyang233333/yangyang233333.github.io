---
title: "Mini-SGLang 源码阅读（五）：模型层、Tensor Parallel 与自定义 Kernel"
date: 2026-08-25T21:10:00+08:00
draft: false
tags: ["Mini-SGLang", "Tensor Parallel", "CUDA Kernel", "源码阅读"]
categories: ["大模型推理"]
description: "从张量并行的矩阵切分原理出发，分析 Mini-SGLang 的并行 Linear、Embedding、模型注册、权重加载和底层 Kernel。"
---

前四篇已经解释请求如何到达 Scheduler、KV Cache 如何管理，以及 Engine 怎样执行一个 Batch。最后还剩一个问题：几十亿参数怎样分布到多张 GPU，并在每层计算后重新组合？

本文先推导 Tensor Parallel 的基本原理，再分析 Mini-SGLang 的 Layers、Models、Distributed、MoE 和 Kernel 模块。源码基于提交 `9a91cfa`。

## 一、为什么需要 Tensor Parallel

如果模型权重或 KV Cache 放不进单张 GPU，最直接的方法是把不同层放到不同 GPU，也就是 Pipeline Parallel。但自回归 Decode 每轮 token 很少，流水线容易出现气泡，而且跨 stage 传递激活会增加延迟。

Tensor Parallel 把同一层的矩阵切到多张 GPU，让所有 GPU 同时计算同一批 token：

```text
同一 Transformer Layer
GPU 0：处理部分权重
GPU 1：处理部分权重
GPU 2：处理部分权重
GPU 3：处理部分权重
        ↓ collective
组合为完整层输出
```

它需要更频繁的 GPU 间通信，但能共同承载单层权重，并保持每个 rank 的执行进度一致。

## 二、Column Parallel Linear

考虑线性层：

```text
Y = XW
```

按输出维切分权重：

```text
W = [W0 | W1 | ... | Wn]
```

每个 rank 都拥有完整输入 `X`，分别计算：

```text
Y0 = XW0
Y1 = XW1
...
```

输出自然是 `Y` 的不同列。如果下一层也能消费分片输出，就不需要立即通信。

QKV projection 和 MLP 的 gate/up projection 通常适合 Column Parallel，因为 Attention heads 或中间维可以按 rank 切分。

Mini-SGLang 在 `layers/linear.py` 中实现对应的列并行 Linear，并在加载权重时只读取本 rank 所需分片。

## 三、Row Parallel Linear

按输入维切分权重：

```text
W = [W0; W1; ...; Wn]
X = [X0 | X1 | ... | Xn]
```

每个 rank 计算部分结果：

```text
P0 = X0W0
P1 = X1W1
...
Y = P0 + P1 + ...
```

最后必须执行 all-reduce sum。

Attention 的 output projection 和 MLP down projection 常用 Row Parallel。它们接收前一列并行层产生的分片激活，并在残差连接前还原完整 hidden states。

一组典型 MLP 因而是：

```text
完整 hidden states
 -> Column Parallel gate/up
 -> 每 rank 计算局部激活
 -> Row Parallel down
 -> all-reduce
 -> 完整 hidden states
```

## 四、Attention head 如何切分

Q heads 通常可以均匀分到各 rank。KV heads 则要考虑 GQA：KV head 数可能少于 TP size。

Mini-SGLang 的配置和 Layer 工具允许 KV heads 在必要时复制，而不是强制每个 rank 至少获得一个独立 KV head。每个 rank 的 KV Cache 只保存本地需要的 heads，因此缓存容量计算使用 `local_kv_heads`。

Attention 输出经过 Row Parallel output projection 和 all-reduce 后，各 rank 再次得到一致的完整 hidden states。

## 五、Vocab Parallel Embedding 与 LM Head

词表也可以沿 vocab 维切分。每个 rank 只保存一段 token embedding：

```text
Rank 0：token [0, V/2)
Rank 1：token [V/2, V)
```

输入 token 不属于本 rank 时，本地输出置零；所有 rank 的 embedding 结果 all-reduce 后得到完整向量。

LM Head 使用相同的词表分片思路，各 rank 产生局部 logits。随后可根据采样实现选择 all-gather 完整词表，或使用分布式选择。Mini-SGLang 的抽象把通信集中在并行 Layer 和 Distributed backend 中，模型代码保持接近普通 Transformer。

## 六、Distributed 模块的两套实现

`distributed/impl.py` 提供统一的：

- `all_reduce()`；
- `all_gather()`。

底层可使用：

- `torch.distributed` + NCCL；
- 项目封装的 PyNCCL；
- 单 GPU 空操作实现。

控制面另有 Gloo group，用于广播 Python 消息、同步配置和检查显存。数据面的模型张量则走 NCCL 类通信。

把控制与计算通信分开，可以避免小型 CPU 消息依赖 GPU collective，也便于 Rank 0 驱动请求广播。

## 七、模型注册与构建

`models/register.py` 使用 Hugging Face config 中的 `architectures[0]` 选择模型类。当前注册：

- `LlamaForCausalLM`；
- `Qwen2ForCausalLM`；
- `Qwen3ForCausalLM`；
- `Qwen3MoeForCausalLM`；
- `MistralForCausalLM`；
- `Mistral3ForConditionalGeneration`。

模型文件主要负责组装 Layer：Embedding、重复的 Decoder Layer、Norm 和 LM Head。运行时调度、KV Cache 与具体 Attention kernel 并没有复制到每种模型中。

这种结构的扩展路径很明确：新模型如果只是层组合不同，可以复用已有 Layer；只有新的算子或权重布局才需要扩展底层模块。

## 八、权重加载与分片

`models/weight.py` 和各 Layer 的 weight loader 负责将 Hugging Face 权重映射到本地参数。

Tensor Parallel 下不能先在每张 GPU 加载完整权重再切分，否则峰值显存过高。更合理的流程是：

```text
读取权重 Tensor
 -> 根据参数类型和 TP rank 选取 shard
 -> 转换 dtype
 -> 复制到本地参数
```

不同参数的切分维不同：

- Column Parallel 沿输出维切；
- Row Parallel 沿输入维切；
- QKV 合并参数分别处理 Q、K、V；
- GQA 的 KV heads 可能复制；
- Norm 和部分 bias 在各 rank 完整保留。

权重加载代码因此是理解 TP 正确性的关键，而不只是 I/O 工具。

## 九、RoPE 与位置编码

`layers/rotary.py` 实现 Rotary Position Embedding。RoPE 将位置相关旋转应用于 Query 和 Key，使注意力分数携带相对位置信息。

Scheduler 为每个 token 准备 position：

```text
Prefill：cached_len ... device_len-1
Decode：当前最后位置
```

Layer 只消费 positions，不关心请求经历了多少次 chunk。这样 Chunked Prefill 在语义上仍等价于一次完整 Prefill。

## 十、MoE 的额外路径

Qwen3 MoE 不再让每个 token 经过同一个 MLP，而是由 router 选择少量 experts：

```text
Token hidden state
 -> Router logits
 -> Top-k experts
 -> Expert GEMM
 -> 按权重合并
```

`moe` 目录定义 backend 接口与 fused 实现，`kernel/triton/fused_moe.py` 提供 Triton kernel。Engine 根据模型配置选择 MoE backend。

这里的核心性能问题是把按 expert 离散分组的 token 高效送入矩阵乘法，同时避免大量小 kernel 和中间 Tensor。

## 十一、自定义 Kernel 模块

`kernel` 目录包含几类底层能力：

- `index.py`：索引和二维映射操作；
- `store.py`：将新 K/V 写入缓存；
- `radix.py`：Radix 相关 C++ 绑定；
- `pynccl.py`：轻量 NCCL 封装；
- `triton/fused_moe.py`：融合 MoE；
- `csrc`：CUDA/C++ 与 TVM-FFI JIT 代码。

为什么不全部使用 PyTorch 算子？因为调度器经常需要执行“小而特殊”的操作，例如根据二维请求索引生成扁平缓存位置。组合多个通用算子会产生额外中间 Tensor 和 kernel launch，自定义 kernel 可以一次完成。

## 十二、完整 Transformer 层中的通信点

以常见 Decoder Layer 为例：

```text
完整 hidden states
 -> RMSNorm
 -> Column Parallel QKV
 -> 每 rank 本地 RoPE + Attention
 -> Row Parallel Output
 -> all-reduce
 -> Residual
 -> RMSNorm
 -> Column Parallel Gate/Up
 -> 本地激活
 -> Row Parallel Down
 -> all-reduce
 -> Residual
```

每层通常有两次主要 all-reduce。TP 越大，单卡计算越少，但通信比例越高。因此 TP size 不是越大越好，还取决于 GPU 互联、模型规模和 batch 形态。

## 十三、这套模块化设计的意义

Mini-SGLang 将系统拆成几条稳定边界：

```text
Scheduler：选择工作
Cache：管理状态
Engine：组织执行
Model：定义网络结构
Layer：封装并行语义
Backend/Kernel：实现硬件优化
```

这使读者可以单独替换一个调度策略、缓存算法或 Attention backend，而不必修改整条推理链路。它也是 Mini-SGLang 作为教学型高性能引擎最有价值的地方。

至此，整个系列从 HTTP 请求一直走到了 GPU kernel。建议下一步结合 profiler 实验：分别关闭 Radix Cache、Overlap Scheduling 和 CUDA Graph，观察 TTFT、ITL、吞吐和显存利用率如何变化。

## 参考源码

- `python/minisgl/layers/linear.py`
- `python/minisgl/layers/embedding.py`
- `python/minisgl/layers/attention.py`
- `python/minisgl/layers/rotary.py`
- `python/minisgl/layers/moe.py`
- `python/minisgl/models/register.py`
- `python/minisgl/models/weight.py`
- `python/minisgl/distributed/impl.py`
- `python/minisgl/kernel`
- `python/minisgl/moe`
