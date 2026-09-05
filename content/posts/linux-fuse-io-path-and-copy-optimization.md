---
title: "Linux FUSE 原理与 I/O 链路：控制面、数据面、复制次数及优化"
date: 2026-09-05T20:40:00+08:00
draft: false
tags: ["Linux", "FUSE", "文件系统", "I/O", "零拷贝"]
categories: ["存储系统"]
summary: "从 VFS、fuse.ko、/dev/fuse 和用户态守护进程出发，逐跳分析 FUSE 控制面与数据面链路，并解释 read/write 路径中的复制次数及 page cache、splice、DAX、passthrough 等优化。"
---

FUSE（Filesystem in Userspace）允许开发者把文件系统的主要逻辑放在用户态实现，而不必把全部代码写进 Linux 内核。SSHFS、对象存储挂载、加密文件系统、容器文件系统和不少分布式存储客户端都采用过这种模式。

它的价值是开发和隔离更简单，代价则是：**一次文件操作可能需要在应用、内核 FUSE 模块和用户态文件系统进程之间往返，并伴随调度、协议编码和内存复制。**

理解 FUSE 性能，不能只问“一次 I/O 复制几次”。必须先区分：

- 控制面还是数据面；
- 读还是写；
- 命中页缓存还是需要请求用户态；
- buffered I/O、direct I/O 还是 DAX；
- 用户态后端是磁盘、网络还是另一份内存；
- 是否使用 `splice`、共享映射或 passthrough。

本文先解释整体架构，再分别展开控制面和数据面链路，最后讨论复制次数与优化方法。

# 一、FUSE 的整体架构

典型 FUSE 文件系统包含四个角色：

```text
┌──────────────────────────────────────────────┐
│ 应用进程                                     │
│ open / stat / read / write / fsync / mmap    │
└───────────────────┬──────────────────────────┘
                    │ 系统调用
                    ▼
┌──────────────────────────────────────────────┐
│ Linux VFS + Page Cache                       │
└───────────────────┬──────────────────────────┘
                    │ FUSE inode/file/address_space ops
                    ▼
┌──────────────────────────────────────────────┐
│ 内核模块 fuse.ko                             │
│ 请求排队、缓存、权限、拥塞控制、协议封装     │
└───────────────────┬──────────────────────────┘
                    │ /dev/fuse
                    ▼
┌──────────────────────────────────────────────┐
│ 用户态 FUSE daemon                           │
│ libfuse / 自研 runtime / 文件系统业务逻辑     │
└───────────────────┬──────────────────────────┘
                    │ 本地文件、网络、对象存储、数据库
                    ▼
┌──────────────────────────────────────────────┐
│ 后端存储                                     │
└──────────────────────────────────────────────┘
```

应用仍然调用标准 POSIX 接口。VFS 根据挂载点找到 FUSE inode，再由 `fuse.ko` 把操作编码成 FUSE 协议消息。用户态 daemon 从 `/dev/fuse` 读取请求，执行实际逻辑，再把回复写回 `/dev/fuse`。

这里有一个容易混淆的地方：

> `/dev/fuse` 不是用户文件数据最终保存的位置，而是内核 FUSE 驱动和用户态 daemon 之间传递请求、回复及数据的通信通道。

# 二、从挂载开始：控制通道如何建立

启动一个 FUSE 文件系统时，通常会经历以下过程：

1. 用户态程序打开 `/dev/fuse`，得到文件描述符；
2. 调用 `mount(2)` 挂载 `fuse` 或 `fuseblk` 文件系统，并把该文件描述符交给内核；
3. 内核创建 `fuse_conn`，建立挂载实例与 `/dev/fuse` 连接的关联；
4. 内核发送 `FUSE_INIT` 请求；
5. daemon 返回支持的协议版本、能力位和限制；
6. 双方协商最大读写长度、并发能力、缓存特性等参数；
7. daemon 进入循环，从 `/dev/fuse` 取请求并返回结果。

概念代码类似：

