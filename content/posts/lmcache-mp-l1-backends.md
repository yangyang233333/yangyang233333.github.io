---
title: "LMCache MP 模式的 L1 存储：CPU 内存、GDS 与 Device-DAX 读写链路解析"
date: 2026-09-09T23:47:00+08:00
draft: false
tags: ["LMCache", "KV Cache", "GDS", "Device-DAX", "存储"]
categories: ["大模型推理"]
summary: "从对象模型与 GPU 数据传输出发，解析 LMCache MP 的 CPU、GDS 和 Device-DAX 三种 L1 后端，以及 L2 兼容性、配置和持久化边界。"
---

> 调研日期：2026 年 9 月 9 日。分析基于 LMCache 源码，固定版本为 `372b0697c05155f4921bf358e3a1d623dc7c230b`。
>
> 本文以 NVIDIA GPU、默认 `lmcache_driven` 传输模式下的成功读写路径为主，另外说明 CPU L1 的 engine-driven SHM 变体。结论来自源码阅读，不包含硬件性能实测。文中的源码位置均相对上述仓库根目录；它们对应这个 commit，而不是对所有 LMCache 版本的承诺。

谈到 LMCache 的多级缓存，很容易画出这样一张图：GPU 显存、CPU 内存、磁盘。它适合入门，却会掩盖 MP 模式中的一个重要事实：

**L1 首先是一个逻辑缓存层，不是某一种硬件的名字。**

在本文对应的实现中，L1 可以由三种 manager 承担：

- **CPU 内存**：把 KV chunk 放在主机内存池中，通常注册为 pinned memory。
- **GDS**：把 chunk 放在存储上的 slab 地址空间中，通过 GPU 存储 I/O 接口传输。
- **Device-DAX**：把设备映射成进程可访问的内存区域，作为 L1 分配池；还支持 DRAM 优先、DAX 承接溢出的混合形态。

它们共用一部分对象管理逻辑，但数据传输并不相同。尤其不能把 GDS 理解为“普通内存池加一个磁盘写线程”，也不能把 DAX 理解为“另一种 GDS”。

## 1. 先认识 L1：位置、职责与两组链路

### 1.1 L1 在哪里？

在默认的服务端驱动传输模式下，推理引擎与 LMCache Server 是不同进程。推理引擎持有用于计算的 GPU KV Cache；LMCache Server 维护可供复用的 L1 对象及其状态。

可以把逻辑架构画成：

```text
推理引擎进程                         LMCache Server 进程

GPU KV Cache  ←── 数据传输 ──→  GPU staging buffer ←──→ L1
      │                                                │
      └──── 注册、STORE、LOOKUP、RETRIEVE 等控制请求 ────┤
                                                       │
                                            可选 L2 Adapter
                                                       │
                                              磁盘 / 远端存储
```

图中的 GPU staging buffer 是传输时的临时显存，**不是 L1 的另一种后端**。对于本文追踪的 CUDA 路径，它用于在推理引擎的分页 KV 布局与 LMCache 的连续 chunk 布局之间进行整理。[S04][S05]

同样，“跨进程”不意味着 KV payload 必须被序列化后通过 RPC 搬运。CUDA IPC 路径在注册阶段重建对引擎 GPU tensor 的访问；后续请求主要携带 key、block ID 和同步信息，真正的数据移动由传输代码提交。engine-driven 的 pickle 传输是另一条路径，不能混在这张图里。[S04][S05][S18]

### 1.2 不要混淆两组读写

本文中的“写 L1”和“读 L1”，首先指：

| 操作 | 数据方向 | 主要目的 |
|---|---|---|
| 写 L1 | 推理引擎 GPU → L1 | 保存已经算出的 KV |
| 读 L1 | L1 → 推理引擎 GPU | 将可复用 KV 装回推理引擎 |
| 写 L2 | L1 → L2 | 通过 L2 存储策略向其他存储层写出 |
| 从 L2 预取 | L2 → L1 | 为后续 GPU retrieve 准备数据 |

`STORE` 处理写入；`LOOKUP` 所在的查询流程检查可复用前缀，并可发起 L2 预取；`RETRIEVE` 再使用已经准备好的对象执行数据搬运。因此，**不能把“查到了 key”“L2 预取已完成”和“KV 已回到 GPU”视为同一件事**。[S04][S06][S08]

L2 是可选的。默认配置的 L2 adapter 列表为空，而且 GDS L1 不能直接套用普通的 L1 内存缓冲区到 L2 的链路。[S02][S10]

## 2. 统一入口：L1Manager 管理对象，后端提供空间

### 2.1 三种 manager，而不是三套缓存状态机

`L1Manager` 的构造逻辑先检查 GDS 配置，再检查 Device-DAX 路径，否则选择普通 CPU manager：

| 选择条件 | 实际 manager | chunk 的数据落点 |
|---|---|---|
| 配置 `gds_l1_config` | `GDSL1MemoryManager` | GDS slab 中的一段空间 |
| 配置 `memory_config.devdax_path` | `DevDaxL1MemoryManager` | DAX 映射，或混合池中的 DRAM |
| 其余情况 | `L1MemoryManager` | CPU 内存池 |

配置校验禁止同时设置 GDS 和 Device-DAX。这里的互斥是 **manager 选择互斥**，并不意味着 Device-DAX manager 内部不能同时使用 DRAM。[S01][S02]

