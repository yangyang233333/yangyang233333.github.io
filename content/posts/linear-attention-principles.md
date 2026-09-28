---
title: "线性注意力机制：从矩阵重排到递推记忆"
date: 2026-09-28T11:04:00+08:00
draft: false
tags: ["线性注意力", "Transformer", "GLA", "DeltaNet", "推理优化"]
categories: ["深度学习", "技术原理"]
summary: "从核函数与矩阵乘法结合律推导线性注意力，解释固定大小的递推状态、分块并行训练，以及 GLA、DeltaNet、Gated DeltaNet 如何管理有限记忆，并用 NumPy 验证等价关系。"
---

线性注意力最重要的变化，不只是把一个算子的复杂度从平方降为线性，而是改变了访问历史的方式：**不再为每个新 query 重新扫描全部历史 K/V，而是把历史持续写入一个固定大小的状态，再从这个状态读取结果。**

这带来了更可控的长序列计算和解码缓存，也把问题从“如何更快地访问历史”变成了“有限状态应该记住什么、遗忘什么，以及怎样修改已有记忆”。本文沿经典核注意力、GLA、DeltaNet 和 Gated DeltaNet 这条路线解释原理，不把所有线性时间序列模型视为同一种公式。

## 1. 线性的是序列长度，不是注意力函数

先看单头 softmax attention。设序列长度为 `T`，query/key 宽度为 `d_k`，value 宽度为 `d_v`，省略投影、多头和输出层：

```text
Q, K: [T, d_k]       V: [T, d_v]

Y = softmax(Q @ K.T / sqrt(d_k) + M) @ V
```

这里 softmax 按行计算。因果场景下，mask `M` 在允许访问的位置为零，在未来位置为负无穷，因此当前位置不能看到后续 token。

核心开销是两次跨序列混合：计算 query 与 key 的匹配关系，再用这些权重聚合 value。其算术量随 `T` 平方增长；朴素实现还会显式保存 `T × T` 的注意力矩阵。

**FlashAttention 与线性注意力解决的不是同一个问题。** FlashAttention 通过分块、在线 softmax 和重计算减少显存读写，不必把完整注意力矩阵存到 HBM，但仍计算原来的精确注意力，完整稠密注意力的算术复杂度仍是平方级。这里的“精确”指不引入注意力近似，而不是保证不同实现逐 bit 相同。

线性注意力则改变或近似跨 token 的交互算子，使计算量在固定特征宽度下随 `T` 线性增长。它不意味着整个网络是线性函数：特征映射、门控、归一化和前馈网络都可以是非线性的。

## 2. 从核函数推导固定大小的状态

### 2.1 先让相似度可以拆开

把注意力写成“相似度加权平均”。对于位置 `t`，因果注意力只聚合 `j <= t` 的 value：

```text
w_tj = phi(q_t).T @ phi(k_j)

y_t = sum_{j <= t}(w_tj * v_j)
      / sum_{j <= t}(w_tj)
```

`phi` 把 query/key 映射到 `m` 维特征空间。经典归一化核注意力通常选择非负特征，让相似度非负；例如早期 Linear Transformer 使用逐元素的 `ELU(x) + 1`。

但必须区分两条路线：

- **改用另一种相似度核**：例如 `ELU(x) + 1` 的点积。它定义了不同的注意力，并不天然等于 softmax attention。
- **近似 softmax 对应的核**：例如 Performer 用正交随机特征近似指数点积核。此时需要考虑特征数量、采样方式和近似误差。

因此，不能直接把已训练模型的 softmax 替换成某个正特征映射，就期待输出或模型质量保持不变。

### 2.2 结合律省掉的是哪一个矩阵

令 `Qf = phi(Q)`、`Kf = phi(K)`，它们的形状都是 `[T, m]`。暂时不加因果 mask，只看分子：

```text
(Qf @ Kf.T) @ V = Qf @ (Kf.T @ V)

Qf @ Kf.T : [T, T]
Kf.T @ V  : [m, d_v]
```

左边先得到所有 token 两两之间的权重；右边先把 key 与 value 汇总成一个小矩阵。两者的差别不是换了硬件，而是改变乘法顺序，避开了 `T × T` 中间结果。

