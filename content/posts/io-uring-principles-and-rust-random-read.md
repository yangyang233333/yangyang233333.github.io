---
title: "从原理到实战：用 Rust 理解 io_uring 异步 I/O"
date: 2026-09-07T09:28:00+08:00
draft: false
tags: ["Linux", "io_uring", "Rust", "异步 I/O", "存储"]
categories: ["系统编程"]
summary: "从 SQ/CQ 共享环、io_uring_enter 和内核执行路径出发，解释 io_uring 为什么能批量提交 I/O，并用 Rust 实现一个可运行的并发随机读程序。"
---

传统文件 I/O 的接口以“调用并等待”为中心：线程调用 `pread`，内核完成读取后再返回。`io_uring` 把它改成了“提交与完成分离”：应用先把多个请求写入共享提交队列，内核异步执行，再把结果写回完成队列。

这套设计的核心价值不是让磁盘本身变快，而是让程序能够以较少的系统调用管理大量在途 I/O，并把批处理、并发和完成通知统一到一套接口中。

本文先拆解 `io_uring` 的工作原理，再用 Rust 的 `io-uring` crate 实现一个并发随机读示例。示例使用普通 Buffered I/O，重点展示 API 和内存生命周期；是否更快，需要结合 Direct I/O、队列深度、块大小、设备和负载实测。

# 一、传统 I/O 的限制在哪里

随机读取文件时，常见选择有三种：

| 方案 | 调用方式 | 线程是否可能阻塞 | 主要特点 |
| --- | --- | --- | --- |
| `pread` | 同步系统调用 | 是 | 简单、位置式读取、不修改文件偏移 |
| `mmap` | 访问映射内存 | 缺页时会阻塞 | 读取路径短，但缺页和回收由内核隐式处理 |
| `io_uring` | 提交请求，稍后收割完成 | 调用方不必为每个请求阻塞 | 支持批量提交和大量在途操作 |

以 `pread` 为例，一个线程通常经历下面的过程：

```text
用户态：准备参数 ── syscall ── 等待 ── 获得结果
                         │
内核态：              查页缓存 / 发起设备 I/O
```

如果程序需要同时维护几百个随机读，直接增加线程会带来线程栈、调度和上下文切换成本。线程池可以限制线程数量，但阻塞任务仍然占用工作线程。

`io_uring` 的思路是：**用队列表示在途 I/O，而不是用一个阻塞线程表示一个在途 I/O。**

# 二、io_uring 的核心数据结构

`io_uring` 的用户态接口主要围绕三个系统调用：

1. `io_uring_setup`：创建 ring，返回文件描述符和内存布局；
2. `io_uring_enter`：通知内核消费提交项，或等待完成项；
3. `io_uring_register`：预注册文件、缓冲区等资源，减少热路径开销。

创建 ring 后，应用通过 `mmap` 获得内核与用户态共享的几块内存。最重要的是提交队列和完成队列：

```text
用户态                                         内核态

准备 SQE
   │
   ▼
┌───────────────┐       io_uring_enter       ┌───────────────┐
│ SQ：提交队列   │ ─────────────────────────▶ │ 取出并执行请求 │
└───────────────┘                             └───────┬───────┘
                                                    │
                                                    ▼
┌───────────────┐                             ┌───────────────┐
│ CQ：完成队列   │ ◀───────────────────────── │ 写入完成结果   │
└───────┬───────┘                             └───────────────┘
        │
        ▼
应用读取 CQE，恢复对应任务
```

常见缩写如下：

| 缩写 | 含义 | 作用 |
| --- | --- | --- |
| SQ | Submission Queue | 保存待提交请求的索引和队列状态 |
| SQE | Submission Queue Entry | 描述一次具体操作，例如读、写、接收或超时 |
| CQ | Completion Queue | 保存已完成操作的队列状态 |
| CQE | Completion Queue Entry | 返回操作结果以及应用提供的 `user_data` |

SQ 和 CQ 都是环形队列。用户态与内核分别推进 `head`、`tail`，在共享内存上交换所有权。准备多个 SQE 后，可以用一次 `io_uring_enter` 批量提交；消费 CQE 通常只需读取共享内存，不必为每个完成事件调用一次系统调用。

