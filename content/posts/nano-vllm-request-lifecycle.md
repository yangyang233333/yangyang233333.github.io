---
title: "从提示词到下一个 token：nano-vLLM 请求生命周期、KV cache 与 batch 时序"
date: 2026-09-12T22:40:00+08:00
draft: false
tags: ["nano-vLLM", "LLM 推理", "KV Cache", "Continuous Batching", "CUDA", "源码阅读"]
categories: ["技术"]
description: "沿一次请求追踪 chat template、tokenizer、调度、GPU 输入打包、Transformer 计算、分页 KV cache、采样与返回，并用源码级实验厘清前缀命中和 batch overlap 的真实边界。"
---

一条提示词进入推理引擎之后，并不是直接“送给 GPU 生成文字”。它先变成 CPU 上的 token ID 和请求状态；调度器决定本轮计算哪些位置、复用哪些 KV 块；执行器把本轮输入和地址映射送上 GPU，运行模型并采样；采样结果再回到 CPU，才能决定下一轮要计算什么。[S1][S2][S3]

nano-vLLM 很适合把这条链看清楚，因为这些环节集中在少量 Python 文件里。但不能把成熟推理系统的所有能力自动套到它身上。本文分析的版本**已有 chunked prefill，却没有把 prefill 和 decode 混进同一次模型调用，也没有实现显式的跨 batch CPU/GPU 流水重叠**。后文会沿实际控制流解释这个结论，而不是根据功能名称猜测。[S2][S3]

分析快照为 **2026 年 9 月 12 日获取的 `GeeeekExplorer/nano-vllm` 默认分支提交 `bb823b3e06983d71485a8e1f23715ebd87d98ef8`**，对应提交日期为 2026 年 4 月 26 日。源码链接全部固定到这一版本。

除静态阅读外，本文做了三类小实验：真实 Qwen3 tokenizer 编码；直接驱动仓库原有 `Scheduler`、`BlockManager` 的控制面实验；提取原始输入准备方法并在 CUDA 上执行，核对实际张量和地址映射。**控制面实验使用人为指定的采样结果，不代表执行了模型；本文没有跑完整模型推理、吞吐 benchmark 或 Nsight 时间线。**

## 一、先分清两条生命周期：引擎初始化与请求执行

`LLM` 本身只是 `LLMEngine` 的简单派生类。真正的主干如下：[S1][S4]

```text
初始化一次
  Config / 模型配置
  → ModelRunner / GPU 模型与权重
  → warmup / KV cache 内存池 / 可选 CUDA Graph
  → tokenizer / Scheduler

每个请求
  字符串或 token IDs
  → add_request / Sequence / waiting
  → schedule
  → prepare_prefill 或 prepare_decode
  → GPU 模型前向 / logits / sampling
  → token IDs 返回 CPU
  → postprocess / 更新 KV 元数据与请求状态
  → 下一轮，或结束后解码为文字
```

`ModelRunner` 设置 CUDA 设备和默认 dtype，在 GPU 上创建 Qwen3 模型。权重加载器从 safetensors 读取 CPU 侧权重，再经各层的 loader 复制到对应参数；Q、K、V 和 gate/up 投影还会被装入打包后的参数矩阵。[S3][S5][S6]

因此，正常逐 token 推理时，**不是每轮都把模型权重从 CPU 搬到 GPU**。权重和 KV 内存池已经在显存中；每轮主要新增的是少量 token ID、位置、长度、映射表，以及本轮计算得到的新 KV。

这也决定了测量口径：进程启动、权重加载、warmup、编译、graph capture，与一个已经预热好的请求的首 token 延迟，不应该混为一个指标。

## 二、用户输入先经过 chat template，再经过 tokenizer

### 2.1 引擎并不会自动把任意字符串包装成聊天消息

仓库 `example.py` 先调用 Hugging Face tokenizer 的 `apply_chat_template(..., tokenize=False, add_generation_prompt=True)`，再把生成的字符串交给 `llm.generate`。[S7]

但 `LLMEngine.add_request` 对字符串做的事情只有 `self.tokenizer.encode(prompt)`。如果直接传入裸字符串，引擎不会自动补 system/user/assistant 角色边界；如果传入 `list[int]`，则直接把它当成已编码 token ID。[S1]

所以这里有两个不同步骤：

- **chat template**：把消息角色、消息正文、assistant 起始标记等组织成模型训练时使用的格式。
- **tokenizer**：按模型附带的词表、规则和特殊 token，把这个格式化结果变成整数序列。

这个区别直接影响模型行为，也影响前缀缓存。即使用户可见文本相同，只要模板、system prompt 或特殊标记不同，最终 token 前缀就可能不同。

### 2.2 一个真实的编码结果

本文使用公开模型 `Qwen/Qwen3-0.6B` 的固定快照 `c1899de289a04d12100db370d81485cdf75e47ca`，在 Transformers 4.57.3 下加载 fast tokenizer。实际类名是 `Qwen2TokenizerFast`，后端 tokenizer 配置中的模型类型是 BPE；这里的类名不意味着误用了 Qwen2 权重。[M1][M2]

输入为：

```text
你好，请解释 KV cache。
```

为使示例明确，调用 chat template 时额外指定 `enable_thinking=False`。得到的字符串可以用转义形式表示为：

```text
<|im_start|>user\n你好，请解释 KV cache。<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n
```

注意：这个模板即使关闭 thinking，也会保留一个空的 think 区段；不能凭参数名称猜测最终字节内容。实际编码得到 **18 个 token**：

```text
[151644, 872, 198, 108386, 37945, 104136, 84648, 6500, 1773,
 151645, 198, 151644, 77091, 198, 151667, 271, 151668, 271]
```

