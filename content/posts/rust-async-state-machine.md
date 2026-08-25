---
title: "Rust 异步编程原理：Future、状态机、Waker 与 Executor"
date: 2026-08-25T15:35:00+08:00
draft: false
tags: ["Rust", "异步编程", "Future", "状态机", "Tokio"]
categories: ["技术"]
description: "面向初学者，从 Future::poll 出发推导 Rust async/await 状态机，并解释 Waker、Executor、Reactor、Pin、并发与阻塞。"
---

Rust 的 `async/await` 写起来很像同步代码：调用异步函数、等待结果，然后继续向下执行。但它的底层既不会为每个任务创建一个线程，也不会在 `await` 时阻塞当前线程。

它真正做的事情是：**编译器把异步函数转换成一个可以暂停和恢复的状态机，这个状态机实现 `Future`；Executor 反复调用 `poll` 推进状态机，Waker 在资源就绪后通知 Executor 再次调度。**

本文从一段普通异步代码出发，逐层解释：

- `async fn` 返回的到底是什么；
- `await` 为什么能暂停函数；
- 异步代码和状态机是什么关系；
- `Future::poll`、`Context` 和 `Waker` 分别负责什么；
- Executor 与 Reactor 如何配合；
- 为什么 Rust 异步需要 `Pin`；
- Tokio 在这套模型中处于什么位置。

本文定位为 [《Rust Tokio Runtime 原理与最佳实践》](/posts/rust-tokio-runtime-and-best-practices/) 的前置教程。建议先理解本文中的 `Future::poll`、状态机、Waker 和 Executor，再继续阅读 Tokio 的调度器、I/O Driver、任务取消与工程实践。

## 阅读前准备

读者只需要了解 Rust 的基本语法、所有权、枚举和 trait，不要求提前掌握 Tokio。

文中的普通 Rust 代码可以放入一个空项目中实验：

```bash
cargo new rust-async-basics
cd rust-async-basics
```

涉及 Tokio 的示例可加入依赖：

```toml
[dependencies]
tokio = { version = "1", features = ["full"] }
```

学习时建议始终追问三个问题：

1. 当前是谁在调用 `poll`？
2. Future 返回 `Pending` 前是否安排了唤醒？
3. 哪些局部变量必须跨越 `await` 保存？

## 一、为什么需要异步编程

假设程序需要从两个远程服务读取数据。最直接的同步写法是：

```rust
fn load() -> Result<Data> {
    let user = read_user_from_network()?;
    let orders = read_orders_from_network()?;
    Ok(Data { user, orders })
}
```

如果网络请求需要 100 毫秒，当前线程会在系统调用或等待结果时被挂起。对于少量请求，这种模型简单可靠；但当服务器同时处理数万个连接时，为每个连接准备一个线程会带来明显成本：

- 每个线程需要独立栈空间；
- 创建和销毁线程有成本；
- 大量线程会增加上下文切换；
- 多数线程实际上只是在等待网络、磁盘或定时器。

异步编程的核心目标不是“让单个操作执行得更快”，而是：

> 当一个任务等待 I/O 时，让出线程去执行其他任务；I/O 就绪后，再从原来的位置继续执行。

这要求函数具备两个同步函数没有的能力：

1. 在某个位置暂停；
2. 之后保留局部变量，从暂停位置继续。

状态机正是实现这两个能力的关键。

## 二、`async fn` 返回的不是结果，而是 Future

看一段最简单的代码：

```rust
async fn answer() -> u32 {
    42
}
```

调用它时：

```rust
let value = answer();
```

此时 `value` 不是 `42`，而是某种编译器生成的匿名类型，这个类型实现了 `Future<Output = u32>`。

可以把 `async fn` 粗略理解为：

```rust
fn answer() -> impl Future<Output = u32> {
    async {
        42
    }
}
```

Future 只是一个“未来可能产生结果的计算”。创建 Future 通常不会立即执行函数体，必须有人主动调用它的 `poll` 方法，它才会向前运行。

