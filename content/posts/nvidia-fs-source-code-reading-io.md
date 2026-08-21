---
title: "nvidia-fs 源码阅读（三）：文件 I/O、批处理、完成路径与诊断"
date: 2026-08-21T10:47:00+08:00
draft: false
tags: ["NVIDIA", "GPUDirect Storage", "GDS", "nvidia-fs", "Linux I/O", "源码阅读"]
categories: ["技术"]
---

前两篇分别介绍了 `nvidia-fs` 的模块结构，以及 GPU virtual address 到 peer DMA address 的映射。本文沿一次 `cuFileRead` 对应的内核路径，分析文件 I/O 如何提交、完成和清理，并介绍 batch、稀疏文件、RDMA 与 `/proc` 诊断接口。

阅读版本：

```text
commit: 328d1d8cce1175c013720985c30e123e9a35242c
GDS_VERSION: 2.29.4
```

## 一、I/O 入口

`nvfs_ioctl()` 接收：

```text
NVFS_IOCTL_READ
NVFS_IOCTL_WRITE
NVFS_IOCTL_BATCH_IO
```

单次读写共用两阶段结构：

```text
nvfs_io_init(op, ioargs)
  -> 验证并构造 nvfs_io

nvfs_io_start_op(nvfsio)
  -> 向目标文件提交真正 I/O
```

分成两步的意义在于：参数验证、对象引用和资源分配都应在进入异步 I/O 前完成。一旦请求提交，完成回调可能很快发生，初始化不完整会造成竞态。

## 二、`nvfs_io_init` 做了什么

`nvfs_io_init()` 是用户参数到内核 I/O 对象的转换层，主要工作包括：

1. 根据文件描述符取得目标 `struct file`；
2. 检查读写权限；
3. 查找已经注册的 GPU buffer/mgroup；
4. 验证 GPU buffer offset、文件 offset 和长度；
5. 建立影子 page 对应的 iov 或迭代器；
6. 初始化 `kiocb`、完成函数和统计字段；
7. 为同步、异步和特殊文件系统路径设置标志。

源码还会检查文件系统类型、direct I/O 条件和文件权限。写操作比读操作更复杂，因为它可能涉及文件扩展、页缓存一致性及磁盘空间预分配。

一个重要边界是：`nvidia-fs` 处理的是已经由用户态打开的文件描述符。路径解析、权限主体和 open flag 仍走标准 Linux VFS；模块不会绕过文件系统安全模型去访问裸文件。

## 三、提交到 Linux 文件 I/O

`nvfs_io_start_op()` 根据内核版本和文件能力选择相应读写入口。代码使用 `kiocb` 与 iterator 风格接口，使请求能够异步完成。

概念路径如下：

```text
nvfs_io_start_op
  -> rw_verify_area / permission checks
  -> file_start_write（写路径）
  -> call_read_iter 或 call_write_iter
  -> 文件系统 direct-I/O
  -> bio / request
  -> nvfs-dma 提供 GPU DMA addresses
```

如果底层立即完成，返回值就是已处理字节数；如果返回异步排队状态，最终由 `nvfs_io_complete()` 收尾。

驱动封装了不同 Linux 内核版本的 API 差异，相关兼容代码集中在 `nvfs-kernel-interface.c/.h` 和 `configure` 生成的特性宏中。这也是该仓库需要 DKMS 构建而不是提供一个永远不变的二进制模块的重要原因。

## 四、同步与异步完成

`nvfs_io_complete()` 是完成路径中心，负责：

- 记录结果与实际字节数；
- 更新 read/write 成功、错误、延迟和带宽统计；
- 处理稀疏读元数据；
- 释放文件和 GPU mapping 引用；
- 唤醒等待方或写入完成栅栏；
- 最终调用 `nvfs_io_free()`。

源码通过 `nvfs_ioctl_metapage` 在共享元数据页中维护：

```text
end_fence_val
result
state
sparse_data
```

这让用户态可以观察异步操作状态，而不必让 GPU 数据本身承担控制信息。

完成路径必须正确处理短读、短写、错误和被终止状态。不能把“回调被调用”等同于“全部请求字节成功完成”。统计代码也分别记录请求数、成功数、错误数、MiB、平均延迟和状态错误。

## 五、I/O 生命周期状态机

一次 I/O 同时引用：

- 目标文件；
- `nvfs_io` 对象；
- GPU buffer/mgroup；
- 可能存在的 peer DMA mapping；
- 元数据或 end-fence page。

因此异常路径不能随意 `kfree`。源码通过 active operation 计数和 GPU mapping 状态协调以下事件：

```text
I/O 正常完成
用户注销 GPU buffer
进程关闭 /dev/nvidia-fs
GPU 驱动触发 free callback
模块开始卸载
```

`nvfs_io_ret()` 统一解释同步返回和异步排队结果，`nvfs_io_free()` 统一归还引用。`nvfs_io_terminate_requested()` 则让正在执行的路径感知 mapping 已进入终止阶段。

这里体现了一条内核驱动通用原则：资源释放的真正时点由最后一个引用决定，而不是由第一个“我要删除”请求决定。

## 六、Batch I/O

启用 `NVFS_BATCH_SUPPORT` 时，`NVFS_IOCTL_BATCH_IO` 进入 `nvfs-batch.c`：

```text
nvfs_io_batch_init
  -> 复制并验证多个 I/O 描述
  -> 为每项调用 nvfs_io_init
  -> 创建 batch 状态

nvfs_io_batch_submit
  -> 逐项提交
  -> 汇总完成和错误
  -> 更新 batch 统计与延迟
```

批处理并不意味着把多个文件请求强行合并成一个 block request。它主要减少用户态到内核态的控制开销，并以统一对象跟踪一组子 I/O。