这个列表是 tokenizer 的实际输出，不是示意 ID。对该列表调用 `decode`，可以还原上述带角色与特殊标记的格式化文本。[M1][M2]

token 既不等于一个汉字，也不等于一个单词：例如换行、前导空格、特殊标记都可能参与分词。BPE 如何合并取决于模型的 tokenizer 文件；推理引擎只消费最终整数，不再处理原始汉字。

### 2.3 token ID 还不是 embedding

此时的 18 个整数仍然是 CPU 上的 Python list。它们是词表行号，不是浮点语义向量。后面送入 GPU 的 `input_ids` 也是整数张量；直到 `VocabParallelEmbedding` 执行 `F.embedding`，才查表得到模型需要的 hidden states。[S1][S8]

## 三、Sequence 保存什么：请求状态不等于 GPU 张量

`Sequence` 保存 token 历史、最后一个 token、采样参数、逻辑块表，以及三类经常被混淆的计数：[S9]

| 字段 | 含义 | 18-token 请求刚加入时 |
|---|---|---:|
| `num_tokens` | CPU 已知的完整序列长度，包含已生成 token | 18 |
| `num_prompt_tokens` | 初始 prompt 长度 | 18 |
| `num_cached_tokens` | 已有有效 KV 的连续前缀长度 | 0 |
| `num_scheduled_tokens` | 本轮要进行前向计算的 token 数 | 0 |
| `block_table` | 逻辑 KV 块到物理块 ID 的映射 | 空 |

`num_cached_tokens` 不仅表示“从别的请求命中的前缀”，也表示当前请求此前已经完成计算的部分。chunked prefill 执行完一段，同样会增加这个数。[S2]

`add_request` 把 `Sequence` 加入 scheduler 的 `waiting` 队列。调度器此时维护的主要是 CPU 状态；它没有把整条请求对象复制成一个 GPU 上的 Python 数据结构。[S1][S2]

对于仍在生成的普通请求，每轮结束后经常有这个关系：

```text
num_tokens = num_cached_tokens + 1
```

多出来的一个 token 是**刚刚采样出来、但还没有作为模型输入计算过 KV 的 token**。理解这个“差一”，就能理解为什么 prefill 已经生成了第一个输出 token，而下一轮 decode 仍要把它送进模型。

## 四、调度器先决定：本轮算哪些请求、哪些位置

`LLMEngine.step` 的顺序非常直接：`schedule → ModelRunner.run → postprocess`。每次 `schedule` 返回一组 sequence，以及一个作用于整个 batch 的 `is_prefill`。[S1][S2]

### 4.1 当前版本是 prefill 优先，不是混合调度

调度器首先检查 `waiting`：

1. 根据 token 预算、sequence 数上限和 KV 空间决定能否接纳请求。
2. 对尚未分配 KV 块的请求，先检查可复用前缀，再分配剩余块。
3. 设置本轮 `num_scheduled_tokens`。
4. 只要本轮选到了任何 prefill 工作，就返回 `is_prefill=True`。
5. 只有没有选到 prefill 工作时，才进入 running 请求的 decode 调度。[S2]

因此，一轮前向调用不会同时处理“请求 A 的 prefill”和“请求 B 的 decode”。`is_prefill` 不是逐 sequence 的 attention 模式数组，而是这次执行的统一分支。

### 4.2 chunked prefill 已经存在，但策略很具体

若一个请求剩余 prompt 太长，超过本轮 `max_num_batched_tokens` 预算，调度器可以只安排其中一段。不过当前代码只允许**本轮第一个选中的请求**做这种分块：如果 batch 中已经有请求，后来的请求又无法完整放进剩余预算，就停止继续选入。[S2]

分块期间，请求仍留在 waiting；只有本轮计划覆盖完它当前全部已知 token 时，才移到 running。实际有效 KV 的进度，要等模型执行后的 `postprocess` 才增加。

一个重要限制是：**分块计算不等于分块预留 KV 容量**。初次 `allocate` 仍然按请求当前总长度分配需要的全部逻辑块，只是本轮不一定把这些槽位全部写满。[S10]

对于 600-token prompt、256-token 块大小，即使第一轮只算 384 个 token，也已经分配了三个物理块。chunked prefill 限制了单轮计算量，却不能据此假设这个实现也把该请求的 KV 预留降到了 384 个 token。

### 4.3 decode 空间不足时是抢占重算，不是 swap 到 CPU

decode 通常为每个选中的 sequence 安排一个 token；只有跨进一个新逻辑块时才需要新增物理块。如果空闲块不足，scheduler 会抢占其他 running 请求，释放其块引用，把请求放回 waiting，并重新标成 prefill。[S2][S10]

token 历史仍在 CPU，所以之后可以重新计算；如果旧前缀块还没有被覆盖，也可能重新命中其中一部分。但这里没有把整份 KV 拷到 CPU、SSD，再无损恢复的交换机制。

## 五、“命中 KV cache”实际上有两种不同含义

### 5.1 同一请求 decode 时复用历史 KV

请求已经计算过前 18 个 token，那么下一次只需为第 19 个输入 token 计算新的 Q/K/V。Attention 会读取此前保存的 K/V，而不必重新对前 18 个 token 执行所有层的投影和前向计算。[S3][S11]

这是自回归推理的**历史 KV 复用**。它不需要每一轮都对整个前缀重新算 hash。

### 5.2 新请求开始时，复用其他计算留下的前缀块

新请求进入时，`BlockManager.can_allocate` 会检查前缀是否与已有块匹配。这才是通常所说的 **prefix caching**：通过 CPU 上的 hash 索引和引用计数，把新请求的逻辑块指向已有物理块，从而跳过这部分前向计算。[S10]