Future trait 的核心形式是：

```rust
trait Future {
    type Output;

    fn poll(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Self::Output>;
}
```

`poll` 只有两种结果：

```rust
enum Poll<T> {
    Ready(T),
    Pending,
}
```

- `Ready(value)`：计算完成，返回最终结果；
- `Pending`：现在还不能完成，稍后需要再来 poll。

Future 不是后台线程，也不是自动运行的回调。它更像一个被动对象：

```text
Executor 调用 poll
        │
        ├── Ready(value)：任务完成
        │
        └── Pending：暂时停止，等待唤醒
```

## 三、`await` 与状态机的关系

考虑下面这个异步函数：

```rust
async fn fetch_and_parse() -> Result<Data, Error> {
    let response = fetch().await?;
    let body = response.body().await?;
    let data = parse(body)?;
    Ok(data)
}
```

这个函数中有两个 `await`。每个 `await` 都可能成为暂停点：

```text
开始
  │
  ▼
poll fetch
  │ Pending
  ▼
暂停点 1
  │ fetch Ready
  ▼
保存 response，poll response.body
  │ Pending
  ▼
暂停点 2
  │ body Ready
  ▼
parse(body)
  │
  ▼
返回 Ready(data)
```

编译器会把它转换为一个类似 enum 的状态机。以下代码不是编译器的真实输出，但可以帮助理解：

```rust
enum FetchAndParseFuture {
    Start,
    WaitingFetch {
        fetch_future: FetchFuture,
    },
    WaitingBody {
        body_future: BodyFuture,
        response: Response,
    },
    Done,
}
```

它的 `poll` 逻辑可以粗略写成：

```rust
fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<Data, Error>> {
    loop {
        match self.state {
            Start => {
                self.state = WaitingFetch {
                    fetch_future: fetch(),
                };
            }

            WaitingFetch { ref mut fetch_future } => {
                match Pin::new(fetch_future).poll(cx) {
                    Poll::Pending => return Poll::Pending,
                    Poll::Ready(Err(error)) => return Poll::Ready(Err(error)),
                    Poll::Ready(Ok(response)) => {
                        self.state = WaitingBody {
                            body_future: response.body(),
                            response,
                        };
                    }
                }
            }

            WaitingBody { ref mut body_future, .. } => {
                match Pin::new(body_future).poll(cx) {
                    Poll::Pending => return Poll::Pending,
                    Poll::Ready(Err(error)) => return Poll::Ready(Err(error)),
                    Poll::Ready(Ok(body)) => {
                        let data = parse(body)?;
                        self.state = Done;
                        return Poll::Ready(Ok(data));
                    }
                }
            }

            Done => panic!("future polled after completion"),
        }
    }
}
```

真实实现会考虑字段布局、借用、析构和优化，不会简单地生成上面这个 enum。但核心思想一致：

> `async fn` 被编译为状态机，`await` 是状态之间可能返回 `Pending` 的边界，跨越 `await` 仍然存活的局部变量会成为状态机的字段。

### 没有跨越 await 的变量不需要保存

例如：

```rust
async fn example() {
    let first = 1;
    consume(first);

    wait_for_io().await;

    let second = 2;
    consume(second);
}
```

`first` 在 `await` 前已经用完，不需要保存进状态机。`second` 在恢复后才创建，也不需要出现在等待状态中。

而下面的 `buffer` 必须跨越 `await`：

```rust
async fn example() {
    let mut buffer = Vec::new();
    read_into(&mut buffer).await;
    consume(buffer);
}
```

因此 `buffer` 会成为 Future 状态的一部分。Future 的大小通常由“所有暂停状态中需要保存的最大数据组合”决定，而不是简单等于所有局部变量之和。

## 四、`await` 本质上做了什么

表达式：

```rust
let value = future.await;
```

可以粗略理解为：

```rust
loop {
    match future.poll(cx) {
        Poll::Ready(value) => break value,
        Poll::Pending => return Poll::Pending,
    }
}
```

