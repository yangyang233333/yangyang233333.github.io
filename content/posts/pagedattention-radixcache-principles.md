---
title: "PagedAttention 与 RadixCache 原理：从分页 KV 管理到跨请求前缀复用"
date: 2026-09-04T21:00:00+08:00
draft: false
tags: ["PagedAttention", "RadixCache", "vLLM", "SGLang", "KV Cache", "大模型推理"]
categories: ["大模型推理"]
summary: "用操作系统虚拟内存和压缩前缀树两个类比，解释 PagedAttention 如何管理 KV 物理块、RadixCache 如何发现并复用公共前缀，并给出可运行的纯 Python 简化实现。"
---

大模型推理中的 KV Cache 有两个不同但经常被混在一起的问题：

1. **一条序列的 KV Cache 如何高效放进显存？**
2. **不同请求拥有相同前缀时，如何避免重复计算？**

PagedAttention 主要回答第一个问题：把 KV Cache 切成固定大小的物理块，通过 Block Table 建立逻辑位置到物理位置的映射，减少显存碎片并支持按需分配。

RadixCache 则回答第二个问题：把已经计算过的 token 前缀组织成压缩前缀树，在新请求到来时查找最长公共前缀，直接复用对应的 KV Block。

二者并非竞争关系，而是位于不同层次：

```text
请求 token 序列
      │
      │ RadixCache：哪些前缀已经计算过？
      ▼
可复用的逻辑 token / KV Block
      │
      │ PagedAttention：这些 Block 放在显存哪里？
      ▼
离散的物理 KV Block
```

本文先解释两者各自的原理，再用两个纯 Python 小程序演示核心数据结构。代码只模拟内存管理和索引，不执行真实 CUDA Attention Kernel。

# 一、为什么连续 KV Cache 容易浪费显存

自回归生成时，每处理一个 token，每层 Attention 都会产生 Key 和 Value。后续 token 需要读取全部历史 K/V，因此推理引擎必须保存它们。

如果系统按请求的最大长度预留一整段连续显存，会产生两种浪费。

## 1. 预留但没有使用

假设请求最多允许 4096 个 token，但最终只生成到 600 个 token。其余空间从未写入，却不能分给其他请求。

```text
连续预留的 KV 空间
┌───────────────┬──────────────────────────────────┐
│ 已使用 600    │ 未使用但已占用 3496              │
└───────────────┴──────────────────────────────────┘
```

## 2. 外部碎片

不同长度的请求不断进入和结束，会留下大小不一的空洞。总空闲空间可能足够，却找不到一整段满足新请求的连续区域。

这与操作系统直接为进程分配可变长连续物理内存的问题相似。PagedAttention 借用了虚拟内存分页的思路：**逻辑上连续，物理上可以离散**。

# 二、PagedAttention：用 Block Table 解耦逻辑与物理位置

PagedAttention 把每条序列的 KV Cache 切成固定大小的逻辑块，再从全局 Block Pool 中按需分配物理块。

假设每个 Block 保存 4 个 token，一条包含 10 个 token 的请求需要 3 个逻辑块：

```text
逻辑块 0: token 0  1  2  3
逻辑块 1: token 4  5  6  7
逻辑块 2: token 8  9
```

物理块不要求连续：

```text
Block Table
┌──────────┬──────────┐
│ 逻辑块   │ 物理块   │
├──────────┼──────────┤
│ 0        │ 7        │
│ 1        │ 2        │
│ 2        │ 11       │
└──────────┴──────────┘
```

Kernel 访问逻辑位置 `token_pos` 时，可以计算：

```text
logical_block = token_pos // block_size
block_offset  = token_pos % block_size
physical_block = block_table[logical_block]
```

再根据 Layer、K/V、KV Head、Head Dimension 和 Block Offset 计算真实地址。

因此，上层看到的仍是一条连续序列，底层只需要找到任意空闲物理块即可。

## 1. 按需增长

序列刚进入时，只分配容纳 Prompt 所需的 Block。生成过程中，当前 Block 写满后再申请一个新 Block，不需要提前为最大上下文长度预留显存。

## 2. Block 内碎片可控

固定大小 Block 不能消除所有浪费。每条活跃序列的最后一个 Block 可能没有填满，但浪费上限小于一个 Block：

```text
waste_per_sequence < block_size
```

Block 越小，内部碎片越少，但 Block Table 更大、调度和地址转换开销更高；Block 越大，元数据更少，但尾部浪费更多。这是典型的粒度权衡。