二者发生在不同位置：

| 行为 | 判定或执行位置 | 节省什么 |
|---|---|---|
| 前缀块命中 | CPU `BlockManager` | 新请求相同前缀的重复前向计算，以及可共享的 KV 容量 |
| decode 读取历史 KV | GPU attention kernel | 当前请求历史 token 的重复前向计算 |

这里没有一个 GPU kernel 在字符或语义层面搜索“相似提示词”。前缀判定基于 token ID，并且依赖完全相同的前文。

### 5.3 为什么 hash 必须包含前一个块

块的 hash 由当前块的 token ID 字节和前一个块的 hash 递推产生，使用 xxHash64。查表后还检查当前块的 token ID 是否相等。[S10]

如果某一块内的文本相同，但它之前的前文不同，当前块的 hidden states 和 KV 也可能不同。链式 hash 把这种上下文差异传播到后续块，避免仅凭“局部 token 一样”复用错误结果。

`can_allocate` 从头顺序查找，遇到第一个 miss 就停止，所以复用的是**连续前缀**，不是在整条 prompt 中搜索任意相同片段。这也不是密码学意义上的无碰撞证明；它是这个单引擎实现采用的快速索引方案。[S10]

### 5.4 最后一个逻辑块被刻意排除

当前实现遍历的是 `range(seq.num_blocks - 1)`：**新请求的最后一个逻辑块不参与前缀复用，即使它恰好是完整块**。[S10]

在相同前缀已经完整计算、物理块仍可用的前提下，直接驱动仓库类得到以下控制面结果：

| 新 prompt 长度 | 默认块大小 | 可命中的 token 数 | 仍需计算 |
|---|---:|---:|---:|
| 18 | 256 | 0 | 18 |
| 256 | 256 | 0 | 256 |
| 512 | 256 | 256 | 256 |
| 600 | 256 | 512 | 88 |

因此，重复提交前面那个 18-token 示例，不会得到块级 prefix hit；但同一请求在 decode 时仍然正常复用它自己的历史 KV。

这个限制也与输出路径有关：引擎缓存的是 K/V，不是最终 hidden state 或 logits。生成下一个 token 仍需要至少一个实际计算出的最后位置。当前代码采取的是保留整个末块计算的简单策略，而不是“完整命中后只重算最后一个 token”。这是对实现效果的解释，不能把后者当成它已经实现的优化。[S8][S10]

### 5.5 free block 不代表里面的数据立即消失

`deallocate` 减少引用计数，把引用归零的物理块放回 free 队列；并不立即清空它的 hash、token 元数据或 GPU KV 内存。只要该块尚未被重新分配覆盖，新请求仍能把它从 free 队列重新激活。[S10]

真正把块交给别的内容时，`_allocate_block` 才会清理对应旧 hash 映射、重置块元数据。因此：

- 请求结束后，前缀缓存仍可能有用。
- 显存中的 KV tensor 是预分配内存池，不会每结束一个请求就逐块 `cudaFree`。
- 内存压力可能覆盖这些可复用块，随后相同 prompt 就会 miss。

本文的控制面实验验证了这个过程：三块容量下，600-token 请求释放后仍可命中前 512 个 token；再把三块全部用于不同内容并释放，旧 prompt 的命中降为零。

### 5.6 同一个冷 batch 的重复请求，不会立刻共享尚未算出的 KV

hash 的发布发生在模型执行完成后的 `Scheduler.postprocess → hash_blocks`，而不是刚分配块时。[S2][S10]

于是，同一轮调度中的两个冷请求，即使 token 完全相同，第二个也不能命中第一个尚未计算的 KV。实验中，两个相同的 600-token 请求一起进入一个足够大的 prefill batch，二者的 cached token 都是 0，而且分配到不同物理块。

这是“复用已经完成的计算”和“合并正在进行的相同计算”的区别。这个版本有前者，没有在此路径上实现后者。

## 六、如何放到 GPU：复制本轮整数输入，不是复制整个请求历史

### 6.1 prefill 把变长序列拼成一维 token 流

`prepare_prefill` 对每个 sequence 计算：[S3]

```text
start = num_cached_tokens
query_length = num_scheduled_tokens
end = start + query_length

本轮输入 = token_ids[start:end]
positions = start ... end-1
本轮可见的 key 长度 = end
```

所有请求的新输入拼接成一个扁平张量，用 `cu_seqlens_q` 标记每条请求的 query 区间。已有缓存时，query 总长度和 key 总长度并不相同：新 token 需要查询完整可见前缀，但只需为新位置计算 Q/K/V。

这里的 key 长度是**已缓存前缀加上当前 chunk**，不是尚未处理完的整个长 prompt；否则当前 chunk 会错误地看见尚未计算或不应看见的后文。[S3]

### 6.2 一个实际送入 CUDA 的打包例子

另取两个用于验证地址的序列：A 总长 600，前 512 个 token 已缓存，本轮计算最后 88 个；B 是两个 token 的冷请求。本实验直接执行源码中的输入准备方法，得到：[S3]

```text
input_ids.shape = [90]
input_ids.dtype = int64
input_ids.device = cuda:0

cu_seqlens_q = [0, 88, 90]
cu_seqlens_k = [0, 600, 602]

positions = [512, ..., 599, 0, 1]
```

`cu_seqlens_k` 的最后一个值 602 是两条请求可见 key 长度的累计值，不表示它们被拼成了一个允许互相注意的 602-token 对话。sequence 边界由长度数组和块表共同传给 attention。

### 6.3 block table 告诉 attention 去哪里读，slot mapping 告诉新 KV 去哪里写