共同接口包括 `allocate()`、`free()`、`get_memory_usage()`、`get_backend_type()` 和 `get_l1_memory_desc()`。其中最后一个接口很重要：它回答的是“有没有可以交给 L2 注册的 L1 内存区域”，不是“L1 有没有存储容量”。GDS 有容量，但这个接口返回 `None`。[S03][S10]

### 2.2 空间分配完成，不等于内容已经可读

L1Manager 维护 key 到对象状态的映射，并管理读写锁。忽略异常、TTL 和多读者计数等细节，一次正常生命周期可以简化为：

```text
不存在
  │ reserve_write：分配空间并加写锁
  ▼
写入中
  │ 数据传输完成后 finish_write
  ▼
可读
  │ reserve_read：获得读锁
  ▼
读取中
  │ finish_read：释放读锁，最后一个读者退出
  ▼
可读 ── 无锁时被淘汰 ──→ 空间回收
```

三种后端都受这套对象状态管理约束。区别在于“分配空间”返回的东西：CPU 和 DAX 通常是内存支持的对象；GDS 返回的对象主要描述 slab 中的 offset 与 size。[S01][S09][S10][S14]

### 2.3 谁决定一次异步写入真正结束？

默认传输模块不会在提交 GPU 工作后立即调用 `finish_write()`。它把完成回调排入对应 stream 的顺序中，经 `DeviceHostFuncDispatcher` 执行对象状态更新。

读取也类似：先提交数据复制和 KV 布局恢复，记录完成事件，再通过 stream 后续回调调用 `finish_read_prefetched()` 释放读锁。[S04]

因此要区分三个时刻：

1. **请求处理函数返回**：服务器完成了本次请求的提交工作。
2. **GPU 完成事件就绪**：事件之前的 stream 工作已经完成。
3. **对象管理回调执行**：L1 写锁释放或读锁归还。

这个顺序避免在传输仍使用缓冲区时就把空间重新分配。它表达的是运行期间的顺序和生命周期管理，**不是落盘持久性或崩溃恢复保证**。

## 3. CPU 内存后端：GPU 与主机内存之间的双向搬运

### 3.1 L1 内存如何分配？

CPU manager 的 `create_memory_allocator()` 有两条主要分支：[S09]

- `use_lazy=True`：使用 `LazyMemoryAllocator`。这是 CLI 的默认设置，先注册初始容量，再由后台线程扩展已经注册、可以分配的范围。
- `use_lazy=False`：使用 `MixedMemoryAllocator`，建立预分配的 CPU 内存池；如果配置了 `shm_name`，可使用命名共享内存作为 backing。

Lazy allocator 的 `PIN_CHUNK_SIZE` 是 64 MiB。这个值用于分段注册和相关传输处理，**不是模型的 KV chunk token 数量，也不是所有对象都要占用 64 MiB**。[S09]

另外，`MixedMemoryAllocator` 的 “Mixed” 指 pinned tensor 分配与 byte-array buffer 分配，并不是 DRAM 与 DAX 的混合池。

在 NVIDIA 平台上，内存注册能力最终对应 CUDA 的 host-memory pinning 接口。Pinned memory 与 SHM 解决不同问题：前者关联 GPU/主机数据传输，后者关联进程间共享同一段内存。两者可以同时存在，但不能互相替代。[S09][S16][S18]

### 3.2 写链路：GPU KV → GPU 暂存区 → CPU L1

写入不是简单地把一整个引擎 tensor 原样复制到主机内存：引擎中的目标 KV 分散在不同 block，LMCache 需要先整理布局。

正常步骤如下：[S04][S05][S07]

1. 服务器找到注册过的 KV Cache 上下文，并等待引擎提供的 producer event，保证要读的 GPU 数据已准备好。
2. 按 key 调用 `StorageManager.reserve_write(..., "new")`，为需要保存的对象获取 L1 空间和写锁。
3. 根据 block ID，通过 `multi_layer_block_kv_transfer` 等 native 逻辑，把引擎分页 KV 整理到 GPU staging buffer。
4. 把 staging buffer 的内容复制到对应 CPU L1 对象。
5. 在 stream 后续回调中执行 `finish_write()`，对象变为可读，并通知 L1 listener。

```text
写 CPU L1

引擎 GPU 上的 paged KV
        │ 按 block ID gather / 布局转换
        ▼
GPU staging buffer
        │ D2H copy
        ▼
CPU L1 chunk
        │ stream 后续完成回调
        ▼
finish_write → 解除写锁 → 对象可读
```

在这个版本中，非 GDS 对象存在一条 native transfer-plan 快路径：Python 先构造批量计划，再由 `execute_object_group_transfer()` 在一次 native 调用中提交 staging copy 与 kernel。CUDA 底层的 `lmcache_memcpy_async()` 调用 `cudaMemcpyAsync()`。[S07]

当没有走这条快路径时，Python helper 分别调用 native memcpy 或 tensor 的 `copy_(..., non_blocking=True)`。**实现上是 native 批量提交还是 Python 逐项提交，不改变“GPU paged KV → GPU 暂存区 → 主机 L1”的数据拓扑。**[S04][S07]

### 3.3 读链路：CPU L1 → GPU 暂存区 → 引擎 KV

