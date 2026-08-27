---
title: "tiny-llm：在 Apple Silicon 上从 Transformer 算子走到推理系统与 Agent"
date: 2026-08-27T15:10:20+08:00
draft: false
tags: ["LLM", "推理系统", "MLX", "Apple Silicon", "Agent"]
categories: ["源码阅读"]
---

如果你已经知道 Transformer 的基本结构，却仍然不清楚 vLLM 一类推理系统为什么需要 KV Cache、Continuous Batching、Paged Attention，以及这些技术最终如何支撑 Agent，`skyzh/tiny-llm` 是一条很好的动手路径。

它不是一个追求生产可用的迷你框架，而是一门以 Qwen3 和 MLX 为载体的系统课程：先用可读的数组运算实现模型，再围绕真实性能瓶颈写 Metal 内核，随后搭出动态批处理和分页缓存，最后把同一个本地模型接入带工具、检查点、压缩、评测和分支选择的 Agent 循环。

截至 2026 年 8 月 27 日，项目约有 4.5k stars，使用 Apache-2.0 许可证。课程主体分为四周，已覆盖从注意力到受控 Agent 的完整主线。

## 项目定位：不是“再写一个 Transformer”

很多教学项目止步于“加载权重并生成一句话”。tiny-llm 的不同之处，是把模型实现当作起点，而不是终点。

它试图回答三个递进的问题：

1. 一个现代开源模型如何由矩阵运算变成文本？
2. 一个正确但缓慢的实现，如何通过测量和内核优化接近成熟框架？
3. 一个推理引擎如何进一步成为可暂停、可审计、可恢复的 Agent 执行器？

项目选择 Apple Silicon、MLX 和 Qwen3。统一内存让 CPU、GPU 和模型权重处在相对简单的硬件环境里；MLX 提供熟悉的 Python 数组接口，同时允许下沉到 Metal；Qwen3 则带有 GQA、QK Norm、BF16 激活和 4-bit 权重，足以暴露真实推理系统中的带宽、注意力与缓存问题。

代码结构刻意分成两套：

- `src/tiny_llm/` 是学习者逐步补全的实现；
- `src/tiny_llm_ref/` 是参考实现，用于测试和基准对照；
- `src/extensions/` 与 `src/extensions_ref/` 分别放置 Metal/C++ 扩展；
- `tests_refsol/` 按周和天组织验收测试；
- `book/` 是完整课程正文。

这种布局把“阅读—实现—测试—测量”闭成了环。

## 第一周：先把 Qwen3 讲清楚

第一周从注意力开始，依次实现 RoPE、GQA、RMSNorm、MLP、Transformer Block、模型加载、解码和采样。默认模型是 `Qwen/Qwen3-0.6B-MLX-4bit`，但教学实现会先解量化，再以 BF16 权重和激活运行。

这一阶段最重要的不是代码量，而是接口边界。以 Qwen3 注意力为例，学习者需要处理：

- Query 头数与 KV 头数不同的 GQA；
- Q、K 投影后的独立 RMSNorm；
- RoPE 对位置的编码；
- causal mask；
- logits 到 token 的采样策略。

最终模型仍然很朴素：每次生成新 token 都重新处理完整上下文。它足以证明模型实现正确，也恰好暴露出下一阶段必须解决的问题——重复计算。

## 第二周：从 KV Cache 到自定义 Metal 内核

第二周的主线很接近真实系统优化方法：先建立基线，再用 profiling 选择优化对象，而不是凭感觉改代码。

### KV Cache 改变了计算形态

有了 KV Cache，prefill 只执行一次，后续 decode 每步只输入一个 token。注意力不再重复计算历史 token 的 K 和 V，但模型进入了另一种瓶颈：单 token 解码中的大量线性层更像 memory-bound 的矩阵向量乘，而不是吞吐友好的大矩阵乘。

这也是课程接下来优化 4-bit 权重投影的理由。

### 优化顺序由数据决定

项目附录给出的 M4 Pro、Qwen3-4B 数据很有代表性：

| 阶段 | Decode 吞吐 |
|---|---:|
| 加入 Dense KV Cache | 21.73 tok/s |
| Packed W4A16 MatVec | 55.96 tok/s |
| Fast RMSNorm | 63.70 tok/s |
| Fast RoPE | 66.20 tok/s |
| Fused SwiGLU | 67.83 tok/s |