这个重排对选定的点积核成立，**不能隔着逐行 softmax 直接使用结合律**。softmax 的指数和归一化不能被随意搬到另一个括号外面。

### 2.3 因果场景需要前缀状态，而不是全局状态

恢复因果约束后，每个位置只能使用自己的历史前缀。定义 `kf_t = phi(k_t)`、`qf_t = phi(q_t)`，统一把向量看作列向量，用 `outer` 表示外积：

```text
S_t = S_{t-1} + outer(kf_t, v_t)     S: [m, d_v]
z_t = z_{t-1} + kf_t                z: [m]

y_t = (S_t.T @ qf_t) / (z_t.T @ qf_t)
```

初始状态 `S_0` 和 `z_0` 都为零。`S` 累积 key-value 关联，`z` 累积归一化分母所需的 key 特征。实现中通常给分母加一个小的 `epsilon`，防止接近零时出现数值问题。

这已经是一种递推网络：输入一个新 token，更新状态，然后用当前 query 读取状态。每个位置都使用自己的 `S_t`，而不是让所有位置共享最终的 `S_T`。后者会把未来 token 混入早期输出，破坏因果性。

注意 `z` 不是可有可无的装饰。删掉它，就从这里推导的归一化核注意力变成了另一种算子；有些现代线性注意力确实使用不同的归一化方式，但必须按其完整架构理解。

## 3. 到底省了多少计算和缓存

下面只比较单头的注意力混合部分，不计 Q/K/V 投影、特征映射本身、MLP、输出投影及其它层。`m`、`d_k`、`d_v` 都视为固定参数：

| 项目 | 完整 softmax attention | 上述归一化核注意力 |
| --- | --- | --- |
| 长度 T 的序列混合算术量 | O(T² × (d_k + d_v)) | O(T × m × d_v) |
| 已有 T 个历史 token 时，解码一个新 token | O(T × (d_k + d_v)) | O(m × d_v) |
| 解码需要保留的历史信息 | T × (d_k + d_v) 个 K/V 元素 | m × d_v + m 个状态元素 |

所以，“常数内存”是指：**固定模型配置下，每条序列、每层、每个状态头的解码历史状态不随历史长度增长。** 它不表示整个模型只占常数内存。

举一个只计算历史状态的例子：令 `d_k = d_v = m = 128`、`T = 32768`。该头的 BF16 K/V 占 **16 MiB**；若 `S` 和 `z` 使用 FP32 累积，则占 **64.5 KiB**。这是指定维度与数据类型的容量计算，不是实测整模型显存，也没有包括工作区、batch 和其它激活。实际还要考虑层数、状态头数量、GQA 的 K/V 共享和混合架构。

固定状态也有固定成本，短序列下未必比保存少量 K/V 更划算。提高 `m` 可以改变表达能力或近似误差，但也会增大状态与每步计算量；“对 T 线性”并没有让特征宽度的代价消失。

## 4. 递推不等于训练必须串行

解码时一次只有一个新 token，逐步更新状态很自然；训练和 prefill 已经拿到了整段输入，如果仍然启动大量细小的逐 token 算子，GPU 很容易利用不足。

对前面的加性状态，更新只是外积的前缀和，可以组织成并行扫描或分块计算。以一个包含 `C` 个 token 的块为例，分子的计算可以拆成：

```text
Numerator_block =
    Qf_block @ S_in
  + tril(Qf_block @ Kf_block.T) @ V_block

S_out = S_in + Kf_block.T @ V_block
```

第一项读取进入当前块之前的历史状态；第二项计算块内的因果交互。分母也按同样思路，由块前的 `z` 和块内 key 前缀共同构成。这样既能维持跨块递推，又能把大量工作组织成适合 GPU 的矩阵乘法。

这里并非完全没有注意力小矩阵：块内可以有 `C × C` 计算，`C` 是受控的实现参数。如果把块大小一直增大到整段序列，就不能继续忽略块内平方项。

加入门控、delta rule 后，状态更新不再只是简单的外积求和，需要相应的分块变换、结构化矩阵乘积和重计算策略。GLA 与并行 DeltaNet 的工作不仅提出了状态更新方式，也解决了怎样让这些更新高效映射到现代硬件的问题。

