---
title: "LMCache 三种 L1MemoryManager 源码解析：DRAM、Device-DAX 与 GDS"
date: 2026-09-10T22:30:00+08:00
draft: false
tags: ["LMCache", "内存管理", "Device-DAX", "GDS", "KV Cache", "源码解析"]
categories: ["技术"]
aliases:
  - /posts/lmcache-gds-l1-memory-manager/
  - /posts/lmcache-devdax-l1-memory-manager/
  - /posts/lmcache-l1-memory-manager/
---

LMCache 的 `L1MemoryManager`、`DevDaxL1MemoryManager` 和 `GDSL1MemoryManager`，并不是同一种分配器换了三个名字。它们分别管理 **DRAM 内存池、Device-DAX 映射池和 GDS slab 偏移空间**。理解三者的关键，是把对象生命周期、空间分配和数据搬运分开。

本文基于 2026 年 9 月 10 日核对的公开 fork `yangyang233333/LMCache`，固定提交 `7d9d54dd9b23703a73040e140753c95cd6c4bdb7`。以下结论限定于这个实现快照，不代表上游发布版已经包含全部功能；本文为源码调研，没有进行真实 GDS、DAX/CXL 硬件性能实测。

## 1. 同一个 L1 层级，三种数据载体

先分清两个名字：`L1Manager` 管理 key、对象状态与读写生命周期，本文讨论的三个 memory manager 则负责空间分配与回收。淘汰策略和数据传输也不应与这层空间接口混为一谈。

`L1Manager` 的构造逻辑先检查 GDS 配置，再检查 DAX 路径，最后选择普通 DRAM 管理器。三者通过结构化接口 `L1ManagerProtocol` 暴露分配、释放、容量、介质识别、内存描述符和关闭等操作。

```text
L1Manager：key、对象状态与读写生命周期
    │
    └── L1ManagerProtocol
          ├── L1MemoryManager
          │     └── LazyMemoryAllocator / MixedMemoryAllocator
          ├── DevDaxL1MemoryManager
          │     └── DevDaxMemoryAllocator
          │           ├── 可选 DRAM pool
          │           └── 一个或多个 DAX arena
          └── GDSL1MemoryManager
                └── AddressManager：slab 偏移与空闲区间

gpu_ops / connector：实际数据传输
    └── GDS 对象分派到 GDSContext
```

`DevDaxL1MemoryManager` 继承普通 `L1MemoryManager`，复用接口逻辑并替换 allocator；`GDSL1MemoryManager` 则是满足同一 Protocol 的独立实现，不继承普通管理器。

| 维度 | L1MemoryManager | DevDaxL1MemoryManager | GDSL1MemoryManager |
| --- | --- | --- | --- |
| 数据载体 | CPU DRAM | DAX 映射，可加 DRAM | 文件或设备上的 GDS slab |
| 对象模型 | host Tensor | 映射区域上的 host Tensor | 偏移与大小占位对象 |
| CPU 数据指针 | 有 | 有 | 不支持 |
| 主要空间管理 | Lazy / Mixed allocator | 每个 arena 独立分配 | slab 字节区间分配 |
| 内存描述符 | 主 CPU buffer | 主 buffer，不覆盖所有 arena | `None` |
| 容量变化 | Lazy 阶段增长 | 可增加设备、drain 退场 | 管理器地址空间固定 |

这里的 L1 是逻辑缓存层级，不是 DRAM 的别名。GDS 模式直接替换 L1 数据载体，并非简单地在 DRAM 后面追加一个 SSD L2。

## 2. L1MemoryManager：薄适配层与两类 DRAM 分配器

### 2.1 管理器自身做了什么

`allocate(layout_desc, count)` 将 shapes、dtypes 和数量交给底层 `batched_allocate()`。返回 `None` 时转换成 `(L1Error.OUT_OF_MEMORY, [])`，成功则返回对象列表。`free()` 委托 `batched_free()`，正常结束后返回 `SUCCESS`。