其中，Packed W4A16 MatVec 一步就把解码吞吐提高约 157.5%。原因不是减少了模型参数，而是避免在每个 token 上反复展开 4-bit 权重，并让内核调度更适合 `M=1` 的 decode 形态。

之后，课程继续实现或融合 RMSNorm、RoPE、SwiGLU 与短上下文 decode attention。它传达了一个很重要的工程经验：优化对象会随前一轮优化而变化。量化投影优化后，原本不起眼的逐点算子才成为值得处理的成本。

### Prefill 与 Decode 需要不同内核

同一个线性层，在 prefill 时面对多行输入，在 decode 时通常只有一行。tiny-llm 因此没有试图用一个万能内核解决全部形态，而是逐步引入：

- 面向单 token 的 packed matvec；
- 面向矩阵输入的 SIMD/cooperative matrix prefill；
- 面向短行、低占用投影的 Split-K；
- 面向不同上下文和 query 长度的 attention dispatch guard。

这比只展示一个“快多少倍”的 kernel demo 更接近推理引擎：性能来自工作负载分类和调度边界，而不只是某段 shader。

## 第三周：搭出一个 tiny vLLM

第三周从单请求转向服务系统，核心是把请求调度、缓存分配和 attention 执行联系起来。

### Continuous Batching

传统静态批处理要等整个 batch 结束后才能接纳新请求。Continuous Batching 则在每轮 decode 后移除已完成请求，并把等待请求补入空位。

项目中的 `TinyLlmBatch` 维护请求状态、当前位置、生成结果和 slot；调度器每轮执行后更新 batch，而不是把批次视为不可变张量。模型侧接收的是动态 slot 映射，因此不同请求可以在同一轮中处于 prefill 或 decode 阶段。

### Chunked Prefill

长 prompt 如果一次性 prefill，会阻塞已经在 decode 的短请求。Chunked Prefill 把长 prompt 切成有界块，让调度器可以在轮次之间重新安排工作，从而在吞吐和交互延迟之间取得更好的平衡。

这项技术的价值不在于减少总计算量，而在于降低 head-of-line blocking。课程把它放在 Continuous Batching 之后，顺序非常自然：只有调度器能逐轮重排请求，切块才真正有意义。

### Paged KV Cache

Dense KV Cache 往往按最大上下文连续预留空间，容易产生内部碎片，也不利于请求动态增长。tiny-llm 用固定大小 page 组成共享池：

```text
请求 A: [page 7] -> [page 2] -> [page 11]
请求 B: [page 4] -> [page 9]
共享池: 按需分配、完成后归还
```

`TinyKvPagedPool` 管理物理 page，`TinyKvPagedCache` 保存单请求的逻辑序列与 page table。随后 attention 内核不再要求先把 KV 拼成连续张量，而是直接按照 page table 读取离散页面。这就是从“分页存储”走向“Paged Attention”的关键一步。

课程又进一步实现 Paged FlashAttention，把分页寻址与在线 softmax 放到同一内核中，避免为完整 attention score 矩阵分配中间存储。

此外，第三周还提供 Speculative Decoding 与 MoE 作为可选章节。它们不是孤立的高级主题：前者复用草稿模型减少主模型步数，后者引入专家路由和稀疏计算，都建立在前面清晰的模型与调度边界之上。

## 第四周：推理系统如何变成 Agent

第四周是这个项目最有辨识度的部分。许多课程在实现 Paged Attention 后结束，tiny-llm 则继续追问：当模型能够调用工具、修改文件、执行命令时，系统需要哪些新语义？

答案不是简单地套一个 `while` 循环。

### 可验证的工具协议

Agent 输出必须解析为结构化的 `FinalAction` 或 `ToolAction`。工具执行受 `ToolPolicy` 和工作区边界约束，文件修改需要审批，命令必须精确匹配配置。系统记录的不只是聊天消息，还包括工具动作、结果和状态事件。

这让失败是显式状态，而不是被隐藏在自然语言里。

### Checkpoint、恢复与压缩