## 3. Copy-on-Write 共享

多个采样分支或 Beam 可以共享相同 Prompt 的物理 Block，并用引用计数管理：

```text
Prompt blocks: [3, 8, 12]
                  ▲
          ┌───────┴───────┐
       Branch A         Branch B
```

当某个分支需要修改共享 Block 时，再分配新块并复制，即 Copy-on-Write。这样无需为每个分支完整复制 Prompt KV。

# 三、简化代码：实现一个分页 KV Block Pool

下面的程序不存真实 Tensor，只用 token 字符串模拟 KV 内容。它展示三件事：

- 从全局空闲池分配离散物理 Block；
- 使用 Block Table 将逻辑 token 位置映射到物理位置；
- 序列结束后归还 Block。

```python
from collections import deque


class PagedKVPool:
    def __init__(self, num_blocks: int, block_size: int):
        self.block_size = block_size
        self.blocks = [[None] * block_size for _ in range(num_blocks)]
        self.free_blocks = deque(range(num_blocks))
        self.block_tables: dict[str, list[int]] = {}
        self.lengths: dict[str, int] = {}

    def _allocate_block(self) -> int:
        if not self.free_blocks:
            raise MemoryError("KV block pool is full")
        return self.free_blocks.popleft()

    def append(self, request_id: str, token: str) -> None:
        table = self.block_tables.setdefault(request_id, [])
        token_pos = self.lengths.get(request_id, 0)
        logical_block = token_pos // self.block_size
        block_offset = token_pos % self.block_size

        if logical_block == len(table):
            table.append(self._allocate_block())

        physical_block = table[logical_block]
        self.blocks[physical_block][block_offset] = token
        self.lengths[request_id] = token_pos + 1

    def read(self, request_id: str, token_pos: int) -> str:
        logical_block = token_pos // self.block_size
        block_offset = token_pos % self.block_size
        physical_block = self.block_tables[request_id][logical_block]
        return self.blocks[physical_block][block_offset]

    def free(self, request_id: str) -> None:
        for physical_block in self.block_tables.pop(request_id, []):
            self.blocks[physical_block] = [None] * self.block_size
            self.free_blocks.append(physical_block)
        self.lengths.pop(request_id, None)


pool = PagedKVPool(num_blocks=6, block_size=4)

for token in ["A", "B", "C", "D", "E"]:
    pool.append("request-1", token)

for token in ["X", "Y", "Z"]:
    pool.append("request-2", token)

print("request-1 block table:", pool.block_tables["request-1"])
print("request-2 block table:", pool.block_tables["request-2"])
print("request-1 token 4:", pool.read("request-1", 4))

pool.free("request-1")
print("free blocks:", list(pool.free_blocks))
```

可能输出：

```text
request-1 block table: [0, 1]
request-2 block table: [2]
request-1 token 4: E
free blocks: [3, 4, 5, 0, 1]
```

真实实现还需要保存 GPU Tensor、引用计数、哈希、LRU 状态、Swap 状态，以及为 CUDA Kernel 准备紧凑的 Block Table，但核心映射关系与这个例子一致。

# 四、PagedAttention 没有自动解决跨请求复用

PagedAttention 让物理 Block 可以离散分配，也允许多个逻辑序列引用同一 Block，但系统仍需回答：

> 新请求到达时，如何知道哪些历史 Block 与它的 token 前缀完全一致？

例如连续到达三个请求：

```text
System Prompt + “介绍一下 PagedAttention”
System Prompt + “介绍一下 RadixCache”
System Prompt + “介绍一下 Continuous Batching”
```

它们共享很长的 System Prompt。如果每次都重新执行 Prefill，即使 KV 内存管理很高效，计算仍然重复。

Prefix Cache 需要建立从 token 前缀到 KV Block 的索引。简单哈希表可以缓存固定长度 Chunk，但面对任意长度、重叠和分叉的前缀时，压缩前缀树更自然。这就是 RadixCache 的作用。

# 五、RadixCache：用压缩前缀树索引 KV Block

Radix Tree（基数树、压缩 Trie）把只有一个孩子的连续节点压缩成一条边。SGLang 的 RadixAttention 将 token 序列作为 Key，将对应 KV Cache 的位置作为 Value。

假设缓存了三条 token 序列：

```text
[1, 2, 3, 4, 5]
[1, 2, 3, 8]
[1, 2, 9]
```

压缩后的树可以表示为：