读取方向与写入相反，但不是“把刚才的写函数反过来调用”那么简单：它还要遵守预取与读锁协议。[S04][S06]

1. 查询/预取流程找到可用对象并为读取预留读锁。
2. `RETRIEVE` 通过 `read_prefetched_results()` 获取这些对象。
3. 将 CPU L1 chunk 复制到 GPU staging buffer。
4. 根据目标 block ID，把 staging buffer 中的连续数据 scatter 回引擎的分页 KV Cache。
5. 引擎通过完成事件与这次写 GPU 工作同步；服务器后续释放对应读锁。

```text
读 CPU L1

CPU L1 chunk（已持有读锁）
        │ H2D copy
        ▼
GPU staging buffer
        │ scatter / 布局转换
        ▼
引擎 GPU 上的目标 KV blocks
        │ 完成事件与后续回调
        ▼
引擎可以继续使用 KV；L1 读锁随后归还
```

即使对象已经命中 L1，GPU 侧也仍然要完成数据搬运与布局恢复。因此，L1 hit 本身不能代表完整的 retrieve 延迟。

### 3.4 与 L2 如何衔接？

CPU L1 同时是 GPU 传输路径和普通 L2 adapter 之间的缓冲区。

**写 L2**：`finish_write()` 通知 `StoreListener`；listener 把 key 加入队列并发出通知。`StoreController` 按策略取得对象读锁，再调用 adapter 的 `submit_store_task()`。在 Linux 的对应实现中，这种唤醒使用 eventfd。[S08]

**从 L2 读回**：`PrefetchController` 为缺失对象在 L1 预留写入空间，把对象交给 adapter 的 `submit_load_task()`。加载成功后，通过 `finish_write_and_reserve_read()` 把写锁转为读锁，交给待执行的 retrieve。[S08]

```text
写出：GPU → CPU L1 → StoreController → L2 adapter
预取：L2 adapter → CPU L1 → RETRIEVE → GPU
```

一个值得注意的细节是，`StoreListener` 对 `on_l1_keys_finish_write_and_reserve_read()` 不再发起写出，避免把刚从 L2 预取回来的内容马上重复存回 L2。[S08]

L2 的实际文件 I/O 或网络传输由 adapter 决定，不属于 CPU L1 allocator 的职责。也不要假设一定要等 L1 满了才会写 L2：这里存在由写完成通知驱动的异步写出路径，具体对象由 store policy 选择。

### 3.5 变体：engine-driven SHM 改变的是谁发起复制

在 engine-driven SHM 模式中，服务器可以通过 PREPARE 阶段返回 L1 slot 的 offset、length、shape、dtype。推理引擎进程映射共享内存池，直接对 slot 发起自己的传输，完成后再 COMMIT；服务器据此完成写入状态更新或释放读取锁。[S18]

```text
PREPARE：服务器分配 / 锁定 L1 slot，返回描述符
TRANSFER：引擎进程访问共享内存 slot 并执行传输
COMMIT：服务器完成对象状态更新
```

共享内存消除了“必须经服务器 RPC 传递 payload”的要求，但**没有消除 GPU ↔ CPU 的数据复制**。而且 worker 侧会单独尝试 pin 共享内存，不能因为服务器已经 pin 过就省略这个步骤。[S18]

本文后两节的主线仍是 `lmcache_driven` CUDA 路径，不把这个 SHM 变体泛化为 GDS 或 DAX 的统一入口。

## 4. GDS 后端：L1 对象变成存储 offset

### 4.1 “有 MemoryObj”不代表“有内存 buffer”

`GDSL1MemoryManager` 使用 `AddressManager` 管理 slab 的字节地址空间。分配 chunk 时返回 `GDSMemoryObject`，其中保存 offset、物理大小等元数据。[S10]

这里的 offset 不是主机虚拟地址。`GDSMemoryObject.tensor` 返回 `None`，`byte_array`、`data_ptr` 等接口不能当作普通内存对象使用。[S11]

真正的存储资源由进程级 `GDSContext` 管理：

- 打开 backing 存储并注册 GDS handle。
- 注册 GPU staging buffer 与 stream。
- 把 chunk offset、长度、GPU buffer 地址组合为异步 I/O 请求。
- 维护异步提交参数的生命周期，避免 DMA 结束前相关参数被释放。[S12]

对于 cuFile 路径，backing 是目录下固定名称的 `lmcache_gds_slab.bin`。初始化先用 `O_TRUNC` 打开并预分配，再按配置重新打开和注册；`O_DIRECT` 默认开启。[S12]

**这是一个把存储空间当作临时 L1 使用的设计，不是启动后扫描旧文件、恢复 KV 索引的设计。** GDS manager 没有 on-disk key index；cuFile slab 在启动时还会被截断重建。[S10][S12]

### 4.2 写链路：GPU KV → GPU 暂存区 → slab

GDS 复用了前面的对象预留、GPU KV 布局整理和 stream 完成机制，但改变了 staging buffer 后面的传输方式。[S04][S07][S12][S13]

1. `reserve_write()` 从 GDS slab 地址空间分配 chunk。
2. paged KV transfer kernel 将引擎 GPU KV 整理到 GPU staging buffer。
3. `lmcache_memcpy_async_d2h()` 识别到对象是 `GDSMemoryObject`。
4. 不执行普通 D2H tensor copy，而是调用 `GDSContext.transfer_async(..., WRITE)`。
5. cuFile 后端最终提交 `cuFileWriteAsync()`：把 GPU buffer 内容写到指定 slab offset。
6. 按 stream 顺序完成对象状态回调。

