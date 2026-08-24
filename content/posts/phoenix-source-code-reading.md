---
title: "Phoenix 源码解读：把 GPU 显存变成 Linux I/O 可直达的内存"
date: 2026-08-24T14:40:15+08:00
draft: false
tags: ["GPU Direct Storage", "CUDA", "Linux 内核", "io_uring", "vLLM", "源码解读"]
categories: ["存储系统"]
summary: "深入解析 xPU-IO/Phoenix：它如何通过内核模块、用户态库和应用适配器，将 SSD 数据直接送入 GPU 显存；并比较 full BAR、staging、batch、stream 等数据路径的实现与取舍。"
---

在大模型推理、训练和 KV Cache 分层存储中，数据搬运经常走这样一条路：

```text
SSD -> CPU 内存缓冲区 -> GPU 显存
```

第二跳通常由 `cudaMemcpy` 完成。CPU 内存只是中转站，却会消耗内存容量、内存带宽、PCIe 带宽和 CPU 周期。NVIDIA GPUDirect Storage（GDS）尝试让存储设备直接 DMA 到 GPU，但它把文件系统、内核模块、用户态库和 CUDA 驱动紧密地绑定在一起，部署与调试并不轻量。

[xPU-IO/Phoenix](https://github.com/xPU-IO/phoenix) 的目标，是重新拆分这条 I/O 栈：让 Linux 原生文件 I/O 能够直接读写一段代表 GPU 显存的 CPU 地址，从而复用 `pread/pwrite`、`io_uring`、线程池和现有文件系统，而不是再造一套文件访问接口。

本文基于 Phoenix 仓库提交 [`638942ee63e8ce691adc967de3825440d55e3936`](https://github.com/xPU-IO/phoenix/commit/638942ee63e8ce691adc967de3825440d55e3936) 阅读。该提交日期为 2026 年 8 月 21 日。项目仍在快速演进，文档和代码之间偶尔存在版本差异，以下分析以源码实际行为为准。

## 一句话理解 Phoenix

Phoenix 的核心不是“用户态直接驱动 NVMe”，也不是“绕过 Linux 内核”。它做的是：

> 将 GPU BAR 中的一段显存重映射为 Linux 可识别的页，再把这些页映射到用户进程；之后普通 `pread/pwrite` 或 `io_uring` 就能以这段用户地址作为 I/O 缓冲区，底层块设备 DMA 最终落到 GPU 显存。

整体分成三层：

```text
应用与框架
  vLLM / LMCache / 自定义应用
          |
          v
应用适配器
  phxloader / phxcache / phxfile
          |
          v
用户态 libphoenix
  设备发现、显存注册、地址解析、同步/批量/异步 I/O
          |
          v
内核模块 phxfs
  GPU BAR 管理、dev_pagemap、P2P 页、mmap/ioctl
          |
          v
Linux 文件系统与块层
  pread/pwrite 或 io_uring -> NVMe/RDMA DMA
```

这个拆分带来一个重要结果：文件系统和存储后端基本看不到 CUDA API。GPU 厂商相关逻辑被压缩到内核 `phxfs_p2p_ops` 和用户态 `DevConnector` 两个接口后面。

## 代码地图

仓库的主要目录很清晰：

| 目录 | 职责 |
| --- | --- |
| `module/` | `phxfs` 内核模块，管理 GPU BAR、页重映射、字符设备、`mmap/ioctl` |
| `libphoenix/` | 用户态 C/C++ 库，实现设备生命周期、显存注册和多种 I/O API |
| `libphoenix/io_engine/` | 同步与 `io_uring` 引擎、NUMA 感知 worker pool |
| `libphoenix/connectors/` | CUDA 等厂商运行时封装 |
| `adapters/vLLM/phxloader/` | safetensors 权重直接加载到 GPU 的 vLLM 适配器 |
| `adapters/lmcache/phxcache/` | 面向 LMCache KV Cache 的 Python/C++ 批量 I/O 封装 |
| `adapters/lmcache/phxfile/` | 仿 `cuFile/hipFile` 风格的稳定 C ABI 与 stream 异步接口 |

下面沿着一笔“把模型权重从文件读进 GPU”的请求，逐层阅读实现。

## 1. 内核层：让 GPU BAR 进入 Linux 内存模型

### 1.1 设备发现不是简单枚举 PCI 设备

`module/phxfs.c` 在加载时寻找目标厂商的 PCI 设备，读取 GPU BAR 和显存信息，并为每个 GPU 建立 Phoenix 设备描述。当前真正实现的是 NVIDIA 后端；AMD、华为等厂商已有接口和构建选项，但仍属于待实现路径。

Phoenix 默认不会粗暴接管整个 BAR。它先读取：

```text
/sys/kernel/debug/x86/pat_memtype_list
```

检查 GPU BAR 范围内是否存在 PAT 内存类型冲突，然后：

1. 在显存头尾各保留 128 MiB；
2. 将中间区域按 16 MiB 单元扫描；
3. 跳过与现有 PAT 映射冲突的单元；
4. 合并连续可用单元，形成可重映射段。

这段逻辑揭示了 Phoenix 的现实约束：GPU BAR 不是一块可以随意重新声明的裸物理地址。CUDA、驱动和其他内核模块可能已经为其中一部分建立缓存属性；重复映射会触发冲突，严重时会让模块加载失败。

### 1.2 `dev_pagemap` 是桥梁

Phoenix 要让文件 I/O 接受 GPU 显存作为用户缓冲区，必须让 Linux 能从用户虚拟地址获得合法的 `struct page`。其做法是为 BAR 区域创建 `dev_pagemap`，将设备内存纳入 `ZONE_DEVICE`/PCI P2P 内存模型。

概念上可以理解为：

```text
GPU 显存地址
   |
GPU BAR 物理窗口
   |
dev_pagemap / ZONE_DEVICE page
   |
用户态 VMA
   |
pread / io_uring 使用的用户缓冲区
```

这样，Linux 块层在 pin 用户页、构造 bio 或散列表时，拿到的不再是普通 DRAM 页，而是指向 GPU BAR 的设备页。支持 PCI P2P DMA 的设备可以直接把数据送到这些页背后的显存。

### 1.3 厂商差异被收敛为 `phxfs_p2p_ops`

`module/phxfs-backend.h` 定义了一组后端函数指针，内核核心代码不直接调用 NVIDIA 私有接口。NVIDIA 实现在 `module/nvidia-backend.c` 中，主要负责：

- 获取 GPU 显存大小与 BAR 信息；
- pin 一段用户 GPU 虚拟地址；
- 获取对应的物理页表/总线地址；
- 释放页表和 pin；
- 响应驱动侧的失效回调。

这个边界是 Phoenix 可移植性的关键。若要支持 AMD 或 NPU，理论上只需实现新的内核后端和用户态 connector，不必改动注册表、I/O 引擎和适配器的主体逻辑。

但“接口存在”不等于“已支持”。当前代码、测试和部署前提都明显围绕 NVIDIA CUDA 环境设计，不能把构建选项误解成生产可用的多厂商实现。

## 2. 显存注册：建立同一段显存的双重地址

应用拿到的是 CUDA 设备指针，例如：

```cpp
void *gpu_ptr;
cudaMalloc(&gpu_ptr, size);
```

Linux 文件 API 不能直接把这个设备指针当作普通用户地址。`phxfs_regmem()` 的任务，就是为它建立一个 CPU 进程可见、但实际指向同一段 GPU 显存的映射地址。

### 2.1 用户态注册流程

`libphoenix/phx_mem.cpp` 中的主流程可概括为：

```text
phxfs_regmem(device, gpu_ptr, len)
  |
  |-- 对齐到 Phoenix 页大小
  |-- mmap(/dev/phxfsN, len)
  |-- ioctl(PHXFS_IOCTL_MAP, gpu_ptr, len, mmap_addr)
  |-- 插入进程内注册区间表
  `-- 返回 target_addr（host-visible 映射）
```

这里出现了两个不同地址：

- **原始地址**：CUDA 返回的 GPU 设备地址，应用持有；
- **目标地址**：Phoenix `mmap` 得到的 CPU 虚拟地址，I/O 引擎使用。

两者指向同一段 GPU 数据，但服务于不同执行环境。应用继续使用原始 GPU 指针做 kernel 计算；Phoenix 将目标地址交给 `pread/pwrite`。

### 2.2 内核 `MAP` ioctl 做了什么

`PHXFS_IOCTL_MAP` 进入 `module/phxfs-mem.c` 后，大致经历：

1. 调用厂商 P2P 后端 pin 用户指定的 GPU 内存；
2. 获取 GPU 页表和 DMA 地址；
3. 把页表与当前 `mmap` 创建的 VMA 关联；
4. 建立原始 GPU 地址到 Phoenix 映射区间的元数据；
5. 注册失效回调，处理 CUDA 释放或驱动撤销映射的情况。

`phxfs_deregmem()` 走相反路径：先 `UNMAP` ioctl 释放 GPU pin，再解除 `mmap` 和用户态区间记录。代码还处理进程异常退出：即使应用忘记显式 deregister，文件释放路径也会尝试回收残留映射。

### 2.3 为什么必须有注册表

后续 I/O API 接收的仍是应用熟悉的 GPU 指针加偏移：

```cpp
phxfs_read(fd, device_id, gpu_ptr, buf_offset, nbytes, file_offset);
```

`libphoenix` 需要从区间表中找到包含 `[gpu_ptr + offset, gpu_ptr + offset + nbytes)` 的注册项，再计算对应目标地址：

```text
host_addr = mapping.target_addr
          + (gpu_ptr + buf_offset - mapping.original_addr)
```

如果范围跨出注册区间，代码返回 `-EFAULT`，而不是允许文件系统写入未知地址。批量 API 还会按设备一次持锁、批量解析多个请求，避免每个请求重复获取注册表锁。

## 3. I/O 路径：主体其实是朴素的 POSIX I/O

地址解析完成后，Phoenix 的数据面出奇地简单。

### 3.1 同步 I/O

`libphoenix/phx_io.cpp` 最终调用共享的 `phxfs_io_loop()`：

```text
resolve GPU pointer -> host_addr
          |
          v
pread(fd, host_addr, chunk, file_offset)
```

写路径则是 `pwrite`。循环会：

- 将单次请求切成不超过 1 GiB 的块；
- 对 `EINTR` 重试；
- 累加短读/短写；
- 返回实际完成字节数或负 errno。

1 GiB 分块不是性能调优噱头，而是为了不超过 Linux `MAX_RW_COUNT` 一类边界，并让超大模型文件的偏移与返回值更可控。

因此 Phoenix 并未重新实现 ext4、XFS、NFS 或分布式文件系统协议。只要目标文件系统能够把 I/O 正确下发到支持相应 DMA 的块设备/网络路径，Phoenix 可以继续使用标准文件描述符语义。

### 3.2 CPU 缓冲区也是一等公民

批量请求的 `device_id < 0` 表示普通 CPU 地址。也就是说，同一套 batch 引擎可以混合处理 CPU 和 GPU 缓冲区。这让适配器可以在不额外维护两套调度代码的情况下，决定某些小对象走 DRAM，某些大对象走显存直达。

## 4. Batch 与异步：io_uring 外面还有一层 NUMA worker pool

Phoenix 的批量接口使用 `phxfs_io_req_t` 描述请求，每项包含：

```text
fd + device_id + buf + buf_offset + nbytes + file_offset + result
```

同步批量调用为：

```cpp
phxfs_read_batch(reqs, n);
phxfs_write_batch(reqs, n);
```

异步批量分成 submit/wait：

```cpp
handle = phxfs_batch_submit_read(reqs, n);
// CPU/GPU compute
phxfs_batch_wait(handle);
```

### 4.1 为什么不是主线程直接提交 io_uring

`libphoenix/io_engine/io_pool.cpp` 构建了共享 worker pool。请求按 NUMA 节点分组后，交给绑定在对应节点的工作线程；每个 worker 拥有自己的 I/O engine。

这样设计有几个理由：

- `io_uring` ring 与完成事件由固定线程管理，避免多个调用线程争用；
- 可以将提交和回收放在更接近 GPU/NVMe 的 NUMA 节点；
- 同步和异步上层 API 共用同一套任务模型；
- 当 `io_uring` 不可用时，可以切换到同步 `pread/pwrite` 引擎。

### 4.2 `io_uring` 是并发器，不改变 DMA 本质

`io_engine_uring.cpp` 为请求准备 SQE，提交后批量收割 CQE。真正决定数据是否直达 GPU 的，不是 `io_uring` 本身，而是 SQE 中用户缓冲区背后的页是不是 Phoenix 创建的 P2P 设备页。

因此可以把职责分开理解：

```text
phxfs/dev_pagemap：决定“DMA 到哪里”
io_uring/worker pool：决定“多少 I/O 如何并发”
```

这是 Phoenix 架构中非常干净的一处解耦。

### 4.3 异步句柄持有生命周期引用

异步提交前，Phoenix 已经完成地址解析，并持有相关设备和映射节点的引用。`wait()` 复制每项结果后才释放这些引用。这样可以防止批量 I/O 在后台运行时，设备或注册映射被正常关闭。

不过 API 契约仍要求调用方不要并发释放正在使用的原始 GPU 内存。库能保护自己的元数据生命周期，却无法替应用修复“CUDA 指针已经被释放”的逻辑错误。

## 5. 两种映射模式：性能与兼容性的正面权衡

这是当前 Phoenix 源码中最值得关注的演进。

### 5.1 Full BAR 模式

`full` 模式在模块加载时重映射可用 GPU BAR 区域。数据路径最短：

```text
SSD -> GPU 用户缓冲区
```

优点是没有额外拷贝，最接近“真正的存储直达显存”。代价是 Phoenix 对 BAR 映射的占用可能与 `nvidia-peermem`、RDMA、其他 GDS/RDMA 组件产生资源或 PAT 属性冲突。

源码和文档明确把 full 模式设为显式 opt-in，而不是默认值。

### 5.2 Staging 模式

默认 `staging` 模式不直接重映射用户的 GPU buffer，而是在 GPU 上创建 Phoenix 自己管理的 staging pool：

```text
SSD -> GPU staging buffer -> D2D copy -> 用户 GPU buffer
```

这仍然绕过 CPU DRAM；第二跳是 GPU 内部/设备到设备复制，而不是回到主机。但它不再是严格的一跳 DMA。

`libphoenix/phx_staging.cpp` 默认配置为：

- 每个设备 2 个 staging slot；
- 默认总大小 256 MiB，可通过环境变量调整；
- 大小按 2 MiB 粒度对齐；
- 读取时在 slot 之间形成流水线：一个 slot 做存储 I/O，另一个 slot 做 D2D；
- 写入时反向执行：先把用户数据 D2D 到 staging，再从 staging 写文件。

其价值是让 Phoenix 与需要直接 pin 用户 GPU 内存的 RDMA/peermem 方案更容易共存。代价也很明确：多一次 D2D、额外 staging 显存、复杂的流水线错误处理。

### 5.3 默认安全，但 API 能力不完全等价

Staging 模式支持同步和 batch 路径，但 stream API 当前明确拒绝 staging 设备，返回 `-EOPNOTSUPP`。原因是 CUDA host callback 内不能调用 CUDA API，而 staging 的 D2D 腿恰好需要 CUDA 调用。

异步 batch 在 staging 模式下也不是“提交后后台执行”：源码会把整批工作推迟到 `wait()` 阶段。内部两 slot 流水仍可重叠存储 DMA 与 D2D，但它不提供跨调用的计算/I/O 重叠。

因此使用者不能只看 API 名称判断异步语义，必须同时检查当前 map mode。

## 6. Stream API：用 CUDA Host Function 建立顺序语义

Phoenix 的 stream API 不是让 CUDA stream 自己发起文件 I/O，而是调用 `cudaLaunchHostFunc`，把一个纯 CPU I/O 回调插进 stream：

```text
GPU kernel A
   |
cudaLaunchHostFunc(Phoenix pread/pwrite)
   |
GPU kernel B
```

CUDA 保证 host function 在前序任务完成后执行，并阻塞后续 stream 工作。因此文件 I/O 与 GPU kernel 获得了 stream-ordered 语义。

`phxfs_read_stream()`/`write_stream()` 的关键契约包括：

- `nbytes`、文件偏移、buffer 偏移和结果变量必须活到 stream 越过该操作；
- 文件描述符、buffer 和注册映射也必须保持有效；
- 提交成功只表示 callback 已入队，真正 I/O 结果写入 `bytes_done`；
- callback 无论 I/O 成败都返回，不能把 CUDA stream 永久卡死；
- callback 内只运行主机侧 I/O，不调用 CUDA API。

这个实现很巧妙：它不需要 Phoenix 维护每个 stream 的状态，也不需要 CUDA Graph 或自定义 GPU kernel。但 host callback 执行同步 `pread/pwrite`，会占用 CUDA runtime 的回调执行资源；如果提交大量细碎 I/O，开销和调度公平性值得专项评估。

## 7. vLLM 适配器：从 safetensors 元数据到批量 DMA

`adapters/vLLM/phxloader` 展示了 Phoenix 如何进入真实 AI 框架。

### 7.1 Python 负责语义，C++ 负责数据搬运

Python 侧解析 safetensors：

1. 读取文件头部长度；
2. 解析 JSON tensor metadata；
3. 计算每个 tensor 在文件中的绝对偏移与字节数；
4. 把多个 tensor 按目标连续 buffer 组织成 read group。

C++ `PhxLoader` 则负责：

1. 打开对应 Phoenix 设备；
2. 注册 PyTorch/CUDA 分配的目标显存；
3. 为每个 tensor 构造 `phxfs_io_req_t`；
4. 调用同步或异步 batch read；
5. 检查每个请求的 `result` 是否等于预期字节数；
6. 等待后 deregister。

这种分层非常合理：模型格式、tensor 名称映射和框架约定留在 Python；对齐、注册和 I/O 热路径留在 C++。

### 7.2 为什么不是一个 tensor 调一次 read

模型权重可能包含成百上千个 tensor。逐 tensor 发起系统调用和锁操作会放大固定开销。Phoenix 先构造请求数组，再由 batch 层一次解析注册区间、按 NUMA 分发并由 `io_uring` 并发提交，更符合模型加载的访问形态。

### 7.3 safetensors 很适合直接加载

safetensors 的 payload 是可按偏移定位的连续原始 tensor 数据，没有 pickle 反序列化，也不要求 CPU 先构造复杂对象。因此：

```text
文件 offset + tensor size + 目标 GPU offset
```

已经足以描述一笔 DMA。这也是 Phoenix 适配器能够保持轻量的根本原因。

## 8. LMCache 的两种接法

仓库同时提供了两种 LMCache 风格封装。

### 8.1 `phxcache`：面向 Python 的批量 API

`phxcache` 与 `phxloader` 类似，通过 pybind11 暴露注册、批量读写和文件对象。它更适合由 LMCache 自己组织 KV block，再批量下发多个离散 offset。

### 8.2 `phxfile`：兼容 cuFile/hipFile 风格的 C ABI

`phxfile` 暴露 driver open、buffer register、file handle register、stream register 和 async read/write。实现中：

- 启动时探测并打开可用 Phoenix 设备；
- buffer 注册时依次尝试设备，找到覆盖该 GPU 地址的设备；
- file handle 只是 POSIX fd 的 identity boxing；
- stream register 当前不保存任何 per-stream 状态；
- async read/write 调用 Phoenix stream API，并修正两套 ABI 中 buffer/file offset 参数顺序的差异。

这一层的意义不是增加能力，而是降低已有 `cuFile`/`hipFile` 调用方切换后端的成本。

## 9. 设计亮点

### 9.1 把创新集中在内存语义，而不是重造存储栈

Phoenix 最漂亮的地方，是把问题转化为“如何让 GPU 内存成为合法 I/O buffer”。一旦地址与页模型打通，文件访问继续复用 Linux 的成熟设施。

### 9.2 内核后端与用户态 connector 双重隔离厂商差异

内核侧处理 pin/page table/BAR，用户态侧处理设备 ID 映射、D2D、stream callback 和 profiler range。核心逻辑不散落 CUDA 调用，为未来多厂商支持留出了相对清楚的边界。

### 9.3 同步、batch、异步、stream 使用同一注册模型

不同 API 最终都围绕同一张“原始设备地址 -> host-visible P2P 地址”注册表工作，减少了路径分叉和一致性风险。

### 9.4 Staging 是工程化妥协，不是退化成 CPU bounce buffer

它牺牲一跳直达，换取与 RDMA/peermem 的兼容，而且中间缓冲仍在 GPU 显存中。对于需要同时运行分布式通信和存储加载的 AI 系统，这种默认选择可能比追求理论最短路径更实用。

## 10. 当前限制与风险

### 10.1 真正可用的厂商后端仍只有 NVIDIA

接口虽然是 vendor-neutral，但 AMD/Huawei 的实现尚未落地。跨厂商能力目前更多是架构承诺。

### 10.2 内核兼容性是最大部署成本

Phoenix 依赖 GPU 驱动导出的 P2P 能力、`dev_pagemap`、PCI P2PDMA、内核模块编译环境以及合适的文件系统/设备路径。它并不是安装一个 Python wheel 就能工作的组件。

### 10.3 大注册区仍有限制

项目 roadmap 指出，单次 `regmem` 超过 32 GiB 仍可能因为映射描述使用 `kmalloc` 而失败，后续计划切换到 `kvalloc`。大模型应用应把权重 buffer 分段注册，而不是假设任意大的连续区域都能成功。

### 10.4 `full` 模式具有资源排他性

GPU BAR 与 PAT 映射可能已被其他驱动或进程占用。部署 full 模式前需要清理冲突组件，并根据 `dmesg`、PAT 列表和 GPU 拓扑定位问题。

### 10.5 “支持某文件系统”仍需逐环境验证

Phoenix 复用 POSIX I/O，不代表任意文件系统、网络文件系统和块设备组合都会自动形成有效 P2P DMA。页面 pin、GUP 对 `ZONE_DEVICE`/`PCI_P2PDMA` 的处理、IOMMU、ACS、NUMA 拓扑和存储驱动实现都可能改变结果。

### 10.6 Stream callback 适合顺序集成，不一定适合海量小 I/O

一个 callback 内执行主机阻塞 I/O，语义直观但成本不低。大块权重、KV block 更匹配 Phoenix；大量几 KB 的随机请求需要实测，并可能更适合 batch 聚合。

## 11. 一次完整读请求的调用链

最后把 full 模式下的读取串起来：

```text
PyTorch 分配 GPU buffer
  |
phxloader.PhxLoader.regmem()
  |
phxfs_regmem()
  |-- mmap /dev/phxfsN
  `-- ioctl(PHXFS_IOCTL_MAP)
        |-- NVIDIA P2P pin GPU pages
        `-- 建立 GPU VA -> BAR/device pages -> host VA 映射

解析 safetensors metadata
  |
构造 phxfs_io_req_t[]
  |
phxfs_batch_submit_read()
  |-- 批量查注册区间，计算 host_addr
  |-- 持有设备与 mapping 引用
  `-- 提交 NUMA worker pool
        `-- io_uring prep_read
              `-- Linux FS / block layer
                    `-- NVMe DMA 到 P2P device pages
                          `-- 数据落入 GPU buffer

phxfs_batch_wait()
  |-- 收割 CQE
  |-- 回填每个 req.result
  `-- 释放 mapping/device 引用

GPU kernel 直接消费权重
```

Staging 模式只是在 I/O 落点后增加：

```text
Phoenix GPU staging buffer -> cudaMemcpy D2D -> 用户 GPU buffer
```

## 总结

Phoenix 并不是简单复刻一组 `cuFile` API。它的核心思想是重新划分 GDS 的职责：

- 内核模块负责把 GPU BAR/显存接入 Linux 页与 P2P DMA 模型；
- 用户态库负责注册表、地址转换、生命周期与 I/O 调度；
- Linux 原生 `pread/pwrite/io_uring` 继续负责文件访问；
- 适配器只负责把框架对象翻译成文件 offset、GPU offset 和长度。

从源码看，它最有价值的贡献不是某个单独的性能技巧，而是一种可组合的系统结构。尤其是 full 与 staging 两种模式，清楚展示了高性能系统软件常见的现实选择：最短数据路径和生态兼容性往往不能同时最大化。

如果后续能补齐多厂商后端、DKMS/发行包、跨内核回归测试、超大内存注册，以及对更多文件系统和拓扑的可重复 benchmark，Phoenix 有机会从研究型 GDS 重构项目，成长为 AI 存储到 xPU 数据面的通用中间层。

## 参考资料

- [xPU-IO/Phoenix GitHub 仓库](https://github.com/xPU-IO/phoenix)
- [Phoenix 架构文档](https://github.com/xPU-IO/phoenix/blob/main/doc/architecture.md)
- [Phoenix 内核模块文档](https://github.com/xPU-IO/phoenix/blob/main/doc/kernel-module.md)
- [Phoenix 用户态库文档](https://github.com/xPU-IO/phoenix/blob/main/doc/libphoenix.md)
- [Phoenix 应用适配器文档](https://github.com/xPU-IO/phoenix/blob/main/doc/adapters.md)
- [Linux PCI Peer-to-Peer DMA Support](https://docs.kernel.org/driver-api/pci/p2pdma.html)