```c
for (;;) {
    request = read(fuse_fd, buffer, buffer_size);
    response = dispatch(request);
    write(fuse_fd, response.data, response.size);
}
```

实际 libfuse 会处理线程池、请求解析、回复编码以及 `read_buf` / `write_buf` 等更高效接口。

初始化阶段决定后续数据面能使用哪些能力，例如：

- 最大请求大小；
- 是否支持异步读取；
- 是否支持并行目录操作；
- 是否由内核执行权限检查；
- 是否启用 writeback cache；
- 是否支持 splice、DAX 或其他扩展。

# 三、控制面 I/O 链路

控制面主要处理命名空间和元数据，不以搬运大块文件内容为主。典型操作包括：

```text
LOOKUP  GETATTR  SETATTR
OPEN    RELEASE  FLUSH
CREATE  MKDIR    UNLINK
RENAME  LINK     SYMLINK
READDIR FSYNC    GETXATTR
```

以应用执行 `open("/mnt/a/b.txt", O_RDONLY)` 为例。

## 1. 路径解析

VFS 需要逐级解析路径：

```text
根 inode
  │ LOOKUP "a"
  ▼
a 的 inode
  │ LOOKUP "b.txt"
  ▼
b.txt 的 inode
```

如果 dentry 和 inode 属性缓存有效，部分 `LOOKUP`、`GETATTR` 可以直接在内核完成；缓存未命中或过期时，内核需要询问用户态 daemon。

一次未命中的 `LOOKUP` 链路是：

```text
应用 open()
   │
   ▼
VFS 路径解析
   │
   ▼
fuse_lookup()
   │ 创建 FUSE_LOOKUP 请求
   ▼
FUSE request queue
   │ 唤醒等待 /dev/fuse 的 daemon
   ▼
daemon read(/dev/fuse)
   │ 查询本地元数据或远端服务
   ▼
daemon write(/dev/fuse)
   │ 返回 nodeid、属性和缓存有效期
   ▼
fuse.ko 更新 dentry / inode cache
   │
   ▼
VFS 继续路径解析
```

对深路径而言，最坏情况下每一层都可能产生一次用户态往返。分布式后端还会在 daemon 内部再发起网络 RPC，因此元数据延迟可能被逐层放大。

## 2. `OPEN` 与文件句柄

路径解析完成后，内核可能发送 `FUSE_OPEN`。daemon 可以返回一个文件句柄 `fh`，后续 `READ`、`WRITE`、`FLUSH` 和 `RELEASE` 都携带该句柄，从而避免每次重新按路径查找后端对象。

```text
VFS file
   │
   ├── FUSE nodeid：标识 inode
   └── FUSE fh：标识一次打开实例或后端句柄
```

`OPEN` 还可以返回影响后续链路的标志，例如要求 direct I/O，或提示内核不要保留 page cache。

## 3. 控制面完成时机

请求进入 `/dev/fuse` 后，daemon 可能同步回复，也可能把请求提交到线程池或异步 runtime，稍后使用请求唯一编号回复。

内核依靠 `unique` 字段匹配请求和响应：

```text
FUSE request:  unique = 42
FUSE response: unique = 42
```

请求失败、超时或被信号打断时，还可能产生 `FUSE_INTERRUPT`。卸载、daemon 崩溃或连接关闭时，内核需要唤醒等待请求并返回错误，不能让应用永久挂起。

# 四、数据面读链路

数据面关注文件内容。先看普通 buffered read：

```c
read(fd, user_buffer, length);
```

## 1. 页缓存命中

如果目标数据已经在内核 page cache 中：

```text
FUSE daemon 不参与

Page Cache ── copy_to_user ──> 应用 buffer
```

这是 FUSE 最希望出现的路径。此时虽然文件由用户态文件系统管理，但热读可以被 Linux 页缓存直接吸收，不发生 `/dev/fuse` 往返。

一次普通 `read()` 通常仍需要从 page cache 复制到应用 buffer。若应用使用 `mmap()`，访问页缓存映射时可以避免这次显式 `copy_to_user`，但缺页和一致性路径另有成本。