它没有捕获所有异常并统一转为错误码，也没有用自己的统一锁包住这些接口。线程安全主要来自 allocator 和地址管理器；对象能否释放，则需要调用者遵守外围生命周期，不能在读者或传输仍持有区域时回收。

`create_memory_allocator()` 按 `use_lazy` 选择两条路径：lazy 使用 `LazyMemoryAllocator`，非 lazy 使用 `MixedMemoryAllocator`。

Mixed 在这里不是“DRAM+DAX 混合”。它组合预分配内存中的 Tensor 分配器与普通字节缓冲区分配器。非 lazy 路径还支持命名 POSIX SHM；创建前会规范化名称，并尝试清理带 `lmcache_l1_pool_` 前缀的同名旧段。

由此推导，手动给多个实例配置同一个 SHM 名称时，必须另外保证所有权和生命周期。清理旧段不等于进程间协调机制。

### 2.2 Lazy 扩的是可分配范围

`LazyMemoryAllocator` 先取得最终大小的底层 CPU buffer，再对初始部分执行 pin 尝试，以该范围建立 allocator 地址空间，最后启动后台线程逐步扩展。非 NUMA 分支会额外申请对齐余量，切出基地址符合要求的 Tensor 视图。

所以这里的 lazy 不是业务请求到来时逐对象 malloc，而是将大内存池的注册和可分配范围开放拆到后台。底层 Tensor 大小、进程 RSS、pin 状态和 allocator 已开放容量是不同指标。

该快照默认初始配置为 20 GiB，配置层先限制其不超过最终配置大小。allocator 内部再按 64 MiB 对齐初始和最终大小，后台按 64 MiB 执行 pin，通常累计 1 GiB 后向地址管理器提交扩展，末尾提交剩余部分。

这里的 `AddressManager.sbrk()` 扩展的是分配器内部地址空间，不是调用操作系统的 `sbrk`。尚未提交的范围无法分配，因此启动初期的 OOM 未必意味着最终配置容量已经耗尽。

还要注意实现与理想语义的差异：平台不支持 pin 时，配置层会自动关闭 lazy；但运行中某次 pin 失败只记录警告，后台仍继续推进容量。不能把“已经开放的空间”全部当成“成功注册的 pinned memory”。

### 2.3 容量和描述符并不等价

普通管理器优先调用 allocator 自身的 `get_memory_usage()`；没有该接口时，从 Mixed 的 pin allocator 或 Lazy 的当前地址管理器取数，计算 `used = heap_size - free_size`。

Lazy 的 total 因而随扩展增长，内部粒度对齐还可能让最终 heap 与未经对齐的配置字节数略有差异。used 含分配对齐开销，不是业务有效 KV 字节，更不是进程 RSS；Mixed 的回退统计也不包括进程内所有普通字节缓冲区与 Python 元数据。

`get_l1_memory_desc()` 则返回底层主 buffer 的指针、配置容量和对齐。Lazy 的描述符指向预先存在的最终 buffer，并不表示整个范围已经完成 pin 或具备所有传输路径要求的注册条件。源码仍保留了扩展完成前 RDMA 注册需要验证的 TODO。

## 3. DevDaxL1MemoryManager：保留 Tensor 模型，替换内存来源

### 3.1 纯 DAX 与 DRAM 溢出模式

这个子类不调用父类构造函数，而是创建 `DevDaxMemoryAllocator`，复用分配、释放、统计和关闭接口，并增加设备管理能力。

容量字段有一个容易误读的约定：

| DAX 配置 | DRAM 容量 | 初始 DAX 容量 |
| --- | --- | --- |
| 有路径，`devdax_size_in_bytes = 0` | 0 | `size_in_bytes` |
| 有路径，`devdax_size_in_bytes > 0` | `size_in_bytes` | `devdax_size_in_bytes` |

DAX size 为零并不是禁用 DAX，而是纯 DAX 模式。下面的配置对象表示 8 GiB DRAM 加 32 GiB DAX；创建管理器时才会真正打开和映射设备，不能跳过设备准备直接运行。

