---
title: "NUMA 是什么？从内存访问距离到 LMCache 的实际用法"
date: 2026-09-10T23:35:00+08:00
draft: false
description: "理解 NUMA 的本地与远端内存、CPU 绑核和 pinned memory 的区别，并沿源码检查 LMCache 哪些路径真正传递了 NUMA 映射。"
tags: ["NUMA", "LMCache", "KV Cache", "内存管理", "性能优化", "源码解析"]
categories: ["技术"]
---

读 LMCache 的内存分配代码时，很容易遇到一个问题：构造函数明明有 `numa_mapping` 参数，为什么运行时一直是 `None`？回答它之前，需要先区分三件事：**内存位于哪个 NUMA 节点、线程运行在哪些 CPU 上，以及主机内存是否经过 GPU 注册。** 它们彼此相关，却不是同一个开关。

本文的源码结论固定在 LMCache 公开提交 `fd6722d9310471bc7d1871969ef3062889878632`，核对日期为 2026 年 9 月 10 日。本文是原理与代码分析，没有进行双路服务器性能实测，也不预设 NUMA 优化一定带来加速。

## 1. NUMA：访问同一份内存空间，距离却不一样

NUMA 是 Non-Uniform Memory Access，即非一致内存访问。这里的“非一致”说的是**访问代价不一致**，不是“不同 CPU 读到的数据不一致”。

Linux 内核文档把 NUMA 系统抽象为多个 node：节点可以包含 CPU、内存和 I/O 资源，节点之间通过互连通信。对某个 CPU 或 I/O 设备来说，目标内存所在节点的距离会影响访问延迟和有效带宽。常见的双路服务器可以简化成下面这样，但真实机器不能直接假定“一个 CPU 插槽等于一个 NUMA 节点”。

```text
NUMA node 0                         NUMA node 1
CPU cores + DRAM 0                  CPU cores + DRAM 1
        |                                  |
        +---------- interconnect ----------+
        |                                  |
    PCIe / GPU 0                       PCIe / GPU 1

CPU 0 -> DRAM 0: local access
CPU 0 -> DRAM 1: remote access
GPU 0 -> DRAM 1: potentially crosses the interconnect
```

这是一张示意图，不是某台机器的真实拓扑。实际 GPU、PCIe 交换机和内存控制器的关系，应以系统和设备工具报告为准。

