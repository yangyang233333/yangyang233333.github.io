---
title: "Mini-SGLang 源码阅读（三）：Paged KV Cache 与 Radix Prefix Cache"
date: 2026-08-25T20:50:00+08:00
draft: false
tags: ["Mini-SGLang", "KV Cache", "Radix Cache", "源码阅读"]
categories: ["大模型推理"]
description: "从自回归推理的 KV Cache 成本出发，分析 Mini-SGLang 的物理页池、请求页表、前缀匹配与缓存驱逐。"
---

KV Cache 是在线 LLM 推理中最重要的状态。它避免 Decode 每轮重新计算全部历史 token，却也通常成为显存容量和并发数的主要限制。

本文先解释 KV Cache、分页管理和前缀复用的基本原理，再阅读 Mini-SGLang 的 `MHAKVCache`、`CacheManager` 与 `RadixPrefixCache`。源码基于提交 `9a91cfa`。

## 一、为什么需要 KV Cache

自回归 Attention 在第 `t` 步需要当前 Query 与位置 `0...t` 的 Key、Value 做注意力。如果每轮都重新计算历史 K、V，生成 `n` 个 token 会重复执行大量投影计算。

KV Cache 将每层历史 K、V 保存下来：

```text
第 1 轮：计算 K0,V0，保存
第 2 轮：只计算 K1,V1，读取 [K0,K1]
第 3 轮：只计算 K2,V2，读取 [K0,K1,K2]
```

缓存大小大致为：

```text
2 × 层数 × token 数 × KV head 数 × head_dim × 元素字节数
```

其中 2 表示 Key 和 Value。长上下文、大 batch 和高精度都会快速放大显存消耗。

## 二、连续分配的问题

最直观的方案是为每个请求预留一块能容纳最大长度的连续缓存，但会产生两个问题。

一是内部浪费。用户设置 `max_tokens=4096`，实际可能只生成几十个 token，预留空间长期闲置。

二是外部碎片。请求长度和生命周期不同，释放后形成许多小洞，剩余总空间足够也未必能找到一块大连续区域。

Paged KV Cache 将缓存切成固定大小的物理页，请求通过页表把逻辑 token 位置映射到物理位置：

```text
请求逻辑页：  [0] [1] [2]
                |   |   |
物理 KV 页：   17   3  28
```

新增长度时只需再分配一页，不要求物理连续。

## 三、Mini-SGLang 的缓存分层

它将缓存系统拆成三层：

```text
MHAKVCache
  管理 GPU 上真正存放 K/V 的物理张量

CacheManager
  管理空闲页、请求页表、分配和回收

BasePrefixCache
  决定哪些 token 前缀可以跨请求复用
```

这三层分别回答：字节存在哪里、页属于谁、内容能否共享。

## 四、MHAKVCache 的物理布局

`kvcache/mha_pool.py` 中的 `MHAKVCache` 按层创建 K 和 V 张量。每个 TP rank 只保存本 rank 负责的 KV heads。

逻辑形态可理解为：

```text
Layer 0: K[num_tokens, local_kv_heads, head_dim]
         V[num_tokens, local_kv_heads, head_dim]
Layer 1: K[...]
         V[...]
...
```

实际布局会考虑 Attention backend 和 page size。Attention Layer 在模型前向中将新 K、V 写入 Scheduler 给出的 `out_loc`，无需知道这些位置属于哪个请求。

## 五、页表如何连接逻辑与物理位置

全局 `Context.page_table` 是二维 GPU Tensor：

```text
行：table_idx，也就是请求槽位
列：请求内的逻辑 token 位置
值：物理 KV Cache token index
```

虽然缓存分配以 page 为单位，这张表对 kernel 暴露的是 token 级索引。`scheduler/cache.py` 中的 `_write_page_table()` 将分配出的页展开后写入对应行。

例如 page size 为 4：

```text
逻辑页 0 -> 物理页 7 -> token index 28,29,30,31
逻辑页 1 -> 物理页 2 -> token index 8,9,10,11
```

Attention backend 根据 `table_idx` 和序列长度读取历史 K、V。

## 六、CacheManager 如何分配缓存

当一个请求进入 Prefill，CacheManager 先查询 Prefix Cache，得到最长匹配长度和缓存句柄。

若输入为：

```text
[System Prompt] [User Prompt]
```

而 `[System Prompt]` 已缓存，则：

```text
cached_len = system prompt 长度
extend_len = user prompt 长度
```

Scheduler 只为 `extend_len` 分配新页和执行 Prefill。