注意这里的 `return Poll::Pending` 不是从普通函数返回，而是当前状态机的 `poll` 方法返回。状态机对象本身仍然存在，内部字段也不会丢失。

下一次 Executor 再次调用 `poll` 时，状态机根据保存的状态，直接从对应暂停点继续，而不是从函数第一行重新开始。

因此，`await` 不是：

- 阻塞当前线程；
- 不断循环检查 Future；
- 创建一个新线程；
- 保存和恢复 CPU 调用栈。

它是：

> poll 子 Future；如果子 Future 未完成，就保存当前状态并把控制权返回 Executor。

这种实现也被称为无栈协程。异步任务不需要保留完整线程栈，只需要保存跨越暂停点的变量和状态编号。

## 五、谁来再次 poll Future：Waker

Future 返回 `Pending` 后，Executor 不能不停地 poll：

```rust
loop {
    future.poll(cx);
}
```

这种方式会形成 busy loop，即使网络数据还要 10 秒才到达，也会持续占用 CPU。

正确做法是：Future 在返回 `Pending` 前，保存 `Context` 中的 `Waker`。当资源就绪时，底层 I/O 驱动调用 `wake()`，通知 Executor：这个任务可能有进展了，可以重新放回运行队列。

流程如下：

```text
Executor poll Task A
        │
        ▼
Future 尝试读取 socket
        │ 暂未就绪
        ▼
向 Reactor 注册 socket + Waker
        │
        ▼
返回 Pending，线程执行其他任务

……网络数据到达……

Reactor 收到可读事件
        │
        ▼
调用对应 Waker::wake()
        │
        ▼
Task A 重新进入 Executor 就绪队列
        │
        ▼
Executor 再次 poll Task A
```

`Waker` 不负责直接执行 Future。它通常只是把任务标记为 ready，并放入调度队列。真正调用 `poll` 的仍然是 Executor 的工作线程。

### 为什么允许伪唤醒

被唤醒只表示“Future 现在可能有进展”，不保证下一次 poll 一定返回 `Ready`。

因此合法流程可能是：

```text
wake -> poll -> Pending -> 再次注册 Waker
```

Future 的 `poll` 必须始终能够处理这种情况，不能假设收到唤醒就一定完成。

## 六、Executor、Reactor 与 Runtime

Rust 标准库定义了 Future、Poll、Context 和 Waker，但没有提供完整异步运行时。Tokio、async-std、smol 等库在此基础上实现运行时。

一个异步 Runtime 通常包括三部分。

### 1. Executor

Executor 负责调度任务：

```text
就绪任务队列
      │
      ▼
取出 Task
      │
      ▼
构造 Context，调用 Future::poll
      │
      ├── Ready：销毁任务并保存结果
      │
      └── Pending：等待 Waker 重新入队
```

一个 Task 通常包含：

- 被 Pin 住的 Future；
- 调度状态；
- 指向 Executor 队列的引用；
- 用于构造 Waker 的信息。

多线程 Executor 还会实现工作窃取、任务迁移和并发队列。

### 2. Reactor

Reactor 负责等待外部事件，例如：

- Linux `epoll`；
- macOS/BSD `kqueue`；
- Windows IOCP；
- io_uring；
- 定时器到期。

它维护 I/O 资源和 Waker 的对应关系。事件发生时，Reactor 不执行整个业务 Future，而是唤醒对应任务。

### 3. Runtime

Runtime 把 Executor、Reactor、定时器、异步 I/O 类型和辅助 API 组合起来。

以 Tokio 为例：

```rust
#[tokio::main]
async fn main() {
    let body = reqwest::get("https://example.com")
        .await
        .unwrap()
        .text()
        .await
        .unwrap();

    println!("{}", body.len());
}
```

`#[tokio::main]` 大致负责：

1. 创建 Tokio Runtime；
2. 把 `main` 的 Future 交给 Runtime；
3. 驱动它直到返回 `Ready`；
4. 管理网络、定时器和任务调度。