```python
from lmcache.v1.distributed.config import L1MemoryManagerConfig

config = L1MemoryManagerConfig(
    size_in_bytes=8 << 30,
    use_lazy=False,
    shm_name="",
    devdax_path="/dev/dax0.0",
    devdax_size_in_bytes=32 << 30,
)
```

配置校验要求 DAX 关闭 lazy 和 SHM，CLI 对应 `--no-l1-use-lazy` 与 `--shm-name ""`。存储配置归一化还可能从匹配的 DAX L2 adapter 补齐混合池容量，因此应查看最终配置，而不只看原始参数。

### 3.2 mmap 如何变成可分配对象

分配器打开设备并建立共享、可读写 mmap，用 `ctypes` 暴露映射，再通过 `torch.frombuffer(..., dtype=torch.uint8)` 创建 CPU Tensor。每个 arena 配有独立的 `TensorMemoryAllocator`。

这个 Tensor 是映射视图，不是把整个设备内容复制到 DRAM。KV 对象从映射区域切出自己的空间，仍具有 CPU 指针和 Tensor 视图。Device-DAX 在此是一种映射接口，不能仅凭名字认定硬件必为 CXL、有掉电持久性，或已支持 KV 重启恢复。

GPU 传输能力还取决于映射注册。平台支持时，分配器尝试 `pin_memory`；失败会警告并退回 pageable host copies，而不是直接拒绝初始化。因此“对象位于 DAX”与“对象走高效 GPU DMA”必须分别验证。

### 3.3 DRAM 优先是放置策略，不是冷热迁移

批量分配先尝试 DRAM 可容纳的部分，再按 arena 顺序从 ACTIVE 的 DAX 池补齐。一个批次可以横跨 DRAM 和多个 DAX arena，但单个对象仍需在某个分配器中取得足够的连续空间。

若无法补齐，已分配的 DAX 对象和本批次 DRAM 对象都会回滚。DAX 池的变更由 `host_mem_lock` 串行化，DRAM 使用自身同步；失败回滚不等于整个混合操作由一把事务锁隔离。

`get_backend_type(memory_obj)` 按对象实际位置返回 `DRAM` 或 `DEVDAX`，不能统一标成 DAX。代码展示的是新对象优先放 DRAM，没有因为 DAX 对象变热就自动将其迁回 DRAM 的逻辑。

### 3.4 动态退场必须等待指针安全

`add_device()` 映射新 arena，增加后续分配容量，不移动已有对象。移除只支持 DRAIN：停止该 arena 的新分配，等待对象释放完，再安全关闭映射。

```text
ACTIVE
   │ remove_device(DRAIN)
   ▼
DRAINING：不再接收新对象，已有对象继续存活
   │ 无存活分配 + 设备传输安全结束 + 外部视图释放
   ▼
REMOVED
```

纯 DAX 的初始 arena 是 primary，承担主内存描述符，禁止移除；混合模式主 buffer 是 DRAM，所有 DAX arena 都是可移除的 overflow。

分配计数归零仍不够。实现会在解除 pin 和 unmap 前同步设备，防止排队中的 GPU 传输访问已失效指针；外部 Tensor 视图可能导致 `BufferError`，使 arena 保持 DRAINING，等待后续重试。

因此 drain 不是强制拔盘或即时迁移。引用未释放可能令退场长期等待，设备同步也可能带来延迟，运维不能把 remove 请求返回当成设备已可拆除。

## 4. GDSL1MemoryManager：管理偏移，不管理 host buffer

### 4.1 allocate 只预留 slab 空间

构造函数只创建 `AddressManager(size_in_bytes, align_bytes)`，并不打开文件、创建 CPU Tensor 或执行 I/O。

批量分配先根据全部 shapes/dtypes 计算 chunk 字节数，再逐个申请空间，得到偏移和对齐大小，最后构造 `GDSMemoryObject`。某次申请抛出 `RuntimeError` 时，管理器释放本次已经预留的区间，返回 `OUT_OF_MEMORY` 和空列表。

默认按 4096 字节对齐，所以一个逻辑上 5000 字节的 chunk 占用 8192 字节。底层 `AddressManager` 在按地址排序的空闲区间中寻找第一段足够大的空间，释放时合并相邻区间；总空闲量足够也可能因碎片化而分配失败。