# 三、一次读取如何穿过 io_uring

假设程序要读取文件的 `offset` 位置，完整路径可以拆成五步。

## 1. 构造 SQE

SQE 描述“做什么”和“对谁做”。一次读取至少需要：

- 操作码，例如 `IORING_OP_READ`；
- 文件描述符；
- 缓冲区地址和长度；
- 文件偏移；
- 应用自定义的 `user_data`。

`user_data` 不参与 I/O，本质上是一个 64 位标签。完成时，内核把它原样放入 CQE，应用据此把结果匹配回原请求。

## 2. 发布到 SQ

应用把 SQE 放入 SQE 数组，再把对应索引写入 SQ ring，最后更新 `tail`。这些步骤需要正确的内存顺序，实际项目应交给 `liburing` 或 Rust crate 处理，不要随意手写原子操作。

## 3. 通知内核

默认模式下，应用调用 `io_uring_enter`，告诉内核有多少新请求。一次调用可以提交多个 SQE，因此高并发场景能自然形成 batching。

启用 SQPOLL 后，内核线程会轮询 SQ。在条件满足时，应用提交 I/O 可以进一步减少系统调用，但会消耗专门的 CPU 时间，不应默认开启。

## 4. 内核执行请求

内核先尝试在当前上下文执行请求。某些操作可以真正异步完成；可能阻塞的 Buffered I/O 则可能转交给 io-wq 工作线程。

这一区别非常重要：**io_uring 提供统一的异步接口，不代表所有底层文件系统操作都变成了硬件级异步。** 如果读取命中 page cache，可能很快完成；如果路径会阻塞，内核仍需要工作线程承接。

## 5. 写入 CQE

操作结束后，内核写入 CQE：

- `user_data`：原请求标签；
- `res`：非负数通常是完成字节数，负数是 `-errno`；
- `flags`：完成项附加信息。

应用消费 CQE、处理结果并推进 CQ 的 `head`，队列槽位即可复用。

# 四、默认、IOPOLL 与 SQPOLL 不要混为一谈

`io_uring` 的几种模式优化的是不同环节：

| 模式 | 谁发现提交 | 谁等待设备完成 | 适用场景 |
| --- | --- | --- | --- |
| 默认模式 | `io_uring_enter` 通知内核 | 中断或内核异步机制 | 通用，首先选择 |
| IOPOLL | `io_uring_enter` 通知内核 | 调用方轮询设备完成 | 低延迟 Direct I/O，要求设备支持 |
| SQPOLL | 内核线程轮询 SQ | 通常仍由中断通知完成 | 高频提交，愿意用 CPU 换少量 syscall |
| SQPOLL + IOPOLL | 内核线程轮询 SQ | 轮询设备完成 | 极端低延迟、专用资源场景 |

两个容易产生的误解是：

- SQPOLL 轮询的是提交队列，不等于轮询存储设备；
- IOPOLL 面向 Direct I/O 和支持 polling 的设备，不适用于普通 Buffered I/O。

对于大多数应用，先使用默认模式，测出瓶颈后再决定是否启用 polling，通常比直接打开高级选项更可靠。

# 五、Rust 包装时真正困难的部分

把 SQE 推入队列并不难，难的是确保内核完成访问前，所有参数仍然有效。

一次读取提交后，内核会在未来某个时刻写入用户缓冲区。因此在 CQE 到来前必须保证：

1. 缓冲区没有被释放；
2. 缓冲区地址没有因 `Vec` 扩容等操作改变；
3. 文件描述符仍然有效；
4. 同一缓冲区没有被其他代码并发读写；
5. 请求被取消时，内核不再访问已经回收的内存。

这也是 `submission.push(entry)` 为 `unsafe` 的原因之一：编译器无法验证 SQE 中的裸指针在异步操作期间是否有效。

一种稳妥的所有权模型是把缓冲区交给操作本身：