```text
写 GDS L1（以 cuFile 直通路径为例）

引擎 GPU 上的 paged KV
        │ gather / 布局转换
        ▼
已注册的 GPU staging buffer
        │ cuFileWriteAsync
        ▼
NVMe 文件中的 slab[offset, length]
```

所以，helper 名字里的 `d2h` 在这里不能按字面理解为“Device to Host”。到了 GDS 分支，实际方向是 **GPU → 存储**，中间没有 LMCache 自己分配的 CPU payload buffer。

### 4.3 读链路：slab → GPU 暂存区 → 引擎 KV

读取通过同一套对象类型分派进入 GDS：[S04][S07][S12][S13]

1. 查询流程取得 GDS L1 对象及其读锁。
2. `lmcache_memcpy_async_h2d()` 识别 GDS 对象，调用 `transfer_async(..., READ)`。
3. cuFile 后端提交 `cuFileReadAsync()`，把指定 offset 的数据读到 GPU staging buffer。
4. 同一 stream 后续的 KV transfer kernel 将数据 scatter 回引擎 KV blocks。
5. 记录完成事件，并在后续回调中归还读锁。

```text
读 GDS L1（以 cuFile 直通路径为例）

NVMe slab[offset, length]
        │ cuFileReadAsync
        ▼
已注册的 GPU staging buffer
        │ scatter / 布局转换
        ▼
引擎 GPU 上的目标 KV blocks
```

**GDS 绕过的是 LMCache 显式的 CPU 数据中转，不是 GPU staging buffer。** 当前传输函数遇到 GDS 对象时，还会跳过非 GDS 的 native transfer-plan 分支，走会调用 GDS helper 的逐步提交路径。[S04]

### 4.4 异步 I/O、对齐与“真直通”是不同问题

这条链路有几项实现细节值得分开看：

- GDS 分配空间使用 4 KiB 对齐；slab 总大小也向 4 KiB 取整。
- `GDSContext` 按最多 16 MiB 的 region 注册 GPU buffer，并在 region 边界切分传输。这是本文版本的实现策略，不应把这个常量直接推广为所有 GDS 实现的统一上限。
- 读写提交绑定当前 GPU stream，I/O 与前后的布局转换按该 stream 的顺序组织。
- 代码把异步提交对象保留到对应事件完成之后，解决的是参数和缓冲区的存活期问题。[S10][S12][S13]

但“调用 cuFile”与“物理上没有主机 bounce buffer”不是等价命题。NVIDIA 的官方 API 文档明确说明 cuFile 存在 compatibility mode，在不支持直通的环境或指定配置下可以转入经主机内存的兼容路径。[E01]

因此，源码中的 “No POSIX fallback” 应准确理解为：**LMCache 在 GDS 初始化失败时不自行改走普通文件后端**；它不能证明 cuFile 库内部绝不会采用兼容路径。是否获得真正的 GPU-storage 直通，还要检查驱动、文件系统、硬件拓扑和库配置。

### 4.5 GDS L1 内部还可以选择不同 I/O 实现

当前 `_gds_async` 分派层提供以下选择：[S13]

| `--gds-l1-backend` | 底层库 | `--gds-l1-path` 的含义 |
|---|---|---|
| `cufile` | `libcufile.so` | 存放 slab 的目录 |
| `hipfile` | `libhipfile.so` | 存放 slab 的目录，面向 ROCm |
| `ugds` | `libugds.so` | 专用 `ugds_drv` 字符设备路径 |
| `phx` | `libphoenix.so` | 存放 slab 的目录，使用对应 Phoenix 接口 |
| `auto` | 按平台选择 | CUDA 选择 cuFile，ROCm 选择 hipFile |

这些是 **GDS L1 下面的 I/O 子实现**，不是另外四种 L1 manager。共同点是 offset 型的 L1 对象和 GPU staging；差异下沉到驱动、handle 注册和异步读写接口。

尤其要注意 uGDS：它不创建普通文件，而是使用专用设备前部的 slab 地址范围，`O_DIRECT` 对这条路径不适用。启动时会检查设备身份和容量，但这不等于备份或保护设备上的已有数据。**不能把保存重要数据的设备用于这种配置。**[S12][S13]

### 4.6 为什么不能直接接普通 L2？

普通 L2 adapter 往往需要以下至少一种能力：

1. 从 `MemoryObj.byte_array` 或 tensor 读取/写入 payload。
2. 获取 L1 buffer 指针并注册给底层 I/O 库。

GDS L1 不提供这两种普通内存语义：`get_l1_memory_desc()` 返回 `None`，对象没有可直接交出的 host buffer。[S10][S11]

所以，这个版本不能直接画成：

```text
GPU ↔ GDS L1 ↔ 任意普通 L2 adapter
```

本文的 GDS 配置示例不启用 L2。要支持进一步分层，需要专门处理 GDS 对象的数据路径，而不是仅多加一项普通 `--l2-adapter`。这也是 **GDS L1** 与 **CPU L1 + NIXL/GDS L2 adapter** 的核心区别：后者仍有 CPU L1，只是把 GDS 用在 L2 I/O 中。