下面只演示地址空间回滚，不会创建 slab 或发起 DMA，但仍需安装项目导入依赖：

```python
import torch
from lmcache.v1.distributed.api import MemoryLayoutDesc
from lmcache.v1.distributed.config import GdsL1Config
from lmcache.v1.distributed.error import L1Error
from lmcache.v1.distributed.memory_manager import GDSL1MemoryManager

manager = GDSL1MemoryManager(
    GdsL1Config(file_location="/unused", size_in_bytes=8192)
)
layout = MemoryLayoutDesc(
    shapes=[torch.Size([4096])], dtypes=[torch.uint8]
)
error, objects = manager.allocate(layout, 3)
assert error == L1Error.OUT_OF_MEMORY
assert objects == []
assert manager.get_memory_usage() == (0, 8192)
```

这种全成或全败是本次申请的回滚契约，不表示整个循环对并发调用原子可见。底层地址管理器对单次操作加锁，循环之间仍可能交错。

### 4.2 MemoryObj 不一定能当 Tensor 用

| GDSMemoryObject 接口 | 实际含义 |
| --- | --- |
| `metadata.address` / `slab_offset` | slab 偏移，不是 CPU 指针 |
| `get_physical_size()` | 对齐后的占用 |
| `get_size()` | 此实现也返回 `phy_size`，非原始逻辑长度 |
| `tensor` | `None` |
| `data_ptr` / `byte_array` | 不支持，抛出异常 |
| `get_shapes()` / `get_dtypes()` | 不支持多组布局查询 |

分配大小使用全部布局计算，但元数据只记录第一组 shape/dtype，不能据此认为任意多组 Tensor 操作都可用。引用计数接口也不能照搬普通 Tensor 对象路径；适配 connector 时，应确认真实需要的是偏移和长度，还是 host pointer 与完整布局。

### 4.3 数据搬运与资源关闭属于 GDSContext

`gpu_ops` 发现 `GDSMemoryObject` 后，将读写分派到 `GDSContext.transfer_async()`：

```text
写入：GPU buffer → gpu_ops → GDSContext → slab[offset, size]
读取：slab[offset, size] → GDSContext → GPU buffer
```

这只是 GPU buffer 与 slab 的链路，不代表模型原始 KV 布局一定省掉了打包、staging 或 scatter/gather。空间分配成功也不等于数据传输完成、对象已经可读。

该快照支持 `auto`、`cufile`、`hipfile`、`ugds` 和 `phx` 后端：cuFile/hipFile 使用文件 slab，uGDS 使用专用字符设备，Phoenix 使用文件系统 slab。GDS 名称本身不保证一定命中 DMA 快路径；后端、挂载、拓扑和运行环境仍决定实际行为。

打开与注册存储、GPU buffer 注册、传输提交和关闭资源均由 `GDSContext` 管理。因此 memory manager 的 `close()` 是空操作，不代表一次 close 调用已经等待所有 GPU I/O 完成。

它也不是持久化 KV 数据库：文件后端初始化会创建、截断并预分配 slab，key 索引与空间分配状态没有持久化。原始设备不能照搬“截断文件”的描述，但字节保留也不等于重启后能恢复缓存。

## 5. 放在一起看，最容易出错的是两个公共接口

### 5.1 get_memory_usage 的分母并不总固定

| 模式 | used | total |
| --- | --- | --- |
| 普通 Mixed DRAM 回退统计 | Tensor/pin 池分配量 | 该池 heap 大小 |
| Lazy DRAM | 当前已分配空间 | 随后台扩展开放的 heap |
| Device-DAX | DRAM 与所有映射 arena 的已用量 | DRAM 加 ACTIVE arena 容量 |
| GDS | slab 已预留的对齐空间 | 管理器 slab 地址空间大小 |

最反直觉的是 DAX drain：used 仍计入待退场数据，total 却排除 DRAINING arena。假设 DRAM 容量 4 GiB、已用 3 GiB，一个已用 6 GiB 的 DAX arena 进入 drain，结果可能是 used=9 GiB、total=4 GiB。