```text
提交前：应用拥有 Buffer
          │ move
          ▼
在途时：Operation 独占 Buffer，内核持有其稳定地址
          │ CQE
          ▼
完成后：Operation 把 Buffer 和结果一起交还应用
```

如果想暴露 `read_at(&mut [u8]).await` 这种借用接口，就必须处理 Future 中途被丢弃的问题。仅仅丢弃 Future 并不会自动取消已经提交给内核的请求；底层需要继续持有缓冲区，或者提交取消请求并等待取消完成。取消安全是生产级封装不能回避的设计点。

# 六、代码实战：并发随机读取文件

下面实现一个小程序：创建测试文件，向 ring 提交四个不同偏移的读取请求，再从 CQ 中收割结果。它直接展示 SQE、CQE、`user_data` 和队列深度之间的关系。

## 1. 创建项目

```bash
cargo new uring-random-read
cd uring-random-read
cargo add io-uring
```

示例面向 Linux。运行环境需要内核支持 `io_uring`，容器的 seccomp 策略也必须允许相关系统调用。

## 2. 完整代码

将 `src/main.rs` 替换为：

```rust
use io_uring::{opcode, types, IoUring};
use std::fs::{File, OpenOptions};
use std::io::{self, Write};
use std::os::fd::AsRawFd;

const BLOCK_SIZE: usize = 16;
const OFFSETS: [u64; 4] = [0, 16, 32, 48];

fn create_demo_file(path: &str) -> io::Result<()> {
    let mut file = File::create(path)?;
    file.write_all(b"block-0000000000block-1111111111block-2222222222block-3333333333")?;
    file.sync_all()
}

fn main() -> io::Result<()> {
    let path = "demo.data";
    create_demo_file(path)?;

    let file = OpenOptions::new().read(true).open(path)?;
    let mut ring = IoUring::new(8)?;
    let mut buffers = vec![[0_u8; BLOCK_SIZE]; OFFSETS.len()];

    for (request_id, (&offset, buffer)) in
        OFFSETS.iter().zip(buffers.iter_mut()).enumerate()
    {
        let entry = opcode::Read::new(
            types::Fd(file.as_raw_fd()),
            buffer.as_mut_ptr(),
            buffer.len() as u32,
        )
        .offset(offset)
        .build()
        .user_data(request_id as u64);

        unsafe {
            ring.submission()
                .push(&entry)
                .map_err(|_| io::Error::other("submission queue is full"))?;
        }
    }

    ring.submit_and_wait(OFFSETS.len())?;

    let mut completed = 0;
    for cqe in ring.completion() {
        let request_id = cqe.user_data() as usize;
        let result = cqe.result();

        if result < 0 {
            return Err(io::Error::from_raw_os_error(-result));
        }

        let bytes_read = result as usize;
        println!(
            "request={request_id}, offset={}, data={}",
            OFFSETS[request_id],
            String::from_utf8_lossy(&buffers[request_id][..bytes_read])
        );
        completed += 1;
    }

    assert_eq!(completed, OFFSETS.len());
    Ok(())
}
```

运行：

```bash
cargo run
```

输出顺序不应被当作提交顺序的一部分。一次可能的结果是：

```text
request=0, offset=0, data=block-0000000000
request=1, offset=16, data=block-1111111111
request=2, offset=32, data=block-2222222222
request=3, offset=48, data=block-3333333333
```

## 3. 代码为什么是安全的

示例在提交前一次性创建 `buffers`，提交后不再改变其长度，因此各数组的地址保持稳定。直到 `submit_and_wait` 返回且 CQE 被消费，`buffers` 和 `file` 都没有离开作用域。

同时，程序用 `request_id` 作为 `user_data`，而不是把 Rust 对象地址直接转换成整数。这样更容易审计，也避免手工恢复裸指针所有权。

不过，这仍然是一段底层同步程序，而不是完整的 Rust `Future` 封装。它会在 `submit_and_wait` 中等待至少四个完成项。要接入异步运行时，还需要一个驱动层负责：

