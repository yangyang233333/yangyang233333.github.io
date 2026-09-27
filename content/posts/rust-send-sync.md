---
title: "Rust Send 与 Sync：从所有权理解线程安全"
date: 2026-09-27T18:35:00+08:00
draft: false
description: "从值的转移和共享引用出发，理解 Send 与 Sync 的区别、Rc 与 RefCell 的限制，以及 Arc<Mutex<T>> 如何支持跨线程共享可变数据。"
tags: ["Rust", "并发", "Send", "Sync", "类型系统"]
categories: ["技术原理"]
---

Rust 中的 `Send` 和 `Sync` 不是发送消息或执行同步的方法，而是编译器用来检查跨线程操作是否安全的两个标记 trait：

- **`Send`：这个类型的值可以安全地转移到另一个线程。**
- **`Sync`：这个类型的值可以安全地通过共享引用被多个线程访问。**

理解它们的起点不是“有没有锁”，而是：跨线程传递的究竟是一个值，还是指向它的共享引用？

## 1. Send：把值交给另一个线程

所有权决定谁负责一个值。以 `String` 为例，转移所有权后，原来的变量不能继续使用这个值。把所有权转移放到两个线程之间，就需要检查 `Send`。

```rust
use std::thread;

fn main() {
    let message = String::from("hello");

    let worker = thread::spawn(move || {
        println!("子线程：{message}");
    });

    worker.join().unwrap();
}
```

`move` 让闭包按值捕获 `message`，随后 `thread::spawn` 把闭包交给子线程。`String` 满足 `Send`，所以这种传递是允许的。主线程不能再通过原来的 `message` 访问它；`join()` 只是等待线程结束，不会自动把所有权还回来。

这里有两个不同的检查：`move` 决定闭包怎样捕获变量，`Send` 决定闭包能否跨线程传递。**写了 `move`，不等于里面的值就自动变得线程安全。**

### 为什么 Rc 不能 Send

`Rc<T>` 允许多个句柄共同拥有同一份数据，并通过引用计数决定何时释放数据。克隆或销毁句柄都会修改这个计数，但它的计数更新不是原子操作。

假设主线程保留一个 `Rc`，把克隆出的另一个 `Rc` 移到子线程。虽然转移出去的句柄有了新主人，两个句柄仍然指向同一个引用计数，两个线程可能同时修改它。因此，`Rc<T>` 不满足 `Send`。

关键不是“这个变量有没有被 move”，而是**移动这个值之后，它关联的共享状态是否仍然安全**。移动一个句柄，不等于独占句柄指向的底层资源。

## 2. Sync：共享引用能否跨线程

借用不转移所有权，而是提供访问一个值的引用。`&T` 是共享引用，`&mut T` 是可变、独占的引用。

`Sync` 的精确定义是：**`T: Sync` 当且仅当 `&T: Send`。**

也就是说，如果指向同一个值的共享引用可以安全地交给不同线程，这个值的类型就是 `Sync`。

```rust
use std::thread;

fn main() {
    let message = String::from("hello");

    thread::scope(|scope| {
        scope.spawn(|| println!("线程 A：{message}"));
        scope.spawn(|| println!("线程 B：{message}"));
    });

    println!("主线程仍然能用：{message}");
}
```

这一次，两个闭包都借用了 `message`，没有取走它的所有权。传给线程的是共享引用 `&String`，因此需要 `String: Sync`。

这里使用 `thread::scope`，是因为它会在返回前等待作用域内的线程完成，允许这些线程借用外面的局部变量。主线程离开这个作用域后，仍然可以使用 `message`。

### 共享引用为什么还需要检查

如果把 `&T` 理解为“底层数据永远不会变化”，就很难理解为什么还需要 `Sync`。更准确的名字是共享引用：某些类型通过内部可变性，允许使用共享引用修改内部状态。

例如，`RefCell<T>` 的 `borrow_mut()` 接收 `&self`，却能在运行时借用检查通过后，提供对内部值的可变访问。它的借用状态不是为多线程并发访问设计的，因此 `RefCell<T>` 不满足 `Sync`。

但 `RefCell<i32>` 满足 `Send`：把整个对象交给另一个线程，由那个线程独自使用，并不会引入这种并发共享问题。它是“可以转移，不可以直接共享”的典型例子。

注意，这里的等价关系专门针对共享引用 `&T`。对于独占引用，规则是 `&mut T: Send` 当且仅当 `T: Send`；借用规则已经限制了其他访问，不能把它与共享引用混为一谈。

## 3. Arc 与 Mutex：分别处理所有权和访问控制

多个线程共同修改一份数据，需要分别解决两个问题：`Arc<T>` 用线程安全的引用计数管理共享所有权，`Mutex<T>` 则通过互斥锁协调对内部数据的访问。

```rust
use std::sync::{Arc, Mutex};
use std::thread;

fn main() {
    let counter = Arc::new(Mutex::new(0));
    let mut workers = Vec::new();

    for _ in 0..4 {
        let shared_counter = Arc::clone(&counter);

        workers.push(thread::spawn(move || {
            let mut value = shared_counter.lock().unwrap();
            *value += 1;
        }));
    }

    for worker in workers {
        worker.join().unwrap();
    }

    println!("{}", *counter.lock().unwrap());
}
```

这个程序输出 `4`，其中每一层各司其职：