假设 A 的逻辑块映射为 `[5, 8, 11]`，B 的映射为 `[13]`，块大小为 256：

```text
A logical block 0 → physical block 5
A logical block 1 → physical block 8
A logical block 2 → physical block 11

B logical block 0 → physical block 13
```

一个逻辑 token 位置的物理槽位为：

```text
slot = block_table[position // block_size] × block_size
       + position % block_size
```

A 的位置 512 对应 `11 × 256 = 2816`；位置 599 对应 2903。B 的两个位置对应 3328、3329。实际 CUDA 打包结果也正是这些值。

两张映射不要混为一谈：

- `slot_mapping` 为**本轮计算的新 token**列出 KV 写入地址。
- `block_tables` 为**每条完整上下文**提供逻辑块到物理块的映射，供 attention 读取历史 KV。

张量块表会把不同请求补到相同列数，本例为 `[[5, 8, 11], [13, -1, -1]]`；有效长度另外传入，不能把补齐位置当成真实上下文。[S3][S11]

### 6.4 pinned memory 和 non-blocking 到底做了什么

这些 Python list 先构造成 CPU pinned-memory tensor，再调用 `.cuda(non_blocking=True)`：token ID 和位置使用 int64，长度、槽位和块表使用 int32。[S3]

它允许相关拷贝相对于 host 异步提交，但不代表 CPU list 自己成为 GPU 数据，也不代表与 GPU 上任意计算自动并行。后续模型对这些输入有依赖；跨 stream 的真正重叠还需要独立工作、硬件条件和正确同步。关于不同 batch 的情况，第十二节会结合整个调用链判断。[D1]

## 七、GPU 上真正计算什么：从整数到 Q/K/V，再到 hidden states

当前 `ModelRunner` 直接构建 `Qwen3ForCausalLM`，不是根据任意架构自动分派到一个无限通用的模型注册表。沿 Qwen3 路径，一轮前向是：[S3][S6]

```text
input_ids
  → embedding 查表
  → 重复执行各个 DecoderLayer
      RMSNorm
      → QKV 线性投影
      → Q/K 的归一化与 RoPE
      → 写入本层 KV cache
      → attention
      → 输出投影与 residual
      → RMSNorm
      → gate/up 投影、SiLU 与逐元素乘法
      → down 投影与 residual
  → 最终 RMSNorm
  → LM head
```

实现会把 residual 加法与 RMSNorm 等操作组织成适合编译的函数；这不改变“每层产生自己的 KV、每层读取自己的历史 KV”这一逻辑。

### 7.1 token embedding 与 Q/K/V 的形状

以公开 Qwen3-0.6B 配置、单卡为例：hidden size 为 1024，query heads 为 16，KV heads 为 8，显式 `head_dim` 为 128，共 28 层。[M3]

若本轮实际计算的 token 数为 `T`，形状是：

| 张量 | 形状 |
|---|---|
| token IDs | `[T]` |
| embedding / 层间 hidden states | `[T, 1024]` |
| Q | `[T, 16, 128]` |
| K、V | 各 `[T, 8, 128]` |
| attention 输出展平后 | `[T, 2048]` |
| 输出投影后 | `[T, 1024]` |

这里有一个不能用常识替代配置的细节：`head_dim` 显式为 128，所以 query 投影宽度为 2048，**不能直接套用 `hidden_size / num_heads = 64`**。源码明确支持独立的 head dimension。[S6][M3]

Q 比 K/V 有更多 heads，属于 grouped-query attention 的组织方式；多个 query heads 使用对应的 KV head 组，而不是为每个 query head 都保存一份独立 KV。[S6][D2]

### 7.2 为什么缓存 K/V，不缓存历史 Q

对于当前 query，attention 的数学关系可以简写为：

```text
output = softmax(Q × Kᵀ / sqrt(head_dim) + causal_mask) × V
```

下一次 decode 的 query 来自新的输入 token。历史 token 的 K/V 仍会作为可查询上下文，但历史 Q 不再用于生成下一位置的 attention 输出，所以这里持久保存的是 K/V。[S11]

Q、K 先经过位置编码，随后 K 才进入缓存。因此缓存里的 key 已包含相应位置变换；后续复用必须与它的 token 前缀和位置一致，不能随意把某段缓存搬成不同位置的“相同文本”。[S6][S12]

V 不做这一步 RoPE。`store_kvcache` 接收的是当前层刚计算出的 GPU K/V 张量，不会先回 CPU 再上传。

### 7.3 KV 内存池长什么样

初始化分配的主 KV tensor 形状为：[S3]

```text
[2, num_layers, num_physical_blocks, block_size, local_kv_heads, head_dim]
```

第一维区分 K 和 V；每层 attention 持有其中对应的视图。块表中的一个物理块 ID，实际指向该 ID 在各层中的那组存储位置。

在上述 BF16 配置、单卡条件下，每个 token 的全模型 KV 存储为：

```text
2 × 28 layers × 8 KV heads × 128 head_dim × 2 bytes
= 114,688 bytes
= 112 KiB
```

一个 256-token 物理块对应 **28 MiB**；一个完整 4096-token 上下文对应 **448 MiB**，尚未计入模型权重、激活和其他临时内存。这是配置推导，不是显存分配实测。[M3][S3]

`allocate_kv_cache` 会结合 GPU 总内存、当前使用量、warmup 峰值和 `gpu_memory_utilization` 计算可分配块数。它不是简单把总显存的某个比例全部当成 KV，而是还要为模型及运行峰值留空间。[S3]

## 八、冷 prefill、缓存 prefill 和 decode 的 attention 路径不同

### 8.1 冷 prefill：当前输入自己提供全部 K/V