```text
root
└── [1, 2]
    ├── [3]
    │   ├── [4, 5]
    │   └── [8]
    └── [9]
```

每条边可以包含多个 token，而不是普通 Trie 的单 token 边，因此公共前缀链不会产生大量中间节点。

新请求 `[1, 2, 3, 7]` 到达时，树中最长匹配为 `[1, 2, 3]`：

```text
命中部分: [1, 2, 3]  → 直接复用已有 KV
未命中部分: [7]      → 执行新的 Prefill
```

## 1. 插入时为什么要 Split

如果树中已有边 `[1, 2, 3, 4, 5]`，现在插入 `[1, 2, 3, 8]`，两者只共享前三个 token。原节点必须分裂：

```text
分裂前：
root ── [1, 2, 3, 4, 5]

分裂后：
root ── [1, 2, 3]
               ├── [4, 5]
               └── [8]
```

分裂操作是 RadixCache 的核心：它把任意公共前缀显式变成可共享节点。

## 2. 节点保存什么

概念上，节点至少包含：

- `key`：该边对应的一段 token；
- `value`：这些 token 对应的 KV Block ID 或位置；
- `children`：下一段 token 到子节点的映射；
- `last_access_time`：用于 LRU 淘汰；
- `lock_ref`：表示正在被请求使用，不能淘汰。

真实系统中，Key 与 Value 的长度通常需要对齐：一段 token 对应一段可复用 KV 位置。

## 3. Leaf-first LRU 淘汰

显存不足时，不能随意删除内部节点。删除仍有子树的公共前缀会破坏多个缓存条目，因此 RadixCache 通常从未锁定的叶子开始，结合 LRU 选择最久未访问的叶节点。

```text
root
└── 公共 System Prompt
    ├── 最近使用的会话 A
    └── 很久未使用的会话 B  ← 优先从叶子回收
```

删除叶子后，如果父节点变成没有价值的空节点，再继续向上整理。正在被运行请求引用的节点需要锁定，防止 Scheduler 使用期间被回收。

# 六、简化代码：实现可分裂的 RadixCache

下面用 `value` 直接保存 token 对应的“KV Block ID”。程序实现最长前缀匹配和节点分裂，不包含生产系统中的 LRU、引用锁和并发控制。

```python
from dataclasses import dataclass, field


@dataclass
class RadixNode:
    key: tuple[int, ...] = ()
    value: tuple[int, ...] = ()
    children: dict[int, "RadixNode"] = field(default_factory=dict)


def common_prefix_length(left: tuple[int, ...], right: tuple[int, ...]) -> int:
    length = 0
    for left_token, right_token in zip(left, right):
        if left_token != right_token:
            break
        length += 1
    return length


class RadixCache:
    def __init__(self):
        self.root = RadixNode()

    def insert(self, tokens: list[int], block_ids: list[int]) -> None:
        node = self.root
        remaining_tokens = tuple(tokens)
        remaining_blocks = tuple(block_ids)

        while remaining_tokens:
            child = node.children.get(remaining_tokens[0])
            if child is None:
                node.children[remaining_tokens[0]] = RadixNode(
                    key=remaining_tokens,
                    value=remaining_blocks,
                )
                return

            prefix_len = common_prefix_length(child.key, remaining_tokens)

            if prefix_len == len(child.key):
                node = child
                remaining_tokens = remaining_tokens[prefix_len:]
                remaining_blocks = remaining_blocks[prefix_len:]
                continue

            shared = RadixNode(
                key=child.key[:prefix_len],
                value=child.value[:prefix_len],
            )
            node.children[shared.key[0]] = shared

            child.key = child.key[prefix_len:]
            child.value = child.value[prefix_len:]
            shared.children[child.key[0]] = child

            new_suffix = remaining_tokens[prefix_len:]
            if new_suffix:
                shared.children[new_suffix[0]] = RadixNode(
                    key=new_suffix,
                    value=remaining_blocks[prefix_len:],
                )
            return

    def match(self, tokens: list[int]) -> tuple[int, list[int]]:
        node = self.root
        remaining = tuple(tokens)
        matched_blocks: list[int] = []
        matched_tokens = 0

        while remaining:
            child = node.children.get(remaining[0])
            if child is None:
                break

            prefix_len = common_prefix_length(child.key, remaining)
            matched_blocks.extend(child.value[:prefix_len])
            matched_tokens += prefix_len

            if prefix_len < len(child.key):
                break

            node = child
            remaining = remaining[prefix_len:]

        return matched_tokens, matched_blocks


cache = RadixCache()
cache.insert([1, 2, 3, 4, 5], [10, 11, 12, 13, 14])
cache.insert([1, 2, 3, 8], [10, 11, 12, 20])
cache.insert([1, 2, 9], [10, 11, 30])

matched, blocks = cache.match([1, 2, 3, 7])
print("matched tokens:", matched)
print("reused KV locations:", blocks)
print("tokens to prefill:", [1, 2, 3, 7][matched:])
```