NUMA 的硬件价值在于扩展处理器和内存带宽；软件优化的目标，是尽量让主要访问发生在更近的节点，减少跨节点流量。它不会增加内存容量，也不保证所有“本地”访问都比所有“远端”访问快：节点负载、容量压力和共享数据分布同样重要。[Linux NUMA 文档](https://docs.kernel.org/mm/numa.html)对此有更完整的解释。

## 2. 为什么只绑 CPU，还不够？

对于常见的匿名内存，在默认策略下，物理页的位置往往与首次实际触发页面分配的执行位置有关，而不是只看调用 `malloc` 或 `torch.empty` 的那一刻。线程之后迁移到别的节点，已经分配的页面也不保证立即跟着迁移；显式内存策略、自动 NUMA balancing、容器限制等还会改变行为。

因此需要分别考虑：

| 控制项 | 回答的问题 | 不代表什么 |
| --- | --- | --- |
| CPU affinity | 哪些 CPU 可以执行这个线程或进程？ | 不等于已有内存自动搬到对应节点 |
| NUMA memory policy | 内存页优先或限制从哪些节点分配？ | 不等于线程也被绑到相同节点 |
| GPU host-memory registration | 主机内存是否为 GPU 传输准备了注册条件？ | 不等于内存位于 GPU 最近的节点 |

CUDA pinned memory 与 NUMA 是两条不同维度。把一块远端内存注册为 pinned memory，不会仅因注册就把它搬到本地节点。反过来，分配在合适 NUMA 节点上的普通内存，也不自动等于已经完成 GPU 主机内存注册。

## 3. LMCache 为什么关心 NUMA？

考虑最直观的 CPU 缓存路径：计算得到的 KV 从 GPU 保存到 CPU，之后命中时再从 CPU 加载回 GPU。

```text
store:    GPU KV -> CPU cache
retrieve: CPU cache -> GPU KV
```

如果缓存页位于离目标 GPU 更远的节点，传输可能额外经过节点互连。此时即使缓存命中率很高，KV 搬运仍可能成为瓶颈；与其他 GPU 共享同一互连，还可能出现争用。

NUMA 放置的潜在收益，是减少不必要的远端流量，而不是改变缓存命中逻辑。对一个服务多个 GPU 的共享池，也不能只问“当前 GPU 最近的节点在哪里”：不同消费者可能靠近不同节点，单节点绑定未必是全局最优，必要时应评估分池或不同内存策略。

这些是基于数据路径的性能假设，需要测量验证，不能直接换算成固定的 TTFT 提升百分比。

## 4. LMCache 如何生成 NUMA 映射？

在 `lmcache/v1/system_detection.py` 中，`NUMAMapping` 包装了一个 `gpu_to_numa_mapping` 字典。下面是概念示例，不应不加检查地复制到实际机器：

```python
mapping = NUMAMapping(
    gpu_to_numa_mapping={
        0: 0,
        1: 0,
        2: 1,
        3: 1,
    }
)
```

含义是 GPU 0、1 对应节点 0，GPU 2、3 对应节点 1。这里必须确认设备编号与**进程内使用的设备编号**一致，不能把经过可见设备重映射后的编号与机箱物理序号混用。

`NUMADetector.get_numa_mapping(config)` 根据 `LMCacheEngineConfig.numa_mode` 选择来源：

| numa_mode | 获取方式 | 当前实现的结果 |
| --- | --- | --- |
| 默认 None | 不进行探测 | 返回 None |
| manual | 读取 extra_config 中的 gpu_to_numa_mapping | 包装为 NUMAMapping；缺少必要配置触发断言 |
| auto | 当前 GPU → PCI bus ID → 系统 numa_node 文件 | 成功返回当前 GPU 的映射；查询失败返回 None |

手动配置示例：

```yaml
numa_mode: manual
extra_config:
  gpu_to_numa_mapping:
    0: 0
    1: 0
```

自动探测读取的是 `/sys/bus/pci/devices/<PCI_BUS_ID>/numa_node`。固定快照中，它只查询当前 GPU，并不一次建立全机 GPU 映射；读到整数后也没有进一步验证节点值。因此，返回非空对象不等于所有节点信息都有效，例如系统报告 `-1` 时就需要额外处理。

## 5. 关键区别：分配器支持 NUMA，不等于调用方已经接线

### LocalCPUBackend：存在探测和传参路径

`lmcache/v1/storage_backend/local_cpu_backend.py` 会调用 `NUMADetector.get_numa_mapping(config)`，并在相应分支中把结果传给 `MixedMemoryAllocator`。对于不被其他分配模式覆盖的 NUMA 路径，映射会继续参与主机内存分配选择。

在本文查看的 CUDA 原生实现 `csrc/cuda/mem_alloc.cpp` 中，NUMA 分配路径包含 `mmap`、`mbind(..., MPOL_BIND, ...)` 和页面触碰；pinned NUMA 路径随后调用 `cudaHostRegister`。这也从代码层面说明：**节点放置和 GPU 注册是不同步骤。** 绑定失败会抛出异常，不应假定所有运行环境都具备相同权限与能力。

### LazyMemoryAllocator：有参数，但 distributed L1 工厂没有传

`LazyMemoryAllocator` 的接口接受 `numa_mapping`，默认值为 `None`。传入映射时，它根据当前 GPU 查出节点，调用 `device_ops.alloc_numa_ptr()`，随后再处理内存注册；不传时走普通 CPU tensor 缓冲区路径。

然而，在本文固定快照中，`lmcache/v1/distributed/memory_manager/l1_memory_manager.py` 的工厂实际只传入三个参数：

```python
return LazyMemoryAllocator(
    config.init_size_in_bytes,
    config.size_in_bytes,
    config.align_bytes,
)
```

于是这条调用链是：

```text
L1Manager
    -> L1MemoryManager
        -> create_memory_allocator(config)
            -> LazyMemoryAllocator(initial, final, alignment)
                -> numa_mapping defaults to None
```

这个 `L1MemoryManagerConfig` 也没有与 `LMCacheEngineConfig.numa_mode` 对应的探测、转发流程。因此，**仅在另一套引擎配置里设置 `numa_mode: auto`，不能让该 distributed Lazy L1 工厂自动获得映射。** 调试时看到 `None`，首先应检查调用方有没有传参，而不是立即认定机器没有 NUMA。

这只是针对固定源码路径的结论，不代表 LMCache 所有后端都不支持 NUMA。即使参数是 `None`，分配仍可能受进程启动时的操作系统内存策略影响；准确说法是“没有通过这个参数显式选择节点”，而不是“操作系统完全没有 NUMA 策略”。

## 6. 如何确认配置真的有用？

先确认机器和进程条件。下面的命令用于 Linux；`numactl`、`numastat` 需要相应工具包，`nvidia-smi` 仅适用于对应 NVIDIA 环境：

```bash
lscpu
numactl --hardware
nvidia-smi topo -m
numastat -p <PID>
cat /proc/<PID>/numa_maps
```

`nvidia-smi topo -m` 用来结合 GPU 连接与 NUMA affinity 理解拓扑；`numastat` 和 `numa_maps` 用来观察页面放置，**它们本身不是 GPU 传输带宽测试**。不要仅凭一行配置或一个非空映射判定优化已经生效。

若要做操作系统层面的绑定对照，可在确认节点编号后，用类似下面的启动模板。`<service-command>` 是占位符，需要替换；它不是建议在任意机器上盲目绑定节点 0：

```bash
numactl --cpunodebind=0 --membind=0 <service-command>
```

`--membind` 是限制性策略，目标节点内存不足时可能分配失败；它也不会自动重新放置某个旧进程已经创建的共享池。应用自己的内存策略和容器允许的节点范围还需一并检查。

一次有意义的 LMCache 对照实验，可以按下面的顺序进行：

1. 固定模型、GPU、请求集合、缓存容量和冷/热缓存条件，先记录实际使用的 adapter 与 allocator。
2. 比较默认策略与经过拓扑确认的 CPU/内存绑定，检查两组实际页面分布，避免只比较配置文件。
3. 测量 KV 保存、加载时间与传输带宽，再看 TTFT、吞吐和尾延迟，并观察跨节点流量及各节点内存压力。
4. 重复运行，确认收益不是首次初始化、缓存命中率变化或其他 GPU 负载造成的；明确哪些指标没有改善。

对于后台扩容的 LazyMemoryAllocator，还应让两组达到可比的已提交容量，避免把初始化进度差异误当作 NUMA 收益。真正接入 NUMA 分配后，页面触碰行为也可能改变初始化成本，需要单独测量。

## 小结与源码入口

NUMA 优化的核心是让访问者与内存的拓扑关系更合理，而不是看到 pinned memory 就认定已经优化。对 LMCache，先追踪“配置 → 映射 → 分配器 → 原生分配”的完整链路，再用实际页面分布和传输指标验证。**支持参数不等于参数已被传入，配置存在也不等于当前运行路径会消费它。**

进一步阅读：

- [Linux NUMA 原理](https://docs.kernel.org/mm/numa.html)与[内存策略](https://docs.kernel.org/admin-guide/mm/numa_memory_policy.html)。
- [numactl 官方手册源码](https://github.com/numactl/numactl/blob/master/numactl.8)与[NVIDIA 拓扑查询文档](https://docs.nvidia.com/deploy/nvidia-smi/index.html)。
- [LMCache NUMA 探测器](https://github.com/LMCache/LMCache/blob/fd6722d9310471bc7d1871969ef3062889878632/lmcache/v1/system_detection.py)与[LocalCPUBackend](https://github.com/LMCache/LMCache/blob/fd6722d9310471bc7d1871969ef3062889878632/lmcache/v1/storage_backend/local_cpu_backend.py)。
- [distributed L1 分配器工厂](https://github.com/LMCache/LMCache/blob/fd6722d9310471bc7d1871969ef3062889878632/lmcache/v1/distributed/memory_manager/l1_memory_manager.py)、[LazyMemoryAllocator](https://github.com/LMCache/LMCache/blob/fd6722d9310471bc7d1871969ef3062889878632/lmcache/v1/memory_allocators/lazy_memory_allocator.py)与[CUDA NUMA 分配实现](https://github.com/LMCache/LMCache/blob/fd6722d9310471bc7d1871969ef3062889878632/csrc/cuda/mem_alloc.cpp)。
- 延伸阅读：[LMCache LazyMemoryAllocator 源码解析](https://yangyang233333.github.io/posts/lmcache-lazy-memory-allocator/)。