## 2. 页缓存未命中

缓存未命中时，链路大致为：

```text
应用 read()
   │
   ▼
VFS / generic_file_read_iter
   │ 发现 page cache 缺页
   ▼
fuse_readpages / readahead
   │ 创建 FUSE_READ 请求
   ▼
用户态 daemon 从 /dev/fuse 取请求
   │
   ├── 读取本地后端文件
   ├── 请求远端存储
   └── 从对象缓存获得数据
   │
   ▼
daemon 把数据作为 FUSE_READ 回复
   │
   ▼
fuse.ko 填充 page cache
   │
   ▼
copy_to_user
   │
   ▼
应用 buffer
```

内核可将相邻页面合并成更大的读取请求，并通过 readahead 提前读取后续数据，从而减少往返次数。

## 3. Direct I/O 读取

启用 FUSE direct I/O 后，内核通常绕过 page cache：

```text
应用 buffer
   ▲
   │ FUSE_READ 回复数据
   │
用户态 daemon
   ▲
   │ /dev/fuse 请求
   │
fuse.ko
```

优点是避免双份缓存和页缓存一致性管理，适合数据库或应用自带缓存的场景；缺点是每次读取通常都要进入 daemon，失去 page cache 命中和通用 readahead 的收益。

Direct I/O 不是自动零拷贝，它只是改变缓存路径。数据怎样从 daemon 到应用 buffer，仍取决于内核版本、请求形式和 daemon 是否使用高效 buffer 接口。

# 五、数据面写链路

写路径要区分 write-through 与 writeback cache。

## 1. 普通同步写入思路

不使用 writeback cache 时，一次写入大致为：

```text
应用 write()
   │ copy_from_user
   ▼
内核 FUSE 请求页
   │ FUSE_WRITE
   ▼
daemon read(/dev/fuse)
   │ 得到请求头和文件数据
   ▼
写入本地文件 / 网络 / 对象存储
   │
   ▼
daemon 回复写入长度或错误
   │
   ▼
应用 write() 返回
```

从应用视角看，写入是否已经持久化仍取决于文件系统语义。`write()` 成功通常不等于介质已经落盘，`fsync()` 才用于请求更强的持久化保证。

## 2. Writeback cache

启用 writeback cache 后，应用写入先进入内核 page cache：

```text
应用 write()
   │ copy_from_user
   ▼
脏页进入 Page Cache
   │ write() 可以较早返回
   ▼
后台回写线程聚合脏页
   │ FUSE_WRITE
   ▼
daemon 批量写后端
```

它的收益包括：

- 合并多个小写；
- 延迟和批量回写；
- 改善随机小写吞吐；
- 让重复覆盖同一页面的写入只回写最终内容。

代价是缓存一致性和错误语义更复杂。daemon 必须正确处理内核可能发出的读改写、脏页回写、`flush`、`fsync` 和文件截断。

## 3. `fsync` 链路

```text
应用 fsync()
   │
   ├── 触发内核脏页回写
   │      └── 一个或多个 FUSE_WRITE
   │
   └── FUSE_FSYNC
          │
          ▼
       daemon 刷新后端
          │
          ▼
       回复成功或错误
```

因此测量写性能时，必须说明是否包含 `fsync`。只测 `write()` 很可能主要测到内存和排队速度，而不是后端持久化能力。

# 六、一次 FUSE I/O 到底复制几次

答案不是一个固定数字。先约定本文只统计 **CPU 可见的数据内存复制**，不把磁盘或网卡 DMA 算成普通 `memcpy`，也不把协议头解析算成完整数据复制。

## 1. 控制面请求

以普通 `LOOKUP` 为例，忽略小型协议头的内部实现细节，可以粗略理解为：

```text
内核请求结构 ──copy_to_user──> daemon 请求 buffer

daemon 回复 buffer ──copy_from_user──> 内核回复结构
```

即请求和回复方向各跨一次内核/用户边界。元数据消息很小，主要成本通常不是复制带宽，而是：