空闲页不足时，CacheManager 请求 Prefix Cache 驱逐一定数量的可回收 token，并把返回的物理索引重新加入空闲池。仍不足则本轮不能接纳该请求。

## 七、Naive Cache 的语义

`NaivePrefixCache` 不匹配跨请求前缀。每个请求从零开始分配，完成后缓存直接释放。

它的价值不只是提供简单模式，还能作为性能消融基线：

```bash
python -m minisgl --model ... --cache naive
```

对比 Naive 与 Radix 模式，可以判断工作负载究竟有多少共享前缀，以及管理前缀树的成本是否值得。

## 八、Radix Tree 为什么适合 token 前缀

Radix Tree 是压缩前缀树。普通 Trie 每条边只保存一个 token，Radix Tree 可以在边或节点中保存一段 token，因此深度更小。

假设有三个缓存序列：

```text
[1, 2, 3, 4]
[1, 2, 3, 8]
[1, 2, 9]
```

压缩后的结构类似：

```text
[1,2]
 ├─ [3]
 │   ├─ [4]
 │   └─ [8]
 └─ [9]
```

每个节点同时保存 token 片段和对应物理 KV index。查找输入时沿 token 前缀向下走，即可得到最长匹配缓存。

## 九、RadixPrefixCache 的关键对象

`RadixTreeNode` 保存：

- 压缩 token key；
- 对应 KV index；
- 父节点与子节点；
- 引用计数；
- 最近访问时间。

`RadixCacheHandle` 保存某个请求当前匹配到的节点和长度。Scheduler 不直接操作树节点，而是持有 handle。

`RadixPrefixCache` 维护两个容量：

```text
protected_size：正在被请求引用，不能驱逐
evictable_size：没有活跃引用，可以驱逐
```

这比简单的“已用/空闲”更准确，因为缓存页即使不属于活跃请求，也可能作为可复用前缀继续存在。

## 十、最长前缀匹配

`match_prefix()` 调用 `_tree_walk()`。遍历时通过当前页的 token key 找子节点，再比较该节点保存的 token 片段。

如果只匹配到节点的一部分，就调用 `split_at()`：

```text
原节点：[3,4,5,6]
输入仅匹配：[3,4]

拆分后：
[3,4]
  └─ [5,6]
```

匹配长度会向下对齐到 page size。原因是缓存分配和 Attention 读取的最小共享单位是完整页；不完整页不能安全地交给另一个请求复用。

## 十一、前缀如何插入

请求完成或缓存状态更新时，`insert_prefix(input_ids, indices)` 将已经计算的 token 与物理 KV index 插入树中。

流程是：

```text
将长度按 page size 向下对齐
 -> 查找已有最长前缀
 -> 未覆盖后缀创建新节点
 -> 保存后缀 token 和 KV index
 -> 返回新的 handle
```

如果另一个请求后来输入相同 system prompt，它会命中这些节点，跳过对应 Prefill。

## 十二、引用计数与驱逐

当活跃请求使用某个缓存节点时，`lock_handle()` 沿该节点向根递增引用计数。节点从 0 变为 1 时，其容量从 `evictable_size` 转入 `protected_size`。

请求结束或不再持有该前缀后，反向递减引用计数。变为 0 的节点重新可驱逐。

驱逐从未引用的叶子节点开始，并按时间戳组成最小堆，近似实现 LRU：

```text
收集 ref_count == 0 的叶子
 -> 按 timestamp 选择最旧节点
 -> 删除节点并回收其物理 KV index
 -> 若父节点也成为可驱逐叶子，再加入堆
```

只删除叶子可以避免破坏仍被更长前缀依赖的内部路径。

## 十三、Radix Cache 的收益边界

它最适合共享前缀明显的场景：

- 多轮对话共享历史；
- 大量请求共享 system prompt；
- few-shot 示例重复；
- agent 工作流中重复读取同一上下文。

如果所有 prompt 都随机且没有公共前缀，收益有限，还会增加树操作和元数据成本。因此 Mini-SGLang 保留 Naive Cache 作为可选策略。

下一篇进入 Engine，分析模型加载、Attention backend、CUDA Graph 和采样如何组成一次真正的 GPU forward。

## 参考源码

- `python/minisgl/kvcache/base.py`
- `python/minisgl/kvcache/mha_pool.py`
- `python/minisgl/kvcache/naive_cache.py`
- `python/minisgl/kvcache/radix_cache.py`
- `python/minisgl/scheduler/cache.py`
- `python/minisgl/core.py`