## 七、为什么 `poll` 接收 `Pin<&mut Self>`

Future trait 不是使用普通的 `&mut self`，而是：

```rust
self: Pin<&mut Self>
```

这与状态机的内存安全有关。

异步状态机可能包含自引用关系。考虑：

```rust
async fn read_line() {
    let mut buffer = String::new();
    let future = read_into(&mut buffer);
    future.await;
    println!("{buffer}");
}
```

概念上，生成的状态机可能同时保存：

```text
buffer: String
read_future: 内部引用 buffer 的 Future
```

如果状态机在内存中移动，`read_future` 内部保存的地址可能仍指向旧位置，从而形成悬空引用。

`Pin` 提供的关键保证是：

> 一旦一个可能自引用的 Future 被 Pin，就不能再通过安全代码移动它。

需要注意，创建 Future 时并不要求它立刻固定地址。通常是在 Executor 准备开始 poll 时，将 Future 放进 `Box::pin` 或任务存储中，之后保持地址稳定。

### `Unpin` 是什么

大多数普通类型移动后不会破坏内部关系，因此自动实现 `Unpin`。对 `Unpin` 类型而言，`Pin<&mut T>` 基本可以安全地当作 `&mut T` 使用。

编译器生成的 async Future 往往不能假设为 `Unpin`，因为它可能在某些状态中包含自引用。因此手写组合器和底层 Future 时经常需要 pin projection，将 `Pin<&mut Struct>` 安全地投影到被 Pin 的字段。

日常业务代码很少需要直接处理这些细节，但它解释了为什么 Future trait 的签名看起来比普通 trait 复杂。

## 八、手写一个 Future

下面用一个简化的定时 Future 串起 `poll` 和 `Waker`。示例重点是原理，并不是生产级定时器实现：

```rust
use std::future::Future;
use std::pin::Pin;
use std::sync::{Arc, Mutex};
use std::task::{Context, Poll, Waker};
use std::thread;
use std::time::Duration;

struct SharedState {
    completed: bool,
    waker: Option<Waker>,
}

struct TimerFuture {
    shared: Arc<Mutex<SharedState>>,
}

impl TimerFuture {
    fn new(duration: Duration) -> Self {
        let shared = Arc::new(Mutex::new(SharedState {
            completed: false,
            waker: None,
        }));

        let thread_shared = Arc::clone(&shared);
        thread::spawn(move || {
            thread::sleep(duration);

            let mut state = thread_shared.lock().unwrap();
            state.completed = true;

            if let Some(waker) = state.waker.take() {
                waker.wake();
            }
        });

        Self { shared }
    }
}

impl Future for TimerFuture {
    type Output = ();

    fn poll(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Self::Output> {
        let mut state = self.shared.lock().unwrap();

        if state.completed {
            Poll::Ready(())
        } else {
            state.waker = Some(cx.waker().clone());
            Poll::Pending
        }
    }
}
```

执行过程是：

1. Executor 第一次 poll `TimerFuture`；
2. 定时器未完成，保存当前任务的 Waker；
3. 返回 `Pending`，Executor 去运行其他任务；
4. 后台线程结束 sleep，将 `completed` 设为 true；
5. 后台线程调用 `waker.wake()`；
6. Executor 把任务重新放入就绪队列；
7. 第二次 poll 看到 `completed = true`，返回 `Ready(())`。

真实 Tokio 定时器不会为每个 sleep 创建线程，而会使用统一的时间轮或定时器驱动，但 Future 与 Waker 的交互模式是一致的。

### 更新 Waker 很重要

示例每次 poll 都执行：

```rust
state.waker = Some(cx.waker().clone());
```

因为同一个 Future 后续可能由另一个任务上下文或工作线程 poll，旧 Waker 不一定仍代表当前调度位置。更严谨的实现可以使用 `will_wake` 判断是否需要替换。

## 九、一个任务如何被完整驱动