- 上下文切换与唤醒；
- 请求排队；
- 路径逐级往返；
- daemon 内部锁与调度；
- 远端元数据 RPC 延迟。

## 2. Buffered read 缓存命中

```text
Page Cache ──> 应用 buffer
```

通常是 **1 次数据复制**。

如果使用 `mmap()` 访问页缓存，则没有每次 `read()` 的显式复制，但首次缺页仍需要建立映射并可能触发 FUSE 读取。

## 3. Buffered read 缓存未命中：普通 buffer 接口

假设 daemon 先把后端数据读入自己的用户态 buffer：

```text
后端内核页缓存 ──> daemon buffer
                         │
                         └──> FUSE 内核页 / Page Cache
                                      │
                                      └──> 应用 buffer
```

常见理解是 **3 次 CPU 数据复制**：

1. 后端文件 page cache 到 daemon buffer；
2. daemon buffer 经 `/dev/fuse` 到 FUSE page cache；
3. FUSE page cache 到应用 buffer。

如果后端是网络，第一步可能表现为 socket receive buffer 到 daemon buffer；如果后端 SDK 又做中间缓冲，复制还会更多。

这个“三次”是典型链路，不是内核 ABI 保证。页拼接、直接 I/O、迭代器类型和具体实现都可能改变次数。

## 4. Direct I/O read

绕过 FUSE page cache 后，典型路径是：

```text
后端页缓存 / socket buffer ──> daemon buffer
                                      │
                                      └──> 应用 buffer
```

通常可视为 **2 次 CPU 数据复制**。它少了 FUSE page cache 到应用 buffer 这一跳，但每次 I/O 都需要 daemon 参与。

## 5. Buffered write

普通路径可以理解为：

```text
应用 buffer ──> FUSE Page Cache / 请求页
                         │
                         └──> daemon buffer
                                  │
                                  └──> 后端 Page Cache / socket buffer
```

常见是 **3 次 CPU 数据复制**：

1. 应用 buffer 到内核页；
2. 内核 FUSE 请求数据到 daemon buffer；
3. daemon buffer 到后端文件或网络内核缓冲区。

writeback cache 会改变时机和聚合方式，但不一定自动减少每个字节跨越各层时的复制次数。

## 6. 汇总

| 场景 | 典型 CPU 数据复制次数 | 主要特点 |
|---|---:|---|
| 元数据请求/回复 | 每个方向约 1 次小消息复制 | 往返和调度通常比带宽更重要 |
| Buffered read 命中 | 1 | 不进入 daemon |
| Buffered read 未命中，普通接口 | 约 3 | 后端→daemon→FUSE cache→应用 |
| Direct I/O read，普通接口 | 约 2 | 绕过 FUSE page cache |
| Buffered write，普通接口 | 约 3 | 应用→FUSE→daemon→后端 |
| `mmap` 热页访问 | 0 次显式 read 复制 | 仍有缺页、映射和首次填充成本 |

不要把这张表当成所有系统的固定答案。分析具体产品时，应该用 eBPF、`perf`、ftrace、源码和基准测试确认真实路径。

# 七、如何减少复制

## 1. 使用 libfuse 的 buffer-vector 接口

高层接口常让 daemon 收到一个普通用户态 buffer。低层接口提供 `fuse_bufvec`、`read_buf` 和 `write_buf`，允许数据由文件描述符、内存块或组合 buffer 表示。

daemon 可以告诉 libfuse：数据已经位于某个后端文件描述符，而不是先读进用户态数组：

```text
后端 fd ── splice / page move ──> /dev/fuse
```

这为 `splice(2)` 零拷贝或少拷贝路径创造条件。

## 2. 使用 `splice`

`splice` 可以让数据在文件描述符之间经 pipe buffer 移动，避免把完整数据拷入 daemon 用户空间：

```text
后端 fd
   │ splice
   ▼
Pipe pages
   │ splice
   ▼
/dev/fuse
```

读取方向和写入方向都可能使用对应 splice 能力。收益取决于：

