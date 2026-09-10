---
title: "LMCache L1MemoryManager 源码解析：DRAM 分配、Lazy 扩容与容量语义"
date: 2026-09-10T22:30:00+08:00
draft: false
tags: ["LMCache", "内存管理", "KV Cache", "源码解析"]
categories: ["技术"]
---

`L1MemoryManager` 不是 LMCache 的全部一级缓存，也不负责决定哪些 key 应该淘汰。它是一层很薄的 **DRAM 分配器适配器**：接收布局和数量，返回 `MemoryObj`，统一错误码，并提供容量与主缓冲区信息。

本文基于 2026 年 9 月 10 日核对的公开 fork `yangyang233333/LMCache`，固定提交 `7d9d54dd9b23703a73040e140753c95cd6c4bdb7`。结论限定于这个源码快照，不将 fork 的演进当作上游发布版保证；没有进行性能实测。[管理器源码](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/distributed/memory_manager/l1_memory_manager.py)

## 1. 先把三个相似名字拆开

| 组件 | 核心责任 | 不应混为一谈的事情 |
| --- | --- | --- |
| `L1Manager` | key 到对象状态的管理、读写生命周期 | 不是只管理一块裸内存 |
| `L1MemoryManager` | 分配、释放、容量与内存描述符 | 不执行缓存替换策略 |
| 底层 allocator | Tensor/字节缓冲区、地址切分、pin 与释放 | 不理解请求命中和 key 语义 |

`L1Manager` 通过 `L1ManagerProtocol` 使用底层管理器。普通 CPU 模式选 `L1MemoryManager`，DAX 模式选其子类 `DevDaxL1MemoryManager`，GDS 模式选独立实现 `GDSL1MemoryManager`。

这个设计将“对象应该何时存在”与“对象字节放在哪里”分开。扩展存储介质时，应先判断是否还能复用 host Tensor 模型，而不是把所有分支塞进普通 DRAM 管理器。

## 2. allocator 的选择由 use_lazy 决定

`create_memory_allocator()` 有两条主要路径：

```text
use_lazy = true
    └── LazyMemoryAllocator(init_size, final_size, alignment)

use_lazy = false
    └── MixedMemoryAllocator(size, alignment, optional SHM)
```

这里的 Mixed 不表示 DRAM 与 DAX 混合。`MixedMemoryAllocator` 组合的是预分配内存中的 Tensor 分配器和普通字节缓冲区分配器；DRAM+DAX 混合属于另一类 allocator。

非 lazy 路径可以使用命名 POSIX SHM，默认名称来自 `lmcache_l1_pool_` 前缀和进程 ID。创建前会规范化名称，并尝试清理同名旧段；清理函数只处理指定前缀，拒绝不合法的嵌套路径名称。

“支持共享内存”不意味着多个独立管理器可以安全争用同一个名字。由清理逻辑可推导，手工配置共享名称时必须保证实例所有权和生命周期，不能把 unlink 旧段当成进程间协调协议。

## 3. Lazy 扩的是可分配区，不是按请求 malloc

`LazyMemoryAllocator` 在启动时先取得最终大小的底层 CPU buffer。非 NUMA 分支还多申请对齐余量，再切出基地址满足对齐的视图。之后只对初始部分执行 pin 尝试，以该初始范围建立地址空间，启动后台线程逐步扩展。

因此，这里的 lazy 主要是把大内存池的注册和地址空间开放拆到后台，而不是等业务请求来了才逐对象向操作系统申请内存。底层 Tensor 的大小、RSS、pin 状态和 allocator 已开放容量不是同一个指标。

这一快照的几个关键粒度是：

- 默认初始配置为 20 GiB，配置层会先将其限制到不超过最终配置大小。
- allocator 内部将初始和最终大小向 64 MiB 对齐。
- 后台按 64 MiB 调用 pin，通常每累计 1 GiB 向地址管理器提交扩展，末尾提交剩余部分。
- `AddressManager.sbrk()` 扩展的是分配器管理的地址区间，不是此处在调用操作系统的 `sbrk` 系统调用。

尚未开放的空间不能被分配。因此服务启动早期返回 OOM，并不必然表示已经达到最终配置上限，也可能是后台扩展还没有跟上。

还有一个容易被注释掩盖的细节：平台完全不支持 pin 时，配置层会自动关闭 lazy；但运行中的某次 pin 失败，`_pin_memory_chunk()` 只记录警告，扩展线程仍会继续推进容量。不能把“已经提交到 allocator”的字节一律解释为“成功注册为 pinned memory”的字节。

## 4. 分配接口只做薄适配

`allocate(layout_desc, count)` 把 shapes、dtypes 和数量传给 `batched_allocate()`。底层返回 `None` 时，它转换为 `(L1Error.OUT_OF_MEMORY, [])`；否则返回 `(L1Error.SUCCESS, objects)`。

`free(mem_objs)` 调用 `batched_free()`，正常返回时给出 `SUCCESS`。这里没有一个捕获所有异常并统一转换错误码的框架，因此不能以为所有错误都只通过 `L1Error` 表达。