## 5. Device-DAX 后端：把设备映射当作 L1 内存池

### 5.1 初始化的核心是 mmap，而不是 cuFile

`DevDaxMemoryAllocator` 的初始化路径非常直接：[S15]

```text
open(device_path, O_RDWR)
        ↓
mmap(MAP_SHARED, PROT_READ | PROT_WRITE)
        ↓
ctypes buffer view
        ↓
torch.frombuffer(..., dtype=uint8)
        ↓
TensorMemoryAllocator 在这片区域内分配 chunk
```

`torch.frombuffer()` 在这里建立的是对映射区域的视图，不是把整个设备内容复制到另一块普通 DRAM。

随后 allocator 尝试注册这片映射。在 NVIDIA 平台，对应到 `cudaHostRegister()`。成功时，GPU/host-copy 路径可以使用已注册映射；失败时，代码记录警告并继续采用 pageable host-copy 路径。[S15][S16]

这与 GDS 的对象模型很不一样：

- GDS 对象拿的是 **slab offset**，不提供普通 tensor。
- DAX 对象拿的是 **映射区域内的内存对象**，可提供 tensor、指针或 byte view。

还有一个容易忽略的边界：代码也接受可 mmap 的普通文件。用普通文件测试 allocator 的行为，不等于证明获得了真实 Device-DAX 设备的访问特性；不能把“用了 mmap”直接当成“绕过普通文件 page cache”的证据。[S02][S15]

### 5.2 写链路：GPU KV → GPU 暂存区 → DAX 映射

对于 DAX 对象，传输函数不会进入 `GDSMemoryObject` 分支，而是使用非 GDS 的传输路径。[S04][S07][S14][S15]

1. `reserve_write()` 从 DAX arena 分配一段空间。
2. GPU kernel 将引擎分页 KV 整理到 GPU staging buffer。
3. 通过 host/device copy，把 staging 内容复制到该对象对应的 DAX 映射地址。
4. stream 后续回调完成 `finish_write()`。

```text
写 Device-DAX L1

引擎 GPU 上的 paged KV
        │ gather / 布局转换
        ▼
GPU staging buffer
        │ D2H copy，目标为 mmap 地址
        ▼
Device-DAX 映射中的 L1 chunk
```

在这条 L1 数据路径上，不会为每个 chunk 额外调用一次文件 `write()`，也不是 `cuFileWriteAsync()`。初始化打开设备、建立映射，与之后通过内存地址访问 chunk，是两个阶段。

这仍不能简称为“零拷贝”：GPU staging 与 DAX backing 之间有明确的数据复制。`cudaHostRegister()` 的存在也不意味着推理 kernel 直接在 DAX 上执行 attention。

### 5.3 读链路：DAX 映射 → GPU 暂存区 → 引擎 KV

读取时，从已锁定的对象获得映射区域的地址，执行 H2D copy，再 scatter 回引擎的 KV blocks：[S04][S07][S15]

```text
读 Device-DAX L1

Device-DAX 映射中的 L1 chunk
        │ H2D copy
        ▼
GPU staging buffer
        │ scatter / 布局转换
        ▼
引擎 GPU 上的目标 KV blocks
```

从 LMCache 的提交逻辑看，它与 CPU L1 更接近；从数据所在的硬件看，映射背后的介质和拓扑可以不同。代码中的 `non_blocking=True` 或异步 memcpy 接口，不应被解读为“无论注册是否成功，都保证传输可以充分重叠”。源码明确保留了 pin 失败后的降级路径。[S15][S16]

### 5.4 两种容量形态：纯 DAX 与 DRAM + DAX

`DevDaxL1MemoryManager` 根据 `devdax_size_in_bytes` 区分两种情况：[S14]

| 配置形态 | DRAM 部分 | DAX 部分 |
|---|---|---|
| 没有单独的 DAX overflow size | 0 | `--l1-size-gb` 对应的容量 |
| 设置了 DAX overflow size | `--l1-size-gb` 对应的容量 | 单独的 overflow 容量 |

混合形态下，`allocate()` 先尝试 local DRAM allocator，不够再尝试 DAX arena；批量分配也允许一批对象分布在 DRAM 和 DAX 两部分。[S15]

```text
同一个逻辑 L1

新对象 → 优先从 DRAM 分配
              │ 不足
              ▼
         从 DAX arena 分配
```

这里的“溢出”指 **新分配对象的落点选择**，不是先写满 DRAM，再通过 StoreController 把它复制到 DAX L2。也不能仅凭这段分配逻辑，就推导出后台自动冷热迁移或自动提升回 DRAM。

这个版本的 CLI 还有一条特殊配置归一化逻辑：如果 `--l1-devdax-path` 与某个 `dax` L2 配置中的设备路径相同，就把该设备的容量提取为 L1 overflow，并将这台设备从实际 L2 adapter 配置中移除。设备列表中其他未匹配的设备可以继续保留在 L2。[S02]

因此，**启动参数里写了 `type: dax`，不代表该设备最终仍作为 L2 运行**。理解实际拓扑时需要同时看归一化后的配置。

### 5.5 L2、SHM 与 P2P 的边界

DAX 的对象有内存视图，因此像按对象读取 `byte_array` 的文件系统 adapter，可以沿用“L1 对象 ↔ L2”的模式。纯 DAX manager 也能返回映射区域的内存描述符。[S14][S19]