- 内核与 libfuse 是否启用相关能力；
- 后端 fd 是否支持 splice；
- 请求大小是否足以抵消系统调用成本；
- 页面是否可以直接引用，还是底层最终仍需复制。

因此 splice 更准确地叫“避免用户态 bounce buffer”，而不是承诺整条链路绝对零拷贝。

## 3. FUSE passthrough

较新的 FUSE passthrough 能让 daemon 在打开文件时把一个后端文件交给内核。后续部分读写可直接走后端文件操作，不再为每次数据 I/O 往返用户态 daemon：

```text
传统 FUSE：
应用 → VFS → fuse.ko → daemon → 后端文件

Passthrough：
应用 → VFS → fuse.ko ─────────→ 后端文件
                    daemon 不在每次数据路径上
```

daemon 仍负责命名空间、策略和初始文件选择，但热数据面被下沉到内核。这对“用户态负责映射，数据实际存放在本地文件”的场景非常有效。

它不适用于所有后端。例如对象存储或自定义网络协议不能天然表现为一个可直接交给内核的普通文件。

## 4. virtio-fs DAX

虚拟机文件共享场景中的 virtio-fs 可以结合 DAX，把宿主机文件页映射到客户机地址空间：

```text
Host file pages
       │ 共享映射
       ▼
Guest page table
       │
       ▼
应用 load/store
```

这样可以绕过客户机 page cache，并减少数据复制和内存重复缓存。DAX 适用于共享内存窗口支持良好的虚拟化场景，不是普通本机 FUSE 挂载都能直接启用的通用开关。

## 5. 避免 daemon 自己再复制

即使内核侧已经优化，用户态实现仍可能制造额外复制：

```text
网络 SDK buffer
   → 解码临时 buffer
   → 文件系统 cache buffer
   → libfuse reply buffer
```

应尽量做到：

- 复用接收和发送缓冲区；
- 使用 scatter/gather I/O；
- 使用引用计数切片而不是重新拼接大块数据；
- 避免为了异步生命周期无条件复制整块数据；
- 让压缩、校验和加密原地或流式执行；
- 为固定大小块使用 buffer pool。

# 八、如何减少控制面往返

对小文件和深目录工作负载，控制面往往比数据复制更重要。

## 1. 合理设置 entry 和 attribute timeout

FUSE daemon 在 `LOOKUP`、`GETATTR` 回复中可以给出缓存有效期。有效期内，VFS 可以复用 dentry 和 inode 属性，避免再次进入用户态。

```text
缓存时间短：一致性更及时，控制面往返更多
缓存时间长：性能更好，但外部修改可能更晚可见
```

不存在的路径也可以进行 negative dentry caching，减少反复查询同一个不存在文件。

## 2. 启用 readdirplus

普通 `READDIR` 只返回目录项名字，应用随后可能对每个名字调用 `stat`，形成大量 `LOOKUP` / `GETATTR`。

`READDIRPLUS` 可以一次返回目录项及其属性：

```text
READDIR：     name1 name2 name3
后续：        GETATTR × 3

READDIRPLUS： name + inode + attr 一次批量返回
```

它适合“列目录后立即查看属性”的场景；如果应用只需要名字，额外预取属性也可能浪费。

## 3. 增大请求并合并操作

应尽量使用更大的 `max_read` / `max_write`，配合 readahead 和 writeback，把大量小请求合并成少量大请求。一次传输 1 MiB 通常比 256 次 4 KiB 往返更有效率。

但请求不能无限增大。大请求会增加单次排队延迟、内存占用和慢请求影响范围，需要结合设备和网络特征测试。

## 4. 多线程或异步 daemon

单线程 daemon 会把所有元数据和数据请求串行化。libfuse 多线程循环或异步执行器可以同时处理多个独立请求。

并发并非越多越好。应设置：

- worker 数量；
- 最大 background 请求数；
- congestion threshold；
- 后端连接池和并发上限；
- 每类请求的优先级与隔离。

否则队列只是从内核转移到 daemon 或后端。

# 九、如何选择缓存模式

## Buffered I/O 适合