1. 接收多个任务产生的 SQE；
2. 批量调用 `submit`；
3. 等待并收割 CQE；
4. 根据 `user_data` 找到任务状态；
5. 保存结果并调用对应 `Waker`；
6. 正确处理取消、关闭和队列满。

# 七、从完成队列到 Future

一个典型的异步封装可以把请求状态保存在表中：

```text
业务 Future
    │ 创建请求并登记 Waker
    ▼
任务表：request_id ──▶ { buffer, state, result, waker }
    │
    ├── SQE.user_data = request_id
    ▼
io_uring 驱动提交请求并收割 CQE
    │
    └── 根据 request_id 写入结果并 wake
```

Future 第一次被 `poll` 时提交请求并返回 `Pending`。驱动收到 CQE 后更新共享状态并唤醒任务；下一次 `poll` 读取结果，返回 `Ready`。

实际实现还要处理一个细节：唤醒和注册 `Waker` 可能并发发生。状态机必须保证“完成发生在注册之前”或“注册发生在完成之前”都不会丢失通知。通常应使用成熟运行时或 crate，而不是从零实现生产级 executor 集成。

# 八、性能判断：io_uring 不是必然更快

`io_uring` 最稳定的优势是统一接口、批量提交和管理高并发 I/O，而不是对所有负载无条件降低延迟。

Buffered I/O 随机读可能出现以下情况：

- 数据命中 page cache，同步 `pread` 已经很快；
- 未命中时，io-wq 仍需要工作线程执行可能阻塞的路径；
- 小队列深度无法形成有效 batching；
- 封装层的分配、channel、锁和唤醒抵消了系统调用收益；
- `mmap` 在特定只读访问模式下路径更短。

因此 benchmark 至少要明确：

| 变量 | 需要说明的内容 |
| --- | --- |
| I/O 模式 | Buffered 还是 Direct I/O |
| 缓存状态 | 热缓存、冷缓存或工作集大于内存 |
| 块大小 | 4 KiB、16 KiB、1 MiB 等 |
| 队列深度 | 同时在途的请求数 |
| 指标 | 吞吐、平均延迟以及 p99/p999 |
| 设备 | HDD、SATA SSD、NVMe 及文件系统 |

对数据库或存储系统，更合理的问题不是“io_uring 是否比 mmap 快”，而是：在指定数据集、缓存状态、并发度和延迟目标下，哪条 I/O 路径更合适。

# 九、工程实践建议

- 先使用默认模式，确认提交、完成和取消语义正确，再评估 SQPOLL 或 IOPOLL；
- 用足够的队列深度形成并发，但为在途请求数设置上限，避免内存失控；
- 优先用整数句柄作为 `user_data`，通过 slab 或 generational arena 管理任务；
- 如果传递裸指针，确保缓冲区地址稳定，并把所有权保持到 CQE 到达；
- 区分“Future 被丢弃”和“内核请求已取消”，不要提前释放资源；
- 分别测试 Buffered I/O、Direct I/O 和热缓存，不要把一种结果推广到所有场景；
- 生产代码优先采用持续维护的封装库，并在目标内核、文件系统和容器策略中验证。

# 总结

理解 `io_uring` 可以抓住三条主线：

1. SQE 描述请求，CQE 返回结果，`user_data` 负责关联两者；
2. 共享环和批量提交减少了高并发 I/O 的系统调用与调度成本；
3. Rust 封装的关键不是调用 API，而是保证缓冲区、文件描述符和任务状态活到内核真正完成访问。

它最适合被看成一个高并发 I/O 基础设施，而不是一个自动加速开关。先建立正确的生命周期和取消模型，再根据真实 workload 调整队列深度、注册资源与 polling 模式，才能把它的能力转化为稳定收益。

延伸阅读：

- [io_uring 的接口与实现](https://www.skyzh.dev/blog/2021-06-14-deep-dive-io-uring)
- [在 Rust 中实现基于 io_uring 的异步随机读文件](https://www.skyzh.dev/blog/2021-01-30-async-random-read-with-rust/)
- [Linux 内核 io_uring 文档](https://docs.kernel.org/io_uring/)
- [Rust io-uring crate](https://docs.rs/io-uring/)