但不能进一步推导“所有内存型 L2 与所有跨节点通道都兼容”：

- **混合 DRAM + DAX L1** 不是一个单一连续注册区。配置校验拒绝与 `nixl_store`、`nixl_store_dynamic` 及 RDMA 模式的 `mooncake_store` 组合。[S02]
- 这个版本的 **MP P2P** 启动检查拒绝 GDS L1 和 Device-DAX L1；这个检查覆盖所有 Device-DAX L1，不只混合形态。[S17]
- Device-DAX 要求关闭 lazy allocation，并禁止对外发布命名 SHM 池：需要 `--no-l1-use-lazy` 与空 `--shm-name`。[S02]

还有一个概念区别：`mmap(MAP_SHARED)` 的 “shared”，并不自动等于 LMCache 的命名 POSIX SHM 传输协议。后者需要明确的池名称、容量、slot 描述及 worker 侧映射过程。[S18]

### 5.6 DAX 中有字节，不等于重启后有可复用缓存

L1 的 key 索引和对象状态仍然由进程内的 L1Manager 维护。这里的 DAX L1 初始化是建立 arena 和分配器，并没有加载旧 key 索引的恢复流程。[S01][S14][S15]

即使某种设备在进程退出之后保留了原始字节，也还缺少“这些字节对应哪个 key、什么模型布局、哪些写入完整有效”的恢复信息。

同时，也不能因为设备经 DAX 暴露，就直接认定它具备断电持久性。例如 Linux CXL 文档明确区分 volatile 与 persistent 容量，CXL 内存扩展设备可能提供不同类型的容量。[E02]

因此，应分别判断：

1. **设备介质是否持久**。
2. **写入完成是否满足所需的持久化语义**。
3. **LMCache 是否实现了元数据和对象恢复**。

本文这条 Device-DAX L1 路径不能仅凭第 1 项，就宣称支持第 3 项。也不要把独立 DAX L2 的功能直接套到 DAX L1 上。

## 6. 三条链路放在一起看

### 6.1 相同的是 GPU 布局整理，不同的是 staging 之后的落点

以下均指本文的默认 CUDA 服务端驱动主路径：

```text
CPU L1
  写：引擎 GPU KV → GPU staging → CPU DRAM
  读：CPU DRAM → GPU staging → 引擎 GPU KV

GDS L1
  写：引擎 GPU KV → GPU staging → GDS I/O → slab
  读：slab → GDS I/O → GPU staging → 引擎 GPU KV

Device-DAX L1
  写：引擎 GPU KV → GPU staging → DAX mmap 区域
  读：DAX mmap 区域 → GPU staging → 引擎 GPU KV
```

GDS 图表示 LMCache 的显式提交路径；底层库是否走兼容中转另行验证。DAX 混合模式中，有些 chunk 的最终落点仍是 DRAM。

### 6.2 对比表

| 维度 | CPU L1 | GDS L1 | Device-DAX L1 |
|---|---|---|---|
| 核心资源 | CPU 内存池 | 存储 slab 地址空间 | 设备 mmap arena，可加 DRAM 池 |
| 对象的关键定位信息 | 内存对象/地址 | slab offset、size | 映射中的内存对象/地址 |
| GPU 传输方式 | H2D / D2H copy | GDS 异步 READ / WRITE | H2D / D2H copy 到映射地址 |
| 是否使用 GPU staging | 是 | 是 | 是 |
| LMCache 是否显式用 CPU buffer 中转 payload | 是，L1 本身就是 host buffer | 否；底层兼容模式另论 | 使用 host-addressable 映射；不是必须再建一份普通 DRAM 副本 |
| 普通 byte-array L2 路径 | 可提供对象 buffer | 不提供该 buffer 语义 | 可提供对象 buffer；注意混合池的注册限制 |
| 本文版本是否直接恢复旧 L1 key 索引 | 否 | 否，文件型 slab 还会重建 | 否 |
| 特别需要核实 | 内存注册、NUMA、复制与 kernel 开销 | 驱动、库、文件系统、直通/兼容模式 | 设备映射、pin 成败、介质与拓扑 |

表中关于路径、对象类型和兼容性的依据见 [S01]—[S19]；关于 GDS 兼容模式与介质属性的补充见 [E01][E02]。

### 6.3 性能结论应该怎么建立？

源码能告诉我们数据经过哪里，却不能直接给出三种后端的吞吐量排行榜。

如果要做后续实测，建议先控制模型、chunk size、GPU KV 布局、batch、并发数与可用容量，再分别记录：

- L1 命中时，完整 STORE / RETRIEVE 的时延和有效带宽。
- GPU gather/scatter、staging copy、GDS I/O 各自的时间。
- CPU 使用率、host memory 注册情况与 GPU 临时显存占用。
- DAX pin 失败时是否降级，以及 GDS 是否进入 compatibility mode。
- 开启 L2 后，写出与预取对主链路和缓冲区占用的影响。

这是一个基于调用链的验证方案，不是本文已经测得的结果。尤其不能只测设备顺序读写带宽，就将它当作端到端 KV retrieve 的性能。

## 7. 配置示例与源码阅读指南

### 7.1 使用示例前先明确边界