输出为：

```text
matched tokens: 3
reused KV locations: [10, 11, 12]
tokens to prefill: [7]
```

这里为了便于理解，让一个 token 对应一个虚拟 Block ID。真实推理引擎通常按多个 token 一个 Block 管理 KV，还要处理只有完整 Block 才能稳定共享、模型与 LoRA 配置隔离、不同 KV Layout、Hash 校验等约束。

# 七、两者如何协同工作

一次带 Prefix Cache 的请求可以概括为：

```text
1. Tokenize 请求
        │
2. RadixCache 查找最长公共前缀
        │
        ├── 命中 token：获得可复用 KV Block
        └── 未命中 token：需要执行 Prefill
        │
3. Paged KV Manager 为未命中部分分配物理 Block
        │
4. Attention Kernel 读取 Block Table
        │
5. Prefill 完成后，把新 token 与 Block 插入 RadixCache
```

举例来说，新请求有 1000 个 Prompt Token，RadixCache 命中前 800 个：

- 前 800 个 token 的 KV Block 增加引用或锁定；
- Paged KV Manager 只为剩余 200 个 token 分配新 Block；
- 模型只对未命中后缀执行必要计算；
- 完成后，新增长路径被插入 Radix Tree，供后续请求复用。

因此：

- **PagedAttention 提高容量利用率**，让更多 KV 能留在 GPU；
- **RadixCache 提高计算复用率**，让命中的 KV 不必重新生成；
- 前者提供物理存储基础，后者提供按 token 前缀发现共享机会的索引。

# 八、容易混淆的边界

## 1. PagedAttention 不等于 Prefix Cache

分页管理本身只说明 KV 可以离散放置。要跨请求复用，还必须有前缀 Key、匹配算法、引用管理和淘汰策略。

## 2. RadixCache 不负责执行 Attention

RadixCache 找到“哪些 token 已命中”和“对应 KV 在哪里”；真正读取分页 K/V、计算 Attention 的仍是推理 Kernel。

## 3. 命中长度不一定等于可直接复用长度

系统可能要求按完整 KV Block 对齐，最后一个不完整 Block 可能需要重新计算或特殊处理。此外，模型、Tokenizer、LoRA Adapter、位置编码和部分采样上下文不同，都可能让表面相同的文本不能安全共享。

## 4. Prefix Cache 用显存换计算

保留已结束请求的 KV，可以加速未来相似请求，却占用原本可以服务新请求的显存。LRU、命中率和工作负载局部性决定这笔交易是否值得。

# 九、适合哪些工作负载

RadixCache 在以下场景收益明显：

- 大量请求共享同一段长 System Prompt；
- Few-shot 示例固定，用户问题不同；
- 多轮对话不断扩展同一历史；
- Agent 的工具描述和规则重复出现；
- Tree-of-Thought、并行采样等工作流具有分叉前缀。

如果每个请求的 Prompt 完全不同，RadixCache 命中率很低，主要价值仍来自 PagedAttention 的显存管理。

# 总结

理解这两个机制，可以分别记住一句话：

> PagedAttention 像操作系统分页：用 Block Table 让逻辑连续的 KV 分散存放，并按需分配物理块。

> RadixCache 像压缩 Trie：用最长前缀匹配找到可复用 KV，并通过分裂、锁定和叶子优先 LRU 管理共享缓存。

PagedAttention 解决“怎么放”，RadixCache 解决“怎么找和复用”。把两者结合，推理引擎既能减少显存碎片，也能跳过重复 Prefix Prefill，这正是现代高吞吐 LLM Serving 的重要基础。

参考资料：

- [Efficient Memory Management for Large Language Model Serving with PagedAttention](https://arxiv.org/abs/2309.06180)
- [vLLM Paged Attention Design](https://docs.vllm.ai/en/latest/design/paged_attention/)
- [SGLang: Efficient Execution of Structured Language Model Programs](https://arxiv.org/abs/2312.07104)
- [SGLang Prefix Caching](https://docs.sglang.ai/advanced_features/radix_attention.html)