`Attention.forward` 先用 Triton kernel 把当前层的新 K/V 写到 `slot_mapping` 指定的缓存槽位。如果没有需要读取的历史缓存，`flash_attn_varlen_func` 直接使用本轮投影出的 Q/K/V 张量，并用 cumulative sequence lengths 处理变长 batch。[S11]

所以冷 prefill 中，“写 KV cache”与“attention 必须再从分页 KV 池读回全部新数据”不是一回事：代码会写池以便后续复用，但这条分支仍使用当前 K/V 输入计算 attention。

### 8.2 有前缀或前一 chunk：Q 短，K/V 长

当 `prepare_prefill` 发现总 key 长度大于总 query 长度时，它会构造 block tables。Attention 随后把 K/V 参数切换为缓存池，用分页块表读取完整可见上下文。[S3][S11]

回到 600-token、前 512 已缓存的例子：只为剩余 88 个 token 计算 embedding、投影和各层前向，但它们的 query 仍需要访问那 512 个 token 的历史 KV，加上当前新增 KV。

**前缀缓存省掉的是前缀自身的重复前向计算，不是把前缀从后缀 attention 的上下文中删掉。** 它减少 prefill 工作，却没有让后缀与前缀之间的注意力代价消失。

对于 query/key 长度不相等的 causal attention，mask 还必须按后缀位置对齐。FlashAttention 文档给出的对应语义是右下对齐；以 2 个 query、5 个 key 为例，第一个 query 可以看见前 4 个 key，第二个可以看见全部 5 个，而不是把它们误当成序列最前面的两个位置。[D2]

### 8.3 decode：每条请求一个 query，读取逐渐增长的历史

decode 使用 `flash_attn_with_kvcache`。每条请求本轮只有一个输入 token，Q 会增加一个长度为 1 的维度；K/V 来自已写入新位置的缓存池，并通过 `context_lens` 和 block table 指定可见范围。[S3][S11]

这解释了 decode 的一组特征：

- 每条请求每轮新增的 Q/K/V 很少。
- 要读取的历史 KV 随上下文变长而增加。
- 不需要重算旧 token，不代表每个新 token 的 attention 代价为常数。
- 物理块可以不连续，但逻辑位置和因果关系必须连续正确。

这些是由数据流得到的趋势；具体是计算还是带宽成为瓶颈，还取决于模型、上下文、batch 大小和硬件，不能不经测量就给出统一比例。

## 九、第一个输出 token 到底在哪一步产生

18 个 prompt token 的冷 prefill 会得到相应 hidden states，但 LM head 不需要为所有 prompt 位置都生成下一词分布。`ParallelLMHead` 在 prefill 时用 `cu_seqlens_q[1:] - 1` 选出每条请求本轮最后一个 query 的 hidden state，再做词表投影。[S8]

对于已经完成整个 prompt 的请求，这个最后位置的 logits 正是用来采样**第一个生成 token**的。它不是在“第一次 decode”才出现。

当前 sampler 先除以 temperature、做 softmax，再用指数随机变量构造采样结果；不是简单取最大 logit。`SamplingParams` 只有 temperature、max_tokens 和 ignore_eos，且明确不允许 temperature 接近零的 greedy sampling，不能套用完整 vLLM 的所有采样参数。[S13]

采样发生在 GPU。rank 0 随后调用 `.tolist()` 得到 CPU Python 整数；`postprocess` 才把新 token 加入 `Sequence.token_ids`。[S3]

以 prompt 长度 18 为例，在请求尚未结束的前提下：

| 时刻 | 已知 token 数 | 有效 KV token 数 | 本轮产生的输出 |
|---|---:|---:|---|
| 加入 waiting | 18 | 0 | 无 |
| 完成 prompt prefill 并采样 | 19 | 18 | 第一个生成 token |
| 将这个生成 token 做一次 decode | 20 | 19 | 第二个生成 token |
| 再 decode 一轮 | 21 | 20 | 第三个生成 token |

表格中的计数是请求仍持有缓存时的状态；请求完成并释放块之后，代码会把它的 `num_cached_tokens` 重置为零，不能把这个零误读成 GPU 从未计算过那些 KV。[S2][S9][S10]

还有一个实现细节：**未完成的 prefill chunk 也会经过 LM head 和 sampler，但其采样结果被 postprocess 丢弃**。剩余 prompt 尚未处理完，此时不能把这个临时预测当成用户输出。它说明当前简化实现仍有避免无用采样的优化空间。[S2][S3][S8]

## 十、decode 如何持续生长，何时结束并返回文字

### 10.1 decode 输入是 last_token，不是整段历史

`prepare_decode` 每条请求取 `seq.last_token`，设置位置为 `len(seq)-1`，context length 为 `len(seq)`，并计算该位置的物理槽位。[S3]

以上表第一轮 decode 为例，输入就是刚生成的第一个 token，位置为 18；GPU 为它补齐 KV，然后预测位置 19 的下一个 token。历史 token 不再作为完整 `input_ids` 重送，但它们的 KV 仍在显存里被读取。

### 10.2 新块分配与“差一”关系相互对应

块大小为 256 时，已知长度变成 257，意味着新采样出来、尚无 KV 的位置 256 即将跨入第二个逻辑块。`may_append` 在 `len(seq) % block_size == 1` 时追加物理块，正是为本轮即将写入的位置准备空间。[S10]

这里不能笼统写成“生成出第 256 个 token 就立即写满一个 KV 块”：生成 token ID 与把它作为下一轮输入算出 KV，是先后两步。

### 10.3 停止是 CPU 后处理决定的