用于理解调用契约的配置与使用示例如下。它会实际分配 CPU 内存，但没有写入或读取业务 KV，也不是性能测试：

```python
import torch
from lmcache.v1.distributed.api import MemoryLayoutDesc
from lmcache.v1.distributed.config import L1MemoryManagerConfig
from lmcache.v1.distributed.error import L1Error
from lmcache.v1.distributed.memory_manager import L1MemoryManager

manager = L1MemoryManager(L1MemoryManagerConfig(
    size_in_bytes=64 << 20, use_lazy=False, shm_name=""
))
layout = MemoryLayoutDesc(
    shapes=[torch.Size([4096])], dtypes=[torch.uint8]
)
objects = []
try:
    error, objects = manager.allocate(layout, 2)
    assert error == L1Error.SUCCESS
finally:
    manager.free(objects)
    manager.close()
```

该管理器自身没有为这些接口建立一把统一锁，线程安全主要依赖底层 allocator 和地址管理器。它也不依据 key 或引用状态决定对象是否可释放；调用者必须遵循外围 L1 生命周期，避免提前释放仍有读者或传输使用的区域。

## 5. 三种“容量”不要画成同一条曲线

`get_memory_usage()` 优先使用 allocator 自己的同名接口；没有时，普通 Mixed 路径从 pin allocator 取得地址管理器，Lazy 路径取得当前开放范围的地址管理器，然后计算 `used = heap_size - free_size`。

| 观测量 | 含义 | 容易误用的解释 |
| --- | --- | --- |
| 配置 `size_in_bytes` | 计划的 L1 容量 | 认为启动后立刻全部可分配 |
| `get_memory_usage()` 的 total | 当前 allocator 可管理的 heap 大小 | 认为永远等于原始配置 |
| `get_memory_usage()` 的 used | heap 中已分配、包含对齐开销的字节 | 认为是业务有效 KV 字节或进程 RSS |

Lazy 路径的 total 随后台扩展增长；内部粒度对齐还可能使最终 heap 大小与未经对齐的配置字节数略有差异。Mixed 的回退统计读取的是 Tensor/pin 池，也不代表进程里所有字节缓冲区和 Python 元数据的总占用。

由此可知，used/total 适合描述当前池的分配压力，却不能单独说明整台机器的内存压力。若用它触发淘汰，还需要知道当前是否处于初始化扩展阶段，并与稳定的配置容量分开展示。

## 6. get_l1_memory_desc 是注册契约，不是容量查询的替代品

`get_l1_memory_desc()` 返回底层 buffer 的 `data_ptr()`、配置容量和对齐值。Mixed 取自己的 buffer，Lazy 取最终底层 buffer，其他未支持的 allocator 类型会抛出 `NotImplementedError`。

这里存在刻意不同的语义：Lazy 的描述符大小来自配置，而 `get_memory_usage()` 的 total 来自当前 heap。描述符能覆盖一个预先存在的地址范围，不等于其中全部字节已经完成 pin 或可以用于所有注册路径。源码也保留了 lazy 扩展完成前 RDMA 注册需要验证的 TODO。

所以，消费者不能用“有指针、大小足够”替代注册就绪检查；更不能把描述符拿去直接证明设备传输性能。需要把地址范围、pin 状态、实际注册结果和 allocator 可用范围分别验证。

## 7. 与另两种管理器对照后，边界就清楚了

普通 `L1MemoryManager` 返回 `DRAM` 介质类型，关闭时委托 allocator 释放资源。DAX 子类保留 Tensor 分配接口，但可能有多个映射区域及动态退场；GDS 独立实现则连 host Tensor 都没有，只保存 slab 偏移。

三者共享的应是空间管理契约，而不是“每个对象都有 CPU 指针”“total 永远固定”“close 总能直接释放”等额外假设。

现有 `test_l1_memory_manager.py` 验证分配释放、并发调用和内存描述符；该测试模块在没有可用设备 runtime 时会跳过。测试被跳过不是功能验证通过。本文没有运行硬件测试，建议实际评估时同时观察启动耗时、扩展进度、pin 警告、OOM、对齐开销和最终业务 TTFT。

继续阅读：[GDSL1MemoryManager](https://yangyang233333.github.io/posts/lmcache-gds-l1-memory-manager/) 解释没有 host buffer 的 L1；[DevDaxL1MemoryManager](https://yangyang233333.github.io/posts/lmcache-devdax-l1-memory-manager/) 解释多介质和动态设备池。

## 源码索引

- [管理器与 allocator 选择](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/distributed/memory_manager/l1_memory_manager.py)
- [LazyMemoryAllocator](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/memory_allocators/lazy_memory_allocator.py)
- [MixedMemoryAllocator](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/memory_allocators/mixed_memory_allocator.py)
- [配置与容量语义](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/distributed/config.py)
- [L1ManagerProtocol](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/distributed/memory_manager/l1_manager_protocol.py)
- [管理器测试](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/tests/v1/distributed/test_l1_memory_manager.py)