由此推导，超过 100% 不一定是统计错误，而是分子、分母描述的集合不同。监控应分别展示配置容量、当前可分配容量、待退场数据和 arena 状态，不能用一个百分比覆盖所有模式。

### 5.2 有内存描述符，也未必能注册整个 L1

GDS 的 `get_l1_memory_desc()` 返回 `None`。DAX 返回主 buffer，但一个池可能还包括其他 arena；该快照的 `l1_exposes_single_memory_region()` 对所有 DAX L1 都返回 false，不只混合模式。普通 Lazy 则还存在地址已预留、pin/注册尚未全部就绪的阶段。

因此，消费者不能仅凭“有指针和长度”就认定整个 L1 是可注册的单区域。兼容性必须结合完整拓扑和具体传输通道检查。

GDS 类注释要求关闭 L2 adapters，混合 DAX 的配置校验也拒绝部分要求单区域注册的 L2 adapter。但这些并不意味着本文已经证明每个入口都执行了完整校验，更不应泛化为所有类型的 L2 都天然兼容或天然不兼容。

## 6. 如何验证实现，而不是从类名推断性能

三组现有测试验证的范围不同：

- `test_l1_memory_manager.py` 检查分配释放、并发调用和描述符；没有可用设备 runtime 时会跳过，跳过不等于验证通过。
- `test_devdax_l1_allocator.py` 使用普通可 mmap 文件，覆盖混合分配、跨 arena 回滚、介质识别、动态增删、外部视图和 unmap 前同步，不证明真实 DAX/GPU 注册成功。
- `test_gds_l1_memory_manager.py` 检查偏移不重叠、OOM 回滚、对齐、容量和 `memcheck`，不测试真实存储 DMA。

实际部署应依次确认空间契约、数据读回一致性、注册与快路径状态，再看业务收益。值得观测的指标包括 Lazy 启动扩展、pin 警告、对齐放大、DAX 溢出后的延迟、drain 尾延迟、GDS 提交与完成延迟，以及最终命中场景 TTFT。本文没有执行这些硬件实验，不给出性能倍数结论。

总结起来：**DRAM 管理器适配普通内存分配，DAX 管理器扩展可寻址内存来源，GDS 管理器管理存储偏移。** 三者共享空间接口，却不共享指针、容量、注册就绪和资源关闭的全部语义；真正的集成风险就藏在这些差异中。

## 源码索引

以下链接均固定到本文分析的提交，避免后续代码变化影响对照。

- [L1Manager 与后端选择](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/distributed/l1_manager.py)
- [L1ManagerProtocol](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/distributed/memory_manager/l1_manager_protocol.py)
- [L1MemoryManager](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/distributed/memory_manager/l1_memory_manager.py)
- [LazyMemoryAllocator](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/memory_allocators/lazy_memory_allocator.py)
- [MixedMemoryAllocator](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/memory_allocators/mixed_memory_allocator.py)
- [DevDaxL1MemoryManager](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/distributed/memory_manager/devdax_l1_memory_manager.py)
- [DevDaxMemoryAllocator](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/memory_allocators/devdax_memory_allocator.py)
- [GDSL1MemoryManager](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/distributed/memory_manager/gds_l1_memory_manager.py)
- [GDSMemoryObject 与 AddressManager](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/memory_management.py)
- [GPU 数据路径分派](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/gpu_connector/gpu_ops.py)
- [GDSContext](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/gpu_connector/gds_context.py)
- [配置归一化与兼容性](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/lmcache/v1/distributed/config.py)
- [普通管理器测试](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/tests/v1/distributed/test_l1_memory_manager.py)
- [DAX 分配器测试](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/tests/v1/distributed/test_devdax_l1_allocator.py)
- [GDS 管理器测试](https://github.com/yangyang233333/LMCache/blob/7d9d54dd9b23703a73040e140753c95cd6c4bdb7/tests/v1/distributed/memory_manager/test_gds_l1_memory_manager.py)