把前面的组件连接起来，一次异步网络读取的完整过程如下：

```text
1. async fn 被调用
   只创建状态机 Future，通常尚未执行函数体

2. Future 被 spawn 到 Runtime
   Runtime 将其包装成 Task，放入 Executor 就绪队列

3. Executor 第一次 poll
   状态机从 Start 开始运行，直到 socket_read.await

4. socket Future 尝试读取
   数据未就绪，向 Reactor 注册 fd 和 Waker

5. 返回 Pending
   外层状态机保存局部变量和当前状态，线程去执行其他 Task

6. 网络数据到达
   epoll/kqueue/IOCP 通知 Reactor

7. Reactor 调用 Waker
   Task 被重新加入 Executor 就绪队列

8. Executor 再次 poll
   状态机从等待 socket 的状态继续

9. socket Future 返回 Ready(data)
   await 表达式得到 data，异步函数继续向下执行

10. 最外层 Future 返回 Ready(output)
    Task 完成，JoinHandle 得到结果
```

异步运行时的本质，就是高效地重复以下循环：

```text
poll -> Pending -> wake -> poll -> ... -> Ready
```

### 一个最小 `block_on` 的概念模型

下面的伪代码故意省略线程安全、任务队列和高效唤醒，只用于说明 Executor 为什么需要响应 Waker：

```rust
fn block_on<F: Future>(future: F) -> F::Output {
    let mut future = Box::pin(future);

    loop {
        let waker = make_waker_for_current_task();
        let mut context = Context::from_waker(&waker);

        match future.as_mut().poll(&mut context) {
            Poll::Ready(output) => return output,
            Poll::Pending => park_until_woken(),
        }
    }
}
```

它体现了 Executor 的最小职责：

1. 固定 Future 的内存位置；
2. 构造能重新调度任务的 Waker；
3. 调用 `poll`；
4. 遇到 `Pending` 时休眠或执行其他任务；
5. 被唤醒后再次调用 `poll`。

真实 Tokio Runtime 不会只驱动一个 Future，也不会用如此简单的等待方式。它需要管理大量 Task、就绪队列、工作窃取、I/O 事件和定时器，但最底层仍然遵循同一个 `poll -> Pending -> wake -> poll` 循环。

## 十、状态机和线程栈有什么区别

同步函数暂停时，操作系统线程会保留完整调用栈：

```text
Thread Stack
┌─────────────────────┐
│ caller frame        │
│ async-like function │
│ nested call         │
│ local variables     │
└─────────────────────┘
```

Rust async Future 是无栈状态机：

```text
Future Object
┌─────────────────────┐
│ state = WaitingBody │
│ response            │
│ body_future         │
│ other live fields   │
└─────────────────────┘
```

只有跨越 `await` 仍然需要的数据才被保存，因此单个异步任务通常比线程栈轻量得多。

代价是：

- Future 类型可能很大；
- 深层 async 调用会形成嵌套 Future；
- 编译器需要生成复杂状态机；
- 错误信息和类型有时更复杂；
- 阻塞代码不会自动让出线程。

## 十一、`async` 并不等于并行

下面的代码仍然是顺序执行：

```rust
let user = fetch_user().await;
let orders = fetch_orders().await;
```

第二个 Future 要等第一个完成后才创建或 poll。

如果两个操作互不依赖，可以并发等待：

```rust
let (user, orders) = tokio::join!(
    fetch_user(),
    fetch_orders(),
);
```

`join!` 会在同一个任务中轮流 poll 两个子 Future。它提供并发，但不保证在不同 CPU 核上并行执行。

`tokio::spawn` 则创建独立 Task：

```rust
let user_task = tokio::spawn(fetch_user());
let order_task = tokio::spawn(fetch_orders());

let user = user_task.await??;
let orders = order_task.await??;
```

在多线程 Runtime 中，不同 Task 可能被不同工作线程并行 poll。但是否并行取决于 Runtime、任务是否可迁移，以及代码是否有足够的 CPU 工作。