**推理状态固定，不等于训练总内存固定。** 反向传播仍然需要输入激活、块边界状态或重计算；训练显存取决于具体保存策略与 kernel 实现。

## 5. 从只会累加，到会遗忘、会改写

只做外积累加，会把很多关联叠加到同一块有限状态里。不同 key 之间如果存在相关性，读取时就可能发生干扰。现代变体的核心问题是：如何管理这块状态，而不只是重复强调线性复杂度。

本节继续采用 `S: [m, d_v]`，但 `q_t`、`k_t` 改为表示各模型处理后的 query/key，不再限定为上一节的正特征。下面只展示状态更新骨架，省略输出归一化、投影和其它门控，不能把它们当成完整网络实现，也不默认保留上一节的 `z`。

### 5.1 GLA：让过去按输入相关的速度衰减

GLA 的代表性做法，是为 key 侧特征通道引入输入相关的遗忘门。用列向量约定，可以写成：

```text
S_t = diag(g_t) @ S_{t-1} + outer(k_t, v_t)
```

`g_t` 的不同分量控制不同状态行的衰减。与永远累加相比，模型可以保留部分通道中的信息，同时更快清除另一些信息。

门控改善了状态管理，但“衰减某些通道”和“准确替换某个 key 对应的 value”仍不是同一件事。

### 5.2 DeltaNet：先读取旧值，再写入误差

Delta rule 先问一个更具体的问题：当前状态对这个 key 的回答是什么？然后只写入预测与新 value 的差值：

```text
predicted_value = S_{t-1}.T @ k_t
error_t = v_t - predicted_value

S_t = S_{t-1} + beta_t * outer(k_t, error_t)
```

这相当于对“用当前 key 预测 value”的平方误差做一步在线梯度更新。被更新的是序列运行时的状态矩阵，不是每生成一个 token 就反向训练整套模型参数，这也是 fast-weight memory 视角的含义。

在 `k_t` 的二范数为 1、`beta_t` 位于 0 到 1 之间的简化情况下，新状态对同一个 key 的读取结果满足：

```text
S_t.T @ k_t =
    (1 - beta_t) * predicted_value + beta_t * v_t
```

因此 `beta_t = 1` 时，该 key 方向上的读取被更新为新 value，而不是不断叠加旧值。但这不保证其它 key 的结果完全不变：只要它们与当前 key 不正交，更新仍然可能产生干扰。

### 5.3 Gated DeltaNet：遗忘与纠错结合

Gated DeltaNet 把状态衰减和 delta 更新结合起来。用本文的矩阵方向，可写成：

```text
S_bar = alpha_t * S_{t-1}
error_t = v_t - S_bar.T @ k_t
S_t = S_bar + beta_t * outer(k_t, error_t)
```

`alpha_t` 控制旧状态保留多少，`beta_t` 控制针对当前 key 的修正幅度。这里读取旧值时使用的是**衰减后的状态**；不能把遗忘门放到任意位置，还认为公式保持不变。

2026 年的 Gated DeltaNet-2 进一步把“擦除旧关联”和“写入新值”的控制解耦，并引入通道级门控，沿着这条路线研究更细粒度的状态更新。

## 6. 能力边界：省下来的不是免费午餐

**首先是历史信息的取舍。** 标准自回归 attention 保留每个历史位置的 K/V，新 query 可以重新比较这些位置；固定状态方法则必须在未来 query 尚未出现时持续汇总历史。固定维度、固定精度的状态不可能无损保留任意长历史中的任意信息，这会影响精确检索、多条关联同时保留等任务。

但不要把这种限制简单解释为“所有线性注意力矩阵的秩都不超过 m”。对于无因果 mask 的纯核矩阵 `Qf @ Kf.T`，秩确实不超过 `m`；施加三角因果 mask 后，这个全局秩结论不再成立。更稳妥的理解是：每个时刻能传递给未来的信息，受递推状态及更新机制约束。

**其次是数值稳定性。** 长时间累积会改变状态尺度；遗忘门的连乘、低精度加法和接近零的分母，也都会引入问题。特征归一化、累积精度、门控参数化与分块缩放，不只是实现细节，可能直接影响可训练性和长上下文表现。