失败处理必须区分：

- batch 初始化阶段失败，一个请求都没有提交；
- 部分子请求已经提交，后续项失败；
- 所有请求都提交，但某些异步完成返回错误。

因此 batch 对象需要自己的完成计数和生命周期，不能在 `ioctl` 返回后立即释放。

## 七、稀疏文件读取

源码定义 `nvfs_io_sparse_data` 和最多若干 hole region。稀疏文件的 hole 不对应真实磁盘块，但读取语义要求返回零。

普通 CPU buffer 可以由内核清零；GPU direct path 则必须明确告诉上层哪些区域是洞，或者通过受支持路径完成相应处理。

相关流程使用：

```text
nvfs_io_map_sparse_data
nvfs_io_unmap_sparse_data
nvfs_io_hole
```

元数据记录起始文件 offset、hole 数量和每段页范围。统计接口还单独输出 sparse read 次数、I/O 数、hole 数和页数。

这说明“存储直接 DMA 到 GPU”不等于可以忽略文件语义。稀疏区、EOF、短读和文件扩展仍需要上层与文件系统协同处理。

## 八、写路径与文件一致性

写路径会检查目标文件权限，并在必要时处理文件增长。源码中的 `nvfs_need_fallocate()` 反映了某些写入场景需要提前建立空间映射，避免 direct I/O 过程中进入不适合的元数据分配路径。

同时，写路径还统计 page-cache 相关结果。GDS 主要追求 direct path，但文件可能已存在缓存页，文件系统必须维持缓存与磁盘内容一致性。

因此部署中常见的 `O_DIRECT`、文件系统 mount mode 和对齐要求不是纯性能建议，而是为了让 I/O 语义落在驱动明确支持的路径上。

## 九、RDMA 支持

`nvfs-rdma.c` 管理写入 GPU mapping 对象的 RDMA 注册信息，对应 ioctl 包括：

```text
SET_RDMA_REG_INFO
GET_RDMA_REG_INFO
CLEAR_RDMA_REG_INFO
```

结构中可以保存 queue pair、LID、rkey 等信息，用于支持 NFS over RDMA 或具备 PeerDirect 能力的分布式存储路径。

这部分再次说明，GDS 不只有本地 NVMe 模式：

```text
本地块设备：block request -> peer DMA mapping
远程存储：RDMA NIC -> 已注册 GPU memory / peer direct path
```

二者共享 GPU buffer 生命周期管理，但设备和传输协议不同。

## 十、`/proc` 可观测性

模块创建多组诊断节点。最常用的是：

```bash
cat /proc/driver/nvidia-fs/version
cat /proc/driver/nvidia-fs/peer_affinity
cat /proc/driver/nvidia-fs/peer_distance
cat /proc/fs/nvfs/stats
```

`/proc/fs/nvfs/stats` 会输出：

- GDS 和 NVFS 驱动版本；
- Mellanox PeerDirect 支持状态；
- read/write 和 peer I/O 统计开关；
- active process 与 shadow buffer；
- batch 数量和平均提交延迟；
- 读写次数、MiB、带宽和平均延迟；
- sparse read；
- mmap、BAR1 mapping 和 callback；
- CPU/GPU page 混合、SG 扩展、DMA mapping 等错误；
- 当前 read/write/batch active ops。

向 stats 节点写入可触发统计重置。实际排障时应先保留现场数据，再决定是否清零。

## 十一、如何判断是否真的走 direct path

仅看到应用调用 `cuFileRead` 并不够。建议组合检查：

```bash
lsmod | grep nvidia_fs
cat /proc/driver/nvidia-fs/version
cat /proc/fs/nvfs/stats
lspci -tv
```

重点观察：

1. `nvidia_fs` 是否加载且版本匹配；
2. read/write 数量和 MiB 是否随测试增长；
3. DMA mapping、mixed CPU/GPU page 等错误是否增长；
4. peer affinity 是否符合 GPU 与 NVMe/NIC 的物理拓扑；
5. `libcufile` 是否发生兼容模式回退；
6. 文件系统、驱动和 mount 参数是否在当前版本支持矩阵内。

`nvidia-fs` 统计只能证明内核侧发生了什么，完整判断仍应结合 `libcufile` 日志、设备性能计数和系统拓扑。

## 十二、三篇源码阅读总结

从整个仓库看，`nvidia-fs` 的核心设计可以压缩成四句话：

1. 用字符设备和 ioctl 接收 `libcufile` 控制请求；
2. 用 NVIDIA P2P API pin GPU pages，并创建 Linux 可携带的影子 pages；
3. 在存储 DMA mapping 阶段把影子 page 转回 peer-specific GPU DMA address；
4. 用状态机、引用计数和完成回调保证异步 I/O 与 GPU 内存释放不冲突。

所以，GDS 的“直接”主要发生在数据面：存储设备与 GPU 显存之间减少 CPU bounce buffer。控制面仍然经过用户态库、系统调用、VFS、文件系统和设备驱动。

这也解释了为什么 `nvidia-fs` 代码量不算巨大，却横跨 GPU 内存、Linux VM、VFS、block layer、PCIe、RDMA 和异步生命周期。它不是一个完整存储系统，而是一块高度耦合的内核适配层。

## 参考资料

- NVIDIA `gds-nvidia-fs` 官方仓库，commit `328d1d8cce1175c013720985c30e123e9a35242c`
- `src/nvfs-core.c`、`src/nvfs-core.h`
- `src/nvfs-batch.c`
- `src/nvfs-rdma.c`
- `src/nvfs-proc.c`
- `src/nvfs-stat.c`
- `src/nvfs-kernel-interface.c`