`postprocess` 先把本轮已算满的块加入 hash 索引、推进有效 KV 计数；若不是未完成的 prompt chunk，就追加采样结果。随后检查 EOS 或 `num_completion_tokens == max_tokens`，满足条件则标记 FINISHED、释放块引用并从 running 移除。[S2]

`generate` 持续调用 `step`，直到 waiting/running 都清空，最后按请求 ID 顺序整理结果，并调用 tokenizer 的 `decode` 返回 `{"text": ..., "token_ids": ...}`。[S1]

因此，这条公开 API 是离线、最终结果返回的形态，不是内置 HTTP 服务，也没有在 `generate` 中逐 token 向客户端流式发送。EOS 已经被追加到 token list，最终 decode 又没有显式传入 `skip_special_tokens=True`；不能未经检查就假定所有特殊标记都自动从返回文本里消失。

## 十一、把两个请求放到同一条时间线上

为了看清 prefix hit、chunked prefill 和 batch 切换，本文直接驱动原仓库的调度与块管理类，设置：

- KV block size 为 256，prefill token 预算为 384，最大 sequence 数为 4，缓存共 32 块。
- A、B 各有 600 个 prompt token，前 512 个相同，之后不同。
- A、B 依次加入 waiting；每个请求生成 3 个 token，忽略 EOS。
- 用人为指定的 token ID 代替 GPU 采样返回，观察控制面状态，不执行模型。

得到的真实调度轨迹如下：

| step | 类型与成员 | 本轮开始已有 KV | 本轮实际安排的输入 | 后处理结果 |
|---|---|---|---|---|
| 1 | prefill：A | A 为 0 | A 的 384 个 prompt token | A 有 384 个 KV，无生成 token |
| 2 | prefill：A | A 为 384 | A 的剩余 216 个 prompt token | A 有 600 个 KV，生成第一个 token |
| 3 | prefill：B | B 命中 512 | B 的剩余 88 个 prompt token | B 生成第一个 token；A 本轮不 decode |
| 4 | decode：A、B | 各 600 | 各自的最后一个生成 token | 各生成第二个 token |
| 5 | decode：A、B | 各 601 | 各自的最后一个生成 token | 各生成第三个 token，然后结束 |

A 分配到物理块 `[0, 1, 2]`，B 后来得到 `[0, 1, 3]`。前两块是共享的已完成前缀，末块各自独立。

这个实验揭示了三件事：

1. **请求生命周期可以重叠。** A 已经产生首 token，B 仍在 prefill；两者同时处于引擎管理之中。
2. **同一个 decode batch 可以包含多个请求。** 第 4、5 步共同执行 A、B 的 decode。
3. **这里没有 prefill/decode 重叠执行。** 第 3 步为 B prefill 时，已经能继续生成的 A 没有获得本轮 decode 机会。

这三句话并不矛盾：它们分别讨论请求驻留、batch 组成和执行时间。

## 十二、不同 batch 到底有没有 overlap

### 12.1 先明确“overlap”指什么

| 所谓 overlap | 当前版本能否从源码确认 | 原因 |
|---|---|---|
| 多个请求同时未完成、同时持有 KV | 能 | waiting/running 和块引用允许多个请求驻留 |
| 多个请求一起组成一次 GPU forward | 能 | 扁平 token 打包或一请求一 token 的 decode batch |
| 请求结束后，后续 iteration 的 batch 成员变化 | 能 | 按轮调度，结束请求移除，新接纳请求可进入 running |
| 同一 forward 混合 prefill 与 decode | 不能，代码未实现 | 整个 batch 共用一个 `is_prefill` |
| CPU 调度下一 batch，与当前 batch GPU forward 形成显式流水 | 代码未实现 | step 等待 run 返回采样结果，才 postprocess 和下一次 schedule |
| 当前 batch 计算与下一 batch H2D 的专门双缓冲流水 | 代码未实现 | 没有对应独立 stream、双缓冲和跨 batch 提交路径 |

“连续批处理”更接近按 iteration 管理请求和动态重组 batch，并不意味着两个 batch 的 GPU kernel 必须同时运行。与此同时，nano-vLLM 的 prefill 优先和离线入口，也不能被等同于成熟在线引擎的完整公平调度与接纳策略。[S1][S2][S3]

### 12.2 `.tolist()` 是这条链上的关键边界

执行器的关键顺序是：[S3]

```text
CPU: schedule 本轮
  → 准备输入与元数据、提交 H2D
GPU: 模型前向 → logits → sampler
CPU: 等待得到 .tolist() 的整数结果
  → postprocess 更新 token 和缓存状态
  → schedule 下一轮
```

PyTorch 的 `Tensor.tolist` 会在需要时把 tensor 移到 CPU，再产生 Python list。这里需要的不是一个尚未完成的 GPU 指针，而是当前采样结果的真实整数，所以主调度线程不能先拿着这个 list 去跑下一轮，再让采样稍后完成。[D3]

自回归 decode 还有数据依赖：下一轮输入 token 本来就是本轮才采样出的结果。系统可以通过其他请求、延迟读取结果、异步调度或专门流水来隐藏部分 CPU 开销，但当前这条调用链没有实现那些机制。

输入端也类似：`generate` 先遍历所有 prompts，逐条 `add_request` 完成字符串编码，然后才进入执行 `step` 的循环。因此这条默认路径没有把后续请求的 tokenizer 工作放到后台，与当前 batch 的 GPU forward 并行执行。[S1]

### 12.3 CUDA 异步提交，不等于下一 batch 已经在执行

GPU 操作可以相对于 host 异步提交；同一 stream 中的工作按顺序执行，不同 stream 才有在满足条件时并行的可能。`non_blocking=True` 主要影响拷贝相对 host 的等待行为，不能单独证明模型计算与另一 batch 的拷贝重叠。[D1]