**最后是实际速度。** 渐近复杂度不能替代吞吐和延迟测试。短序列、较大的状态、低效的 scan、大量 kernel 启动，或未利用 Tensor Core 的实现，都可能吃掉理论收益。公平比较需要固定硬件、batch、精度、上下文长度和模型质量，并分别测量 prefill 与 decode。

这也是混合架构有意义的原因：用递推层降低大部分序列混合成本，同时保留部分全局或局部 attention 层，承担不同的历史访问需求。这样的模型不能再被笼统地描述成“完全没有 KV cache”；整体缓存和复杂度要按每种层分别计算。

## 7. 用代码检验等价关系

下面的 NumPy 示例直接生成正的 query/key 特征，用 `float64` 比较显式因果权重矩阵与逐 token 递推。它只验证**同一个核注意力算子**的两种计算方式，不是在证明它与 softmax attention 等价。

```python
import numpy as np

generator = np.random.default_rng(7)
query_features = generator.uniform(0.1, 1.0, size=(32, 8))
key_features = generator.uniform(0.1, 1.0, size=(32, 8))
values = generator.normal(size=(32, 4))
epsilon = 1e-12

weights = np.tril(query_features @ key_features.T)
parallel_output = (weights @ values) / (
    weights.sum(axis=1, keepdims=True) + epsilon
)

state = np.zeros((key_features.shape[1], values.shape[1]))
normalizer = np.zeros(key_features.shape[1])
recurrent_output = np.empty_like(values)

for index, (query, key, value) in enumerate(
    zip(query_features, key_features, values)
):
    state += np.outer(key, value)
    normalizer += key
    recurrent_output[index] = (query @ state) / (
        query @ normalizer + epsilon
    )

np.testing.assert_allclose(
    parallel_output, recurrent_output, rtol=1e-12, atol=1e-12
)
print("max_abs_error:", np.max(np.abs(parallel_output - recurrent_output)))
```

除这个示例外，本文的数值检查还覆盖了 15 组序列与特征形状、75 次不同块大小的比较、状态接续，以及修改未来 token 不影响前缀输出的检查。递推和分块结果相对显式核注意力的最大绝对误差低于 `1e-12`，delta 更新的单位 key 恒等式与 Gated DeltaNet 的衰减顺序也通过了独立检查。这些检查确认的是公式与实现的一致性，不代表完成了模型训练或真实 GPU 性能评测。

## 小结

理解线性注意力，可以抓住两个问题：**历史怎样被压缩成可递推的状态，以及这块状态怎样被查询、遗忘和改写。** 核函数与结合律解释了计算为何能线性化；门控、delta rule 和分块算法则决定这种结构是否既好用又高效。

## 参考资料

- [Katharopoulos 等：Transformers are RNNs，ICML 2020，核注意力与因果递推推导](https://proceedings.mlr.press/v119/katharopoulos20a.html)
- [Choromanski 等：Rethinking Attention with Performers，正交随机特征与 softmax 核近似](https://arxiv.org/abs/2009.14794)
- [Dao 等：FlashAttention，精确注意力的 IO 优化与分块计算](https://arxiv.org/abs/2205.14135)
- [Schlag 等：Linear Transformers Are Secretly Fast Weight Programmers，ICML 2021，快速权重记忆与 delta rule](https://proceedings.mlr.press/v139/schlag21a.html)
- [Yang 等：Gated Linear Attention Transformers with Hardware-Efficient Training，输入相关门控与高效训练](https://arxiv.org/abs/2312.06635)
- [Yang 等：Parallelizing Linear Transformers with the Delta Rule over Sequence Length，DeltaNet 的分块并行算法](https://arxiv.org/abs/2406.06484)
- [Yang 等：Gated Delta Networks，原始论文第 3.1 节的门控 delta 更新与第 3.2 节的分块算法](https://arxiv.org/html/2412.06464v1)
- [NVIDIA Research：Gated DeltaNet-2，2026 年提出的擦除与写入解耦机制](https://research.nvidia.com/publication/2026-05_gated-deltanet-2-decoupling-erase-and-write-linear-attention)