1. `Arc::clone` 增加一个持有者，不会复制底层的锁或计数器。
2. `lock()` 返回锁守卫，线程通过守卫访问并修改整数。
3. 守卫离开作用域时释放锁；主线程等待所有工作线程完成后，再读取最终结果。

### Arc 不会把任意类型变成线程安全

对于常用的 `Arc<T>`，要让它满足 `Send + Sync`，内部的 `T` 也需要满足 `Send + Sync`。`Arc` 只保证引用计数安全，不负责同步 `T` 自己的内部状态。

因此，`Arc<RefCell<i32>>` 既不是 `Send`，也不是 `Sync`。外面的原子引用计数，无法保护里面非线程安全的借用状态。

`Mutex<T>` 的条件不同：**它满足 `Send + Sync`，只要求 `T: Send`，不要求 `T: Sync`。** 锁可以让不同线程轮流获得对内部值的独占访问，而不是直接共享一个没有保护的 `&T`。

这解释了为什么 `Mutex<RefCell<i32>>` 可以满足 `Send + Sync`，也解释了为什么互斥锁不是万能补丁：`Mutex<Rc<i32>>` 仍然不行，因为锁外可能还有其他 `Rc` 句柄，共享同一个未受这把锁保护的引用计数。

## 4. 常见类型：两个维度，不是安全等级

下面都使用具体类型，避免漏掉泛型参数带来的条件：

| 类型 | Send | Sync | 关键原因 |
| --- | --- | --- | --- |
| `i32`、`String`、`Vec<i32>` | 是 | 是 | 可以转移，也可以通过共享引用安全访问 |
| `Rc<i32>` | 否 | 否 | 引用计数更新不是原子操作 |
| `RefCell<i32>` | 是 | 否 | 可以独自接管，不能并发共享借用状态 |
| `Arc<i32>` | 是 | 是 | 引用计数安全，内部类型也满足条件 |
| `Arc<RefCell<i32>>` | 否 | 否 | 内部类型不满足 Sync |
| `Mutex<RefCell<i32>>` | 是 | 是 | 锁协调访问，内部类型满足 Send |
| `Arc<Mutex<i32>>` | 是 | 是 | 同时解决共享所有权和互斥访问 |
| `std::sync::MutexGuard<'_, i32>` | 否 | 是 | 可共享借用，但不能转移到另一线程释放 |

最后一行说明 `Sync` 也不意味着 `Send`。标准库的 `MutexGuard` 出于跨平台兼容性，需要遵守在加锁线程释放锁的约束，所以不能跨线程转移；当内部类型满足 `Sync` 时，守卫本身仍然可以满足 `Sync`。

## 5. 编译器如何使用这些标记

`Send` 和 `Sync` 是自动标记 trait，没有需要调用或实现的方法。普通结构体如果所有字段都满足 `Send`，通常会自动满足 `Send`；`Sync` 同理。例如，只包含 `String` 和 `Vec<i32>` 的结构体不需要额外手写实现。

库通过泛型约束使用这些能力。`thread::spawn` 要求传入的闭包满足 `Send`，因为闭包要按值交给新线程；它也要求返回值满足 `Send`，因为返回值可能通过 `join()` 交回另一个线程。

遇到相关编译错误，可以按这个顺序检查：

1. **实际跨线程的是什么类型？** 传递值时看这个类型是否满足 `Send`；传递共享引用 `&T` 时，要继续检查 `T` 是否满足 `Sync`。闭包还需要检查实际捕获的内容。
2. **问题是不是生命周期，而不是线程安全？** `thread::spawn` 还要求闭包满足 `'static`，不能携带可能过早失效的借用。这个约束不意味着值必须活到程序结束；局部拥有的 `String` 就可以满足它。

不要为了消除报错，直接添加 `unsafe impl Send` 或 `unsafe impl Sync`。这不是让编译器帮忙补上保护，而是由实现者承诺所有安全性条件已经成立；承诺错误可能导致未定义行为。

这两个标记本身没有运行时加锁动作，也不保证程序不会死锁或出现业务逻辑上的竞态。它们解决的是类型层面的安全转移和安全共享，具体的同步机制仍然由类型实现。

理解 `Send` 与 `Sync` 后，更有用的习惯不是给类型贴一个笼统的“线程安全”标签，而是分别检查它的所有权如何流转、共享状态由什么规则保护。

## 参考资料

- [The Rust Programming Language：Send 与 Sync](https://doc.rust-lang.org/book/ch16-04-extensible-concurrency-sync-and-send.html)
- [标准库：Send 的定义与实现](https://doc.rust-lang.org/std/marker/trait.Send.html)
- [标准库：Sync 与各种引用的关系](https://doc.rust-lang.org/std/marker/trait.Sync.html)
- [标准库：thread::spawn 的类型约束](https://doc.rust-lang.org/std/thread/fn.spawn.html)
- [标准库：thread::scope 与作用域线程](https://doc.rust-lang.org/std/thread/fn.scope.html)
- [标准库：Arc 的线程安全边界](https://doc.rust-lang.org/std/sync/struct.Arc.html)
- [标准库：Mutex 对内部类型的要求](https://doc.rust-lang.org/std/sync/struct.Mutex.html)
- [标准库：MutexGuard 为什么不满足 Send](https://doc.rust-lang.org/std/sync/struct.MutexGuard.html)
- [The Rustonomicon：Send 与 Sync 的安全契约](https://doc.rust-lang.org/nomicon/send-and-sync.html)