当前代码没有围绕 batch 设置独立 copy/compute stream 或异步 scheduler。不能因为在输入准备里看见了 `.cuda(non_blocking=True)`，就画出“batch N 计算时 CPU/GPU 已在完整处理 batch N+1”的时间线。[S3]

这个判断的边界也要说清：它说明**应用层没有实现这种跨 batch 流水**，不等于声称 CUDA、FlashAttention、NCCL 内部不存在任何并行或异步行为。没有 profiler，不能给出它们实际 kernel overlap 的毫秒级结论。

### 12.4 chunked prefill 并没有自动改善这里的 decode 插队

长 prompt 被切成小块，确实限制了单次 prefill 工作量。但只要下一次 `schedule` 还能选择 waiting 中的 prefill，代码仍优先返回 prefill，而不先安排已有 running 请求的 decode。[S2]

所以在本版本里，chunked prefill 不意味着“每处理一块长 prompt，就穿插一轮已有请求的 decode”。前面的五步轨迹已经显示这种区别。对于首 token 和后续 token 延迟的权衡，真正决定公平性的仍然是调度策略，而不只是有没有分块。

## 十三、CUDA Graph 和 Tensor Parallel 改变了什么，没改变什么

### 13.1 CUDA Graph 减少启动开销，不自动改变请求依赖

在未启用 `enforce_eager`、处于 decode 且 batch 不超过 512 时，`run_model` 可以使用预捕获 graph。prefill 则走正常前向路径。仓库示例显式设置了 `enforce_eager=True`，而配置默认是 False；讨论时应说明用的是哪种模式。[S3][S7][S14]

graph 使用预分配输入与输出缓冲，按若干 batch size 桶捕获。运行时选择不小于真实 batch 的桶，把本轮内容写入静态缓冲，再 replay；例如默认桶设置下，17 条 sequence 可以使用 32 的桶。[S3]

补齐位置的 `slot_mapping` 被设为 -1，`context_lens` 被清零；KV 写入 kernel 对 slot -1 直接跳过，避免虚拟行污染真实缓存。真实输出只取前 `batch_size` 行。[S3][S11]

还要看清 graph 边界：捕获的是 `self.model(input_ids, positions)` 这段模型执行，**LM head 和 sampler 不在这段 capture 内**；replay 后仍需计算 logits、采样并 `.tolist()`。因此 graph 不会把整个 CPU 调度循环改造成一个无需 CPU 介入的 GPU 自循环。[S3]

此外，源码给 RMSNorm、RoPE、激活和 sampler 等函数加了 `torch.compile`。这不等价于 scheduler、tokenizer 和全部 Python 引擎都被整体编译。[S12][S13][S15]

要增加跨 batch overlap，也不能只给同一个 runner 再开一条 Python 线程：`graph_vars` 是共享的静态缓冲，attention 使用的 `_CONTEXT` 也是进程级可变状态。并发执行前必须先解决缓冲所有权和上下文隔离，否则后一批可能覆盖前一批仍在使用的输入或映射。[S3][S17]

### 13.2 Tensor Parallel 是同一模型调用的分工，不是独立 batch 流水

启用 TP 时，rank 0 承担主引擎的请求管理，并通过共享内存与 event 向其他 worker 广播执行方法和 sequence 状态。各 rank 都执行对应模型分片，并各自维护本地 KV heads 的缓存。[S1][S3]

QKV 和 gate/up 投影沿输出维度分片；输出投影及 down projection 等 row-parallel 层需要 `all_reduce` 汇总部分结果。LM head 产生分片词表 logits，再收集到 rank 0；采样仅在 rank 0 执行。[S6][S8][S16]

decode 广播也做了一个小优化：`Sequence.__getstate__` 在 decode 状态主要携带最后一个 token、计数和块表，而不是每轮广播完整历史 token list；prefill/recompute 则需要完整 token 历史。[S9]

这属于一个 batch 的跨 GPU 协作。它不会让 GPU 0 随意跑 batch A、GPU 1 随意跑 batch B：TP 通信必须满足同一次模型执行的参与关系。当前代码也没有显式使用 `async_op=True` 搭建可独立调度的通信/计算重叠流水；通信库内部行为仍应另行测量。[S3][S16]

## 十四、把机制映射成实际排查问题的方法

### 14.1 “明明提示词相同，为什么没有 prefix hit？”

先检查 token，而不是比较用户可见文字：模板和特殊标记是否一致？是否有至少两个逻辑块？此前计算是否已经完成并发布 hash？块是否已被其他请求覆盖？是不是把同一冷 batch 内的重复请求误认为会做 in-flight 去重？[S7][S10]

尤其是短 prompt：默认 256-token 块大小加上末块不复用策略，使 18-token 的重复输入零 prefix hit 完全符合代码预期，而不是证明历史 KV cache 失效。

### 14.2 “为什么已经出首 token 的请求，后续 token 停了一会儿？”

检查那几轮 scheduler 是否在为 waiting 请求做 prefill。prefill-first 可以让一个已进入 running 的请求等待；不应先把所有停顿归因为 GPU attention kernel 变慢。[S2]

把已初始化请求的一轮 decode 时间分解为以下部分，有助于定位问题：

```text
调度与状态处理
  + CPU 输入打包
  + H2D 与依赖等待
  + Transformer forward
  + LM head / sampling
  + 结果回到 CPU
```

这些是需要测量的组成项，不是相互独立、可以简单假定完全重叠的六条流水。

### 14.3 “命中了很长前缀，为什么生成阶段仍不便宜？”

