---
title: "nvidia-fs 源码阅读（一）：模块初始化、设备接口与整体架构"
date: 2026-08-21T10:45:00+08:00
draft: false
tags: ["NVIDIA", "GPUDirect Storage", "GDS", "nvidia-fs", "Linux 内核", "源码阅读"]
categories: ["技术"]
---

`nvidia-fs` 是 NVIDIA GPUDirect Storage（GDS）的 Linux 内核模块。它不是一种文件系统，而是连接 `libcufile`、NVIDIA GPU 驱动、Linux 文件 I/O 和支持 GPUDirect 的存储驱动的一层内核协调组件。

这组文章阅读 NVIDIA 官方 `gds-nvidia-fs` 仓库，采用的版本为：

```text
commit: 328d1d8cce1175c013720985c30e123e9a35242c
GDS_VERSION: 2.29.4
commit date: 2026-06-01
```

系列分为三篇：

1. 模块初始化、设备接口与整体架构；
2. GPU 内存注册、页表与 DMA 映射；
3. 文件 I/O、批量请求、完成路径与可观测性。

本文先回答一个问题：加载 `nvidia_fs.ko` 后，内核里究竟多了什么？

## 一、源码目录

核心代码都位于 `src/`：

| 文件 | 职责 |
|---|---|
| `nvfs-core.c/.h` | 字符设备、ioctl、GPU 内存注册和文件 I/O 主流程 |
| `nvfs-mod.c` | 与 NVMe、RDMA 等外部模块动态注册 DMA 回调 |
| `nvfs-mmap.c/.h` | GPU 页对应的影子 `struct page` 和 mmap 管理 |
| `nvfs-dma.c/.h` | block request 到 GPU DMA 地址的映射 |
| `nvfs-batch.c/.h` | 批量 I/O 提交与完成 |
| `nvfs-rdma.c/.h` | RDMA 注册信息管理 |
| `nvfs-pci.c/.h` | GPU 与存储设备的 PCIe 拓扑、距离和亲和性 |
| `nvfs-proc.c`、`nvfs-stat.c` | `/proc` 配置、统计和诊断接口 |
| `nvfs-kernel-interface.c` | 不同 Linux 内核版本的兼容封装 |

主线集中在 `nvfs-core.c`，但真正的数据直达能力是多个文件共同完成的。

## 二、整体架构

源码展示的路径可以抽象为：

```text
应用程序
  │ cuFile API
  ▼
libcufile.so（用户态，未包含在本仓库）
  │ open / ioctl / mmap
  ▼
/dev/nvidia-fs
  │
  ├── nvfs-core：注册 GPU buffer、构造文件 I/O
  ├── nvfs-mmap：创建代表 GPU 内存的影子 page
  ├── nvfs-dma：把 block request 映射到 GPU DMA 地址
  ├── nvfs-pci：选择和统计 GPU—存储设备路径
  └── nvfs-proc/stat：配置与诊断
  │
  ├── NVIDIA GPU 驱动的 P2P API
  └── NVMe / RDMA / 文件系统扩展接口
```

这里最关键的设计不是“在内核里重新实现一个文件系统”，而是让现有 Linux I/O 栈能够携带一种特殊页面：它看起来是 `struct page`，实际背后对应 GPU 显存。

## 三、模块入口 `nvfs_init`

模块入口位于 `nvfs-core.c`：

```c
module_init(nvfs_init);
module_exit(nvfs_exit);
```

`nvfs_init()` 的工作可以归纳为五步：

```text
1. 检查运行环境和模块参数
2. 注册字符设备主设备号
3. 创建 nvidia-fs class 与设备节点
4. 初始化 /proc、统计、PCI 与 DMA 子系统
5. 探测并连接支持 nvfs 扩展的外部驱动
```

设备节点由 `register_chrdev`、`class_create` 和 `device_create` 这一组典型字符设备 API 建立，用户态 `libcufile` 随后可通过 `/dev/nvidia-fs` 与模块通信。

设备权限由 `nvfs_devnode()` 设置。这说明 `nvidia-fs` 对用户态暴露的核心形态是字符设备控制面，而不是一个可挂载文件系统。

初始化还会调用 `probe_module_list()`。该函数位于 `nvfs-mod.c`，通过 `__symbol_get()` 动态查找外部模块导出的注册与注销函数，然后把 `nvfs_dma_rw_ops` 或新版 iterator ops 注册进去。

这样设计的优点是：

- `nvidia-fs` 不需要静态依赖每一种存储驱动；
- NVMe、RDMA 或厂商文件系统可以按约定接入；
- 模块加载顺序变化时可以重新探测；
- 卸载时能够成对注销回调，避免留下悬空函数指针。

换句话说，`nvidia-fs` 同时扮演消费者和服务者：向 GPU 驱动消费 P2P 页表能力，又向存储侧提供 GPU 页面识别及 DMA 映射能力。

## 四、字符设备接口

核心 `file_operations` 包含：

```text
open       -> nvfs_open
release    -> nvfs_close
unlocked_ioctl -> nvfs_ioctl
compat_ioctl   -> nvfs_ioctl（按构建条件）
mmap       -> nvfs_mmap
```