下面是三种后端及 DAX 混合形态的 **LMCache Server 侧配置示例**。目录、设备路径与本机 HTTP 地址均为通用示例，不代表实际部署环境。它们假设对应版本已经安装，CUDA/设备/库等环境满足要求；推理引擎的 MP connector 配置需另行完成。

虽然参数名包含 `gb`，本版本配置解析使用 `1 << 30` 换算，所以示例中的数值按 GiB 理解。[S02]

**这些命令没有在调研期间执行。GDS 文件型 slab 初始化会截断同名文件；DAX 映射会被实际写入。请只使用明确划给缓存、没有重要数据的目录或设备，且不要让多个独立服务器共用同一个 slab 文件或 DAX 地址范围。**

### 7.2 CPU L1：默认 lazy 内存池

```bash
lmcache server \
  --supported-transfer-mode lmcache_driven \
  --l1-size-gb 32 \
  --l1-init-size-gb 4 \
  --eviction-policy LRU
```

含义：CPU L1 目标容量 32 GiB，lazy 初始容量 4 GiB，使用 LRU 淘汰，不启用 L2。

若要使用预分配池而不是 lazy allocator，可以改用 `--no-l1-use-lazy`。命名 SHM 则还需要显式配置池名称和相匹配的传输模式，不能仅凭关闭 lazy 就认定启用了 engine-driven SHM。[S02][S09][S18]

### 7.3 GDS L1：cuFile slab

```bash
lmcache server \
  --supported-transfer-mode lmcache_driven \
  --l1-size-gb 256 \
  --gds-l1-path /mnt/nvme/lmcache-gds-server-a \
  --gds-l1-backend cufile \
  --gds-l1-use-direct-io \
  --shm-name "" \
  --eviction-policy LRU
```

这里的 256 GiB 用来确定 slab 容量，不是分配一个 256 GiB CPU L1。GDS manager 直接取代 CPU L1 manager；仍然存在 host 侧元数据，以及 GPU staging buffer 等其他运行资源。[S01][S10][S12]

例子显式使用 `lmcache_driven`，不启用普通 L2，并让 SHM 名称保持为空。实际 slab 文件是 `/mnt/nvme/lmcache-gds-server-a/lmcache_gds_slab.bin`。

### 7.4 纯 Device-DAX L1

```bash
lmcache server \
  --supported-transfer-mode lmcache_driven \
  --l1-size-gb 64 \
  --l1-devdax-path /dev/dax0.0 \
  --no-l1-use-lazy \
  --shm-name "" \
  --eviction-policy LRU
```

含义：L1 数据池来自 `/dev/dax0.0` 的 64 GiB 映射，不另建一个同容量的 local DRAM L1。这里“纯 DAX”只描述 KV 数据池，不意味着整个进程不占主机内存。

设备需要具备足够容量、访问权限，并满足其实际映射要求。不要假设每种 DAX 映射都能成功被 CUDA 注册；启动警告和后续传输表现需要单独确认。[S15][S16]

### 7.5 DRAM + DAX 混合 L1

下面演示本版本的“同路径 DAX adapter 配置归一化”机制：[S02][S14]

```bash
lmcache server \
  --supported-transfer-mode lmcache_driven \
  --l1-size-gb 32 \
  --l1-devdax-path /dev/dax0.0 \
  --no-l1-use-lazy \
  --shm-name "" \
  --eviction-policy LRU \
  --l2-adapter '{"type":"dax","device_path":"/dev/dax0.0","max_dax_size_gb":64}'
```

归一化后的含义是：

```text
L1：32 GiB DRAM + 64 GiB Device-DAX
L2：示例中的同路径 DAX 设备已被提取，不再是独立 L2
```

它不是“32 GiB CPU L1 + 64 GiB DAX L2”。这个差别会影响分配策略、统计口径，以及能否使用需要单一内存注册区的 L2 adapter。

### 7.6 如何确认实际启用了什么？

先看配置和初始化日志，再看状态，而不是仅根据某个参数名判断。

- GDS manager 会记录 `GDS L1 tier enabled; CPU pinned-DRAM L1 disabled`。
- Device-DAX manager 会记录 `Device-DAX L1 tier enabled; CPU-only L1 disabled`。这条日志表示选择了 DAX manager，不证明它内部没有 DRAM 子池。
- DAX pin 失败会记录 `falling back to pageable host copies`。
- GDSContext 会记录 slab 路径、容量、后端等初始化信息。[S01][S12][S15]

在默认 HTTP 地址可用时，可以读取状态：

```bash
curl -s http://localhost:8080/status | jq
```

L1 状态中包括 `memory_used_bytes`、`memory_total_bytes`、`memory_configured_bytes` 和读写锁对象数量。Lazy 模式下，当前 allocator 容量与最终配置容量可能不同；字段名带 `memory` 也不意味着它统计的一定是 CPU DRAM。[S01][S20]

这些信息能确认软件选择的路径和逻辑状态，但不能代替对 GPU-storage 直通、DAX pin 成功及物理数据路径的验证。

### 7.7 按调用顺序阅读源码

下表既是源码阅读指南，也是正文中 `[Sxx]` 的定位索引。所有路径都基于本文开头固定的 commit。