可以简单区分：

```text
async/await：描述可暂停的计算
concurrency：多个计算在时间上交错推进
parallelism：多个计算在不同 CPU 核上同时执行
```

## 十二、为什么异步代码中不能随便阻塞

异步任务共享少量 Executor 工作线程。如果在 async 函数中执行长时间阻塞：

```rust
async fn bad() {
    std::thread::sleep(Duration::from_secs(10));
}
```

这 10 秒内，当前工作线程无法 poll 其他任务。线程上的所有异步任务都可能出现延迟。

应改用异步定时器：

```rust
async fn good() {
    tokio::time::sleep(Duration::from_secs(10)).await;
}
```

异步 sleep 会注册定时器并返回 `Pending`，工作线程可以继续运行其他任务。

必须执行同步阻塞操作时，可以使用专用阻塞线程池：

```rust
let result = tokio::task::spawn_blocking(|| {
    blocking_computation()
})
.await?;
```

CPU 密集型任务也不会因为加上 `async` 就自动变快。它们应根据场景使用 Rayon、专用线程池、`spawn_blocking` 或其他并行计算方案。

## 十三、常见理解误区

### 1. `await` 会阻塞线程

不会。`await` 在子 Future 返回 `Pending` 时，让当前状态机的 `poll` 返回，把线程还给 Executor。

### 2. Future 创建后会自动运行

通常不会。Future 是惰性的，必须被 `.await`、`block_on` 或 spawn 到 Executor，才会有人 poll 它。

### 3. Waker 会直接执行 Future

通常不会。Waker 负责通知调度器，将任务重新标记为 ready；Executor 之后才调用 `poll`。

### 4. 每个 async Task 对应一个线程

不是。大量 Task 可以复用少量工作线程。Task 保存的是状态机对象，而不是独立系统线程栈。

### 5. 每次 poll 都从函数开头执行

不是。状态机保存了当前状态，下一次 poll 会跳到上次暂停点对应的分支。

### 6. 收到 wake 后下一次 poll 必须 Ready

不是。唤醒只表示“可能取得进展”，Future 仍然可以再次返回 `Pending`。

### 7. `async fn` 本身就是状态机 enum

更准确地说，调用 `async fn` 得到的匿名 Future 类型内部实现了状态机语义。enum 是理解它的常用近似模型，不代表编译器一定按手写 enum 的方式布局。

## 十四、总结

Rust 异步编程可以浓缩为下面几句话：

1. `async fn` 返回一个实现 `Future` 的匿名状态机；
2. 每个 `await` 都是潜在暂停点；
3. 跨越 `await` 的局部变量被保存为状态机字段；
4. Executor 通过 `poll` 主动推进 Future；
5. Future 暂时无法继续时返回 `Pending`，不会阻塞线程；
6. I/O 或定时器就绪后通过 Waker 通知 Executor；
7. Executor 再次 poll，状态机从暂停状态继续；
8. `Pin` 保证可能自引用的状态机在被 poll 后不会移动。

最终执行模型是：

```text
async fn
   │ 编译
   ▼
Future 状态机
   │ Executor 调用
   ▼
poll
   │
   ├── Ready(output) ──> 完成
   │
   └── Pending
          │ 保存 Waker
          ▼
       等待事件
          │ wake
          ▼
       再次 poll
```

因此，状态机不是 Rust 异步实现中的一个附带概念，而是 `async/await` 能够暂停、保存现场并恢复执行的核心机制。Future 描述状态机的推进接口，Executor 负责调度，Waker 负责重新激活，Reactor 负责感知外部事件，几者共同组成了 Rust 的异步运行模型。

理解这些基础后，可以继续阅读 [《Rust Tokio Runtime 原理与最佳实践》](/posts/rust-tokio-runtime-and-best-practices/)，进一步学习 Tokio 的 Runtime 类型、Task 调度、I/O Driver、定时器、取消安全、背压和优雅退出。