`nvfs_open()` 会为进程建立与文件实例相关的状态，并增加活跃操作计数；`nvfs_close()` 负责释放该进程尚未清理的 GPU 映射。

真正的控制中心是 `nvfs_ioctl()`。公开命令定义在 `nvfs-core.h`：

```text
NVFS_IOCTL_REMOVE
NVFS_IOCTL_READ
NVFS_IOCTL_MAP
NVFS_IOCTL_WRITE
NVFS_IOCTL_SET_RDMA_REG_INFO
NVFS_IOCTL_GET_RDMA_REG_INFO
NVFS_IOCTL_CLEAR_RDMA_REG_INFO
NVFS_IOCTL_BATCH_IO（按构建配置）
```

它们大致分为三组：

- **内存控制**：`MAP` 和 `REMOVE`；
- **文件 I/O**：`READ`、`WRITE` 和 `BATCH_IO`；
- **RDMA 元数据**：设置、查询和清理注册信息。

用户态参数先统一复制到 `nvfs_ioctl_param_union`，再按命令解释为 map、I/O、batch 或 RDMA 参数。这种 union ABI 可以保持设备接口集中，但也意味着用户态库和内核模块必须严格匹配结构布局与版本。

## 五、一次典型调用如何进入内核

以应用调用 `cuFileBufRegister` 和 `cuFileRead` 为例，概念调用链为：

```text
cuFileBufRegister
  -> libcufile 打开 /dev/nvidia-fs
  -> NVFS_IOCTL_MAP
  -> nvfs_map
  -> nvfs_map_gpu_info
  -> nvfs_pin_gpu_pages
  -> 建立 GPU page table 与影子 page

cuFileRead
  -> NVFS_IOCTL_READ
  -> nvfs_io_init
  -> nvfs_io_start_op
  -> Linux 文件异步读入口
  -> block / filesystem 路径识别影子 page
  -> nvfs DMA 映射回调得到 GPU DMA 地址
  -> 存储设备向 GPU 显存传输
  -> nvfs_io_complete
```

需要注意，公开仓库不包含 `libcufile` 的实现。因此我们能确认内核 ABI 和执行路径，但不能仅凭这个仓库还原 `cuFile` 用户态的全部策略，例如兼容模式选择、分块和某些回退逻辑。

## 六、状态机与并发安全

GPU buffer 的生命周期不是一个简单布尔值。`nvfs-core.c` 使用状态转换和原子操作协调：

- 正常 I/O；
- GPU 驱动触发 free callback；
- 用户主动注销；
- 进程退出；
- 模块卸载；
- 尚未完成的异步 I/O。

`nvfs_transit_state()`、`nvfs_io_terminate_requested()`、`nvfs_io_terminate()` 和 `nvfs_free_gpu_info()` 是理解并发释放的关键函数。

难点在于 GPU 页可能仍被设备 DMA 使用。释放流程不能只删除哈希表条目，还要：

1. 阻止新 I/O 获取该映射；
2. 通知或等待正在执行的 I/O；
3. 释放 peer DMA mapping；
4. 归还 NVIDIA P2P page table；
5. 清理影子 page 和元数据页；
6. 最后释放 `nvfs_gpu_args`。

这种“注销”和“异步完成”竞态，是整个驱动里比 ioctl 分发更值得关注的部分。

## 七、模块退出

`nvfs_exit()` 基本按初始化的逆序执行：

```text
设置 shutdown 标志
  -> 阻止新请求
  -> 等待或终止活跃操作
  -> 注销外部 DMA 回调
  -> 清理 proc/stat/PCI 子系统
  -> 销毁设备和 class
  -> 注销字符设备
```

动态符号通过 `__symbol_put()` 归还。注册函数和注销函数必须成对存在，`probe_module_list()` 对不完整的符号对直接拒绝接入，避免模块卸载时出现不可恢复的不一致状态。

## 八、这一层究竟负责什么

读完入口代码后，可以更准确地定义 `nvidia-fs`：

> 它是一层 GPU 内存到 Linux 文件 I/O 的适配与协调驱动，通过字符设备提供控制面，通过影子 page 嵌入现有 I/O 栈，并通过动态注册接口让存储驱动获取正确的 GPU DMA 地址。

它不负责：

- 实现 `cuFile` 的完整用户态 API；
- 替代 ext4、XFS、NFS 或并行文件系统；
- 自己驱动 NVMe 控制器；
- 绕过 Linux 内核的全部控制路径。

它真正减少的是数据经过 CPU 内存 bounce buffer 的必要性，CPU 仍然参与系统调用、I/O 提交和完成处理。

下一篇将进入最核心的数据结构：`nvfs_gpu_args`、`nvfs_mgroup`、NVIDIA P2P page table，以及为什么驱动需要为 GPU 显存创建“影子 `struct page`”。

## 参考资料

- NVIDIA `gds-nvidia-fs` 官方仓库，commit `328d1d8cce1175c013720985c30e123e9a35242c`
- `README.md`
- `src/nvfs-core.c`
- `src/nvfs-core.h`
- `src/nvfs-mod.c`