- 文件会被重复读取；
- 工作集能进入内存；
- 希望利用内核 readahead 和 writeback；
- 应用没有自己的完整缓存体系；
- 后端变化可以通过失效机制管理。

## Direct I/O 适合

- 数据只顺序经过一次，不值得缓存；
- 应用或数据库已有自己的缓存；
- 不希望 FUSE cache 与后端 cache 双份占用内存；
- 需要更明确的 I/O 完成语义；
- 工作负载能接受每次操作进入 daemon。

## Passthrough 适合

- 数据最终落在本地普通文件；
- daemon 主要负责路径映射、权限或策略；
- 希望把稳定的数据面交还内核；
- 当前内核和用户态库支持所需特性。

没有一种模式适合所有负载。大量随机小读、顺序大读、元数据密集构建、数据库 direct I/O 和对象存储挂载的最优配置完全不同。

# 十、上下文切换和调度也要优化

FUSE 性能损耗不只有复制。一次缓存未命中的同步请求可能经历：

```text
应用运行
  → 进入内核并睡眠
  → 唤醒 daemon
  → daemon 运行并等待后端
  → daemon 回复
  → 唤醒应用
```

需要关注：

- daemon 是否被 CPU throttling；
- daemon 与应用是否争抢同一批 CPU；
- NUMA 节点是否远离后端网卡或存储设备；
- 是否存在全局锁或单队列瓶颈；
- 大请求是否阻塞小元数据请求；
- 后端延迟是否占据所有 worker。

常见方法包括 CPU affinity、NUMA 亲和、控制面与数据面线程池隔离、请求分片、优先级队列和异步后端 I/O。

# 十一、如何测量真实链路

只看应用吞吐无法判断瓶颈位置。建议组合使用：

```text
strace
  查看应用系统调用、请求大小和阻塞时间

perf
  查看 CPU 周期、memcpy、调度和锁热点

ftrace / trace-cmd
  跟踪 fuse、VFS、writeback 和 block 层事件

eBPF / bpftrace
  统计请求延迟分布、上下文切换和函数调用

pidstat / vmstat
  观察线程调度、缺页、CPU 和 I/O wait

iostat / network metrics
  判断后端设备或网络是否才是真正瓶颈
```

对复制次数，最可靠的方法不是背一个数字，而是画出当前实现的 buffer 所有权：

```text
数据最初在哪个页或 buffer？
谁创建了新的目标 buffer？
哪次 read/write 触发 copy_to_user 或 copy_from_user？
能否改成页引用、共享映射、splice 或 fd passthrough？
```

然后用 `perf` 的 memcpy 热点、内核 tracepoint 和吞吐变化验证推断。

# 总结

FUSE 把文件系统策略放到用户态，内核通过 `/dev/fuse` 与 daemon 交换协议消息。

控制面链路主要是：

```text
应用系统调用 → VFS → fuse.ko → /dev/fuse → daemon
                                      ↓
                                元数据或远端 RPC
                                      ↓
应用被唤醒 ← VFS ← fuse.ko ← /dev/fuse ← daemon 回复
```

数据面则要进一步区分页缓存命中、缓存未命中、direct I/O、writeback、splice、DAX 和 passthrough。普通 buffer 实现中，缓存未命中的读取或写入经常可以观察到约三次 CPU 数据复制，但这不是所有 FUSE 实现的固定常数。

优化 FUSE 的优先级通常是：

1. 先通过元数据缓存、readdirplus 和批量请求减少用户态往返；
2. 再通过 page cache、readahead 和 writeback 提高命中与聚合；
3. 使用 `fuse_bufvec` 和 `splice` 消除 daemon bounce buffer；
4. 条件允许时使用 passthrough 或 virtio-fs DAX，把热数据面移出用户态往返；
5. 最后结合 NUMA、线程池、队列和后端并发做系统级调优。

最重要的判断不是“FUSE 有几次复制”，而是：

> 当前工作负载中的每个请求经过哪些缓存、线程和 buffer；其中哪些往返可以缓存掉，哪些数据复制可以变成页面引用、共享映射或内核直通。