Agent 执行具有副作用。恢复时如果简单重放历史，就可能再次写文件或重复执行命令。项目因此在“完整工具观察边界”创建检查点，保存模型状态、消息和执行记录；恢复使用新模型实例，但不会重放已经完成的副作用。

长会话还会遇到上下文膨胀。课程将旧交互压缩为模型可见摘要，同时保留精确 receipt，记录动作、结果与变更产物。模型上下文可以缩短，审计事实不能丢失。

### 分支不是回滚世界

课程支持从同一个 tokenizer/KV checkpoint 派生多个 continuation，给每个分支加入不同 steering 指令，再用统一评测器选择通过者。

值得注意的是，每个分支使用隔离的副作用工作区。所谓 fork 只是复用模型前缀，不是假装现实世界的修改可以自动回滚。这一点比“让模型多生成几个答案再投票”严谨得多。

### 有界工具证据

工具输出可能大到无法安全塞进 prompt。项目把原始字节存到 `ArtifactStore`，只向模型暴露身份、摘要、长度和头尾片段；模型需要更多内容时，再按明确字节范围读取。

这种设计同时解决了上下文预算、可审计性和内容完整性问题，也把推理层的 KV 前缀复用与 Agent 层的证据管理连接起来。

## 为什么这套课程设计有效

### 1. 每一周都保留上一周的因果链

KV Cache 不是凭空出现，而是为消除 Week 1 的重复计算；量化 matvec 是 profiling 选出的瓶颈；Paged KV Cache 是动态批处理后的内存问题；Agent checkpoint 又复用了前面已经建立的生成状态概念。

学习者看到的不只是功能列表，而是系统为何逐步长成现在的形状。

### 2. 正确性与性能证据分开

测试负责验证数值、边界和协议；benchmark 负责说明性能；书中还明确区分 CI 持续验证与单机研究数据。这避免了常见的错误：用一次微基准证明整个模型更快，或者把固定硬件上的数据包装成普遍结论。

### 3. 学习者实现与参考实现并存

双包结构既方便课程挖空练习，也能让测试和 benchmark 始终有可信对照。按天复制测试的工具则保证学习路径是渐进的，不要求一开始就通过整个仓库。

### 4. 把限制写进课程

项目明确说明未覆盖量化 KV Cache、跨请求 prefix caching、微调和更多长上下文技术；Week 2/3 的性能结论也限定在具体芯片、模型与 MLX 版本。对系统课程来说，边界声明和代码同样重要。

## 适合谁，不适合谁

它最适合以下读者：

- 理解 Transformer，但缺少推理系统实现经验；
- 使用 Apple Silicon，希望在本地学习 GPU kernel；
- 想理解 vLLM 的设计动机，而不是直接阅读大型生产代码库；
- 对 Agent 的可靠执行、恢复和评测机制感兴趣。

它不适合直接用作生产服务，也不是 CUDA/vLLM 源码的替代品。课程依赖 macOS Apple Silicon，性能数据不能直接外推到 NVIDIA GPU；实现也刻意省略了分布式推理、跨请求前缀缓存、完整服务协议和生产级故障处理。

## 建议的学习方式

不要先通读整本书再开始写代码。更有效的节奏是：

1. 先完成 Week 1，用 0.6B 模型跑通生成；
2. 每做一项 Week 2 优化，先记录基线，再看 profile 是否支持下一步；
3. 在 Week 3 画出请求状态、slot、page table 和物理 page 的关系；
4. 到 Week 4 时，把“模型状态”和“外部副作用状态”分开思考；
5. 最后再读 performance appendix，检查自己的结果是否具有相同趋势，而不是追求完全相同的数字。

## 总结

tiny-llm 的价值不在于代码“tiny”，而在于学习路径足够完整：它用一个小而可运行的系统串起模型结构、硬件内核、服务调度、缓存管理和 Agent 可靠性。

如果你想理解现代 LLM 推理系统各组件之间的因果关系，而不只是记住 KV Cache、Paged Attention、Continuous Batching 这些名词，这个项目值得按课程顺序亲手完成。

项目地址：<https://github.com/skyzh/tiny-llm>

课程文档：<https://skyzh.github.io/tiny-llm/>