| 编号 | 关注点 | 源码入口 |
|---|---|---|
| S01 | manager 选择、对象状态与锁、L1 状态统计 | `lmcache/v1/distributed/l1_manager.py:137`；后端选择从 `lmcache/v1/distributed/l1_manager.py:187` 开始 |
| S02 | 配置、容量换算、DAX overflow 归一化、后端限制 | `lmcache/v1/distributed/config.py:30`；CLI 从 `lmcache/v1/distributed/config.py:398` 附近开始 |
| S03 | 各 manager 的结构化公共接口 | `lmcache/v1/distributed/memory_manager/l1_manager_protocol.py:16` |
| S04 | 主传输链路、native 分支与 GDS 分支、完成回调 | `lmcache/v1/multiprocess/modules/lmcache_driven_transfer.py:284`；STORE 从 `lmcache/v1/multiprocess/modules/lmcache_driven_transfer.py:1040` 开始；RETRIEVE 从 `lmcache/v1/multiprocess/modules/lmcache_driven_transfer.py:1284` 开始 |
| S05 | CUDA IPC tensor 重建、GPU staging 分配与注册 | `lmcache/v1/platform/cuda/cache_context.py:46`；GDS buffer 注册位于 `lmcache/v1/platform/cuda/cache_context.py:449` |
| S06 | L1 reserve、预取结果读取与读锁释放 | `lmcache/v1/distributed/storage_manager.py:182` |
| S07 | memcpy 分派、native 批量提交与 CUDA copy | `lmcache/v1/gpu_connector/gpu_ops.py:18`；`csrc/cuda/mp_mem_kernels.cu:477`；`csrc/cuda/mem_kernels.cu:1366` |
| S08 | L2 写出通知与预取完成状态转换 | `lmcache/v1/distributed/storage_controllers/store_controller.py:121`；`lmcache/v1/distributed/storage_controllers/prefetch_controller.py:1129`；`lmcache/v1/distributed/storage_controllers/prefetch_controller.py:1349` |
| S09 | CPU allocator 的选择、lazy 扩容与预分配池 | `lmcache/v1/distributed/memory_manager/l1_memory_manager.py:44`；`lmcache/v1/memory_allocators/lazy_memory_allocator.py:59`；`lmcache/v1/memory_allocators/mixed_memory_allocator.py:29` |
| S10 | GDS slab 空间分配，无可注册 L1 buffer | `lmcache/v1/distributed/memory_manager/gds_l1_memory_manager.py:24` |
| S11 | GDSMemoryObject 的 offset 与不支持的内存接口 | `lmcache/v1/memory_management.py:1051` |
| S12 | GDSContext、slab 初始化、buffer 注册与异步提交 | `lmcache/v1/gpu_connector/gds_context.py:119`；slab 打开逻辑从 `lmcache/v1/gpu_connector/gds_context.py:271` 开始 |
| S13 | GDS 子实现分派与 cuFile 异步 API | `lmcache/v1/gpu_connector/_gds_async.py:1`；`lmcache/v1/gpu_connector/_cufile_async.py:326`；其他后端见 `lmcache/v1/gpu_connector/_hipfile_async.py:1`、`lmcache/v1/gpu_connector/_ugds_async.py:1`、`lmcache/v1/gpu_connector/_phx_async.py:1` |
| S14 | 纯 DAX 与混合 DRAM/DAX manager | `lmcache/v1/distributed/memory_manager/devdax_l1_memory_manager.py:23` |
| S15 | DAX mmap、pin 尝试、DRAM 优先分配 | `lmcache/v1/memory_allocators/devdax_memory_allocator.py:234`；`lmcache/v1/memory_allocators/devdax_memory_allocator.py:337`；`lmcache/v1/memory_allocators/devdax_memory_allocator.py:571` |
| S16 | NVIDIA host memory 注册接口 | `lmcache/v1/platform/cuda/pin_memory.py:108` |
| S17 | MP P2P 对 GDS / Device-DAX L1 的启动限制 | `lmcache/v1/multiprocess/http_server.py:232` |
| S18 | engine-driven SHM slot、worker 映射与 COMMIT | `lmcache/v1/multiprocess/modules/server_transfer.py:310`；`lmcache/v1/multiprocess/transfer_context/shm.py:83`；SHM 池发布条件见 `lmcache/v1/multiprocess/engine_context.py:310` |
| S19 | 按对象 byte view 读写的文件系统 L2 示例 | `lmcache/v1/distributed/l2_adapters/fs_l2_adapter.py:636` |
| S20 | `/status` 与各后端配置文档 | `docs/source/mp/http_api.rst:340`；`docs/source/mp/configuration.rst:285` |

外部资料只用于补充硬件和底层库的语义，不用于替代对 LMCache 的代码分析：

- **E01**：NVIDIA《GPUDirect Storage API Reference Guide》，§3.3 “cuFile Compatibility Mode”。地址：`https://docs.nvidia.com/gpudirect-storage/api-reference-guide/index.html`。
- **E02**：Linux Kernel Documentation《Devices and Protocols》，CXL Type-3 与 volatile/persistent 容量说明。地址：`https://docs.kernel.org/driver-api/cxl/devices/device-types.html`。

## 结语

理解 LMCache MP 的 L1，关键是问清楚：**对象指向哪里，GPU staging 与它之间使用什么传输接口？** 三种后端共享对象生命周期，却不共享所有 buffer 语义、L2 兼容性或持久化能力。配置与性能验证都应从这些边界出发。