prefix hit 跳过前缀自身的前向计算，但每个新 query 仍要读相应历史 KV。长上下文的 decode 并不会因为开头来自缓存，就变成只关注最后一个 token 的计算。[S11]

### 14.4 “怎么判断 batch 优化是否真的实现了 overlap？”

先从源码寻找异步接纳、结果延迟回收、独立 streams、双缓冲和跨 iteration 依赖管理，再用 profiler 验证。看见 CUDA Graph、continuous batching 或 non-blocking 中任意一个名字，都不足以证明完整 overlap。

若要进一步做端到端实验，应固定本文的代码和模型快照，分别比较 eager/graph、冷前缀/热前缀、短请求/长请求混合，并同时记录 TTFT、逐 token 延迟、batch 成员、每轮 scheduled token 数与 CUDA 时间线。本文的小实验已经验证控制面和输入地址，但没有替代这一步。

## 结语

沿请求生命周期看，nano-vLLM 的核心协作关系是：**CPU 负责 token 历史、调度和块身份；GPU 负责当前输入的数值计算与历史 KV 访问；采样结果回到 CPU 后，下一轮状态才能继续推进。**

前缀缓存决定哪些位置可以不重算，分页映射决定这些 KV 放在哪里，batch 决定本轮一起计算哪些请求；三者都很重要，却不是同一个概念，更不能自动推出跨 batch overlap。

最值得对照阅读的调用链是 `LLMEngine.step → Scheduler.schedule → ModelRunner.prepare_* → Qwen3ForCausalLM → Attention → Sampler → Scheduler.postprocess`。先看清每一步数据在哪里、哪些状态已经有效，再讨论缓存命中和并发，很多看似神秘的行为就能解释清楚。[S1][S2][S3][S6][S11][S13]

---

## 参考资料与验证范围

源码引用固定到 `bb823b3e06983d71485a8e1f23715ebd87d98ef8`。本文实跑环境用于 tokenizer、控制面和输入准备验证的版本是 PyTorch 2.9.1+cu128、Transformers 4.57.3；输入准备方法在 RTX 3090 上执行。完整模型 forward 和 FlashAttention kernel 的结果、性能、时间重叠未实测。

控制面实验执行的是仓库原始类，用人为给定的下一 token 驱动 `postprocess`；输入准备实验通过 AST 提取原始 `prepare_prefill`、`prepare_decode` 和 `prepare_block_tables` 方法以隔离完整模型依赖，本次实际核对的是 prefill 打包。不能把这两类实验描述为真实语言生成或 KV 数值正确性测试。

- [S1] [LLMEngine：请求加入、step、generate 与最终 decode](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/engine/llm_engine.py#L43)。
- [S2] [Scheduler：prefill-first、chunked prefill、抢占与 postprocess](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/engine/scheduler.py#L25)。
- [S3] [ModelRunner：初始化、KV 池、输入准备、执行与 graph](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/engine/model_runner.py#L15)。
- [S4] [LLM：引擎入口](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/llm.py#L1)。
- [S5] [权重加载与 packed module 映射](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/utils/loader.py#L12)。
- [S6] [Qwen3：attention、MLP、decoder layer 与模型前向](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/models/qwen3.py#L14)。
- [S7] [官方示例：先应用 chat template，再 generate](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/example.py#L6)。
- [S8] [Embedding 与 LM head：词表查找和 prefill 最后位置选择](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/layers/embed_head.py#L34)。
- [S9] [Sequence：计数、逻辑块、追加 token 与跨进程序列化](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/engine/sequence.py#L14)。
- [S10] [BlockManager：前缀 hash、块复用、释放与发布](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/engine/block_manager.py#L26)。
- [S11] [Attention：Triton KV 写入与两条 FlashAttention 路径](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/layers/attention.py#L10)。
- [S12] [RoPE：Q/K 的位置变换](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/layers/rotary_embedding.py#L6)。
- [S13] [Sampler：temperature、softmax 与采样](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/layers/sampler.py#L5)，以及 [SamplingParams 的约束](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/sampling_params.py#L4)。
- [S14] [Config：默认块大小、token 预算和 eager 开关](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/config.py#L6)。
- [S15] [RMSNorm 与融合 residual 路径](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/layers/layernorm.py#L5)，以及 [SiLU 与乘法](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/layers/activation.py#L6)。
- [S16] [并行 Linear：QKV 分片、row-parallel 与 all-reduce](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/layers/linear.py#L96)。
- [S17] [Context：进程级 attention 执行上下文](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/utils/context.py#L6)。
- [M1] [Qwen3-0.6B 固定快照的 tokenizer 配置与 chat template](https://huggingface.co/Qwen/Qwen3-0.6B/blob/c1899de289a04d12100db370d81485cdf75e47ca/tokenizer_config.json)。
- [M2] [同一快照的 tokenizer.json](https://huggingface.co/Qwen/Qwen3-0.6B/blob/c1899de289a04d12100db370d81485cdf75e47ca/tokenizer.json)。
- [M3] [同一快照的模型 config.json](https://huggingface.co/Qwen/Qwen3-0.6B/blob/c1899de289a04d12100db370d81485cdf75e47ca/config.json)。
- [D1] [PyTorch 2.9.1 CUDA semantics：异步执行、streams 与同步](https://github.com/pytorch/pytorch/blob/v2.9.1/docs/source/notes/cuda.rst#L277)。
- [D2] [FlashAttention 官方说明：GQA、KV cache 与 causal mask 对齐](https://github.com/Dao-AILab/flash-attention/blob/main/README.md)。
- [D3] [PyTorch Tensor.tolist：必要时先移动到 CPU](https://docs.pytorch.org/docs/stable/generated/torch.Tensor.tolist.html)。
