---
title: "从 Store Buffer 与 Invalidate Queue 理解 C++ 内存序"
date: 2026-09-01T14:45:00+08:00
draft: false
tags: ["C++", "并发编程", "内存模型", "CPU 架构"]
categories: ["技术原理"]
---

多核程序最难理解的地方，不是“多个线程同时执行”，而是不同核心可能以不同时间、不同顺序观察到内存操作。C++ 原子内存序就是程序员与编译器、处理器之间的约定：哪些重排被允许，哪些读写必须建立跨线程的先后关系。

本文重点解释四种常见内存序：`relaxed`、`release`、`acquire` 和 `seq_cst`，并把它们与 CPU 中的 Store Buffer、Invalidate Queue 联系起来。先给出结论：

- `relaxed` 只保证单次原子访问不可分割，不负责发布其他数据；
- `release` 用于发布，保证它前面的操作不会越过发布点跑到后面；
- `acquire` 用于接收，保证它后面的操作不会越过接收点跑到前面；
- 当 acquire 读到 release 写入的值时，两线程之间建立 `synchronizes-with`，进而形成 `happens-before`；
- `seq_cst` 在 acquire/release 的基础上，再要求所有 `seq_cst` 操作进入一个全局一致的单一顺序。

## 1. 为什么原子性还不够

看一个典型的消息发布程序：

```cpp
#include <atomic>
#include <cassert>

int payload = 0;
std::atomic<bool> ready{false};

void producer() {
    payload = 42;
    ready.store(true, std::memory_order_release);
}

void consumer() {
    while (!ready.load(std::memory_order_acquire)) {
    }
    assert(payload == 42);
}
```

`payload` 不是原子变量，但这个程序没有数据竞争。原因不是 `ready` 本身具有原子性，而是 release/acquire 建立了下面这条关系链：

```text
producer                                      consumer

payload = 42
     │ sequenced-before                            ▲
     ▼                                             │ happens-before
ready.store(true, release) ─ synchronizes-with ─> ready.load(acquire)
                                                   │ sequenced-before
                                                   ▼
                                              read payload
```

如果 acquire load 读到了 release store 写入的 `true`，那么 release 之前的操作 happens-before acquire 之后的操作。因此 consumer 看到 `ready == true` 后，也必须看到此前发布的 `payload == 42`。

把 `ready` 改成普通变量会产生数据竞争；把两端都改成 `relaxed`，虽然 `ready` 的访问仍然原子，但 `payload` 不再受跨线程顺序保护。

## 2. Release 与 acquire 分别约束什么

### Release：把此前结果推出去

```cpp
flag.store(true, std::memory_order_release);
```

release 操作是单向屏障：当前线程中位于它之前的读写，不能在语言允许的执行中被移到 release 之后。它适合表示“数据已经准备好”。

release 不要求后续操作留在它后面，也不会自动让另一个线程同步。只有另一个线程执行匹配的 acquire，并读到该 release 或其 release sequence 中的值，跨线程关系才成立。

### Acquire：在确认发布后再继续

```cpp
bool value = flag.load(std::memory_order_acquire);
```

acquire 同样是单向屏障：当前线程中位于它之后的读写，不能被移到 acquire 之前。它适合表示“确认数据已经发布，现在可以使用”。

acquire 也不是“刷新整个缓存”。现代缓存一致性协议本来就在维护单个缓存行的一致性；acquire 的核心语义是限制可观察顺序，并在读到对应 release 时接收其发布的写入。

### 两者必须通过读到的值配对

仅仅一个线程用了 release、另一个线程用了 acquire，不代表它们必然同步：

```cpp
std::atomic<int> state{0};

// 线程 A
state.store(1, std::memory_order_release);

// 线程 B
int value = state.load(std::memory_order_acquire);
```

只有当线程 B 的 load 读到线程 A 的写入，或标准规定的对应 release sequence 时，才建立 `synchronizes-with`。如果 B 读到的是初始值 `0`，就没有通过这次访问获得 A 之前写入的其他数据。

## 3. Store Buffer 为什么存在

如果核心每次写内存都必须等到缓存一致性协议完成，流水线会频繁停顿。为隐藏写入延迟，处理器通常在核心和缓存层次之间放置 Store Buffer：

```text
             Core 0                         Core 1
        ┌─────────────┐                ┌─────────────┐
        │ Load/Store  │                │ Load/Store  │
        └──────┬──────┘                └──────┬──────┘
               │                              │
        ┌──────▼──────┐                ┌──────▼──────┐
        │Store Buffer │                │Store Buffer │
        └──────┬──────┘                └──────┬──────┘
               └──────── Cache Coherence ─────┘
```

核心执行 store 时，可以先把“地址和值”放入自己的 Store Buffer，然后继续执行。待目标缓存行取得适当的一致性权限后，写入才真正进入缓存并对其他核心可见。

这会产生 store→load 重排的效果。经典例子是：

```cpp
// 初始：x = 0, y = 0

// Core 0                  // Core 1
x = 1;                     y = 1;
r0 = y;                    r1 = x;
```

如果两个 store 都暂存在各自的 Store Buffer 中，而后面的 load 读取了另一个仍为旧值的缓存行，就可能得到 `r0 == 0 && r1 == 0`。从每个核心的局部视角看，它自己的写已经执行；从另一个核心的视角看，该写却尚未可见。

处理器还会进行 store-to-load forwarding：本核心读取一个尚在 Store Buffer 中的同地址写入时，可以直接得到新值。因此“自己已经读到新值”和“其他核心已经看到新值”完全是两件事。

## 4. Invalidate Queue 又解决什么问题

在典型的写无效缓存一致性协议中，一个核心要独占修改某缓存行，需要其他核心使自己的副本失效。若接收无效化消息的核心必须先完成全部相关流水线工作再回复确认，发送者就可能等待很久。

一种常见的微架构优化，是先确认消息，再把待处理的失效操作放入 Invalidate Queue，稍后完成：

```text
Core 0                         Core 1
store x
  │
  ├── 请求独占缓存行 ────────> 收到 invalidate
  │                            │
  │<──── 快速返回确认 ─────────┤
  │                            ▼
  │                      Invalidate Queue
  │                            │
  ▼                            ▼
写入可继续                 稍后真正失效旧副本
```

这减少了写入方的等待，却意味着接收方在短时间内仍可能从尚未处理失效的缓存行读取旧值。概念上说，Store Buffer 延迟了“我的写何时被别人看见”，Invalidate Queue 延迟了“我何时停止使用旧副本”。两者共同解释了为何跨核可见性不能只靠源代码顺序来推断。

需要注意：Store Buffer 和 Invalidate Queue 是理解弱内存行为的硬件模型，不是 C++ 标准的一部分，也不是所有 CPU 都以完全相同的结构实现。C++ 程序应依据语言内存模型证明正确性，而不是依赖某款处理器恰好如何排空队列。

## 5. Acquire/release 与这两个队列的关系

可以用下面的直觉理解二者，但不要把它当作逐条机器指令的精确定义：

- release 必须保证发布标志不能先于此前的数据写入而对观察者生效；从 Store Buffer 的角度看，不能让后来的发布越过更早的相关写入；
- acquire 必须保证确认发布后，后续读取不能继续使用在同步点之前获得的陈旧观察；从 Invalidate Queue 的角度看，需要尊重此前应当生效的一致性消息；
- 不同架构实现这些约束的成本不同，编译器可能只需禁止编译期重排，也可能需要发出屏障或使用带顺序语义的原子指令。

在 x86-64 的强内存模型上，普通 acquire load 和 release store 通常不需要额外的硬件屏障，主要依靠架构已有的顺序保证并阻止编译器重排。但这不表示 acquire/release 没有作用：它们仍然决定 C++ 层面的 happens-before，也是编译器优化必须遵守的约束。

在 ARM、POWER 等更弱的内存模型上，编译器通常需要选择带 acquire/release 语义的加载、存储指令，或插入适当屏障。具体映射由架构版本、编译器和操作宽度决定。

## 6. Relaxed：只要原子性，不要跨线程排序

`std::memory_order_relaxed` 提供：

1. 对该原子对象的访问仍然是原子的，不会发生撕裂；
2. 该原子对象仍有自己的 modification order，即所有线程对它的修改存在一致顺序；
3. 不建立用于发布普通数据的 `synchronizes-with`，也不限制周围普通内存操作的跨原子排序。

最适合 relaxed 的例子是统计计数：

```cpp
std::atomic<std::uint64_t> request_count{0};

void record_request() {
    request_count.fetch_add(1, std::memory_order_relaxed);
}
```

如果计数只用于监控，不承担“计数增加后某份数据一定可见”的同步职责，那么 relaxed 足够，而且准确表达了意图。

另一个常见用途是引用计数中的递增：对象既然已经通过其他机制安全取得，递增只需原子性；但递减到零并销毁对象往往需要更强的顺序，以保证析构线程看到其他线程此前对对象的操作。

不要用 relaxed 发布指针：

```cpp
Widget* object = nullptr;
std::atomic<bool> initialized{false};

// 错误示意：relaxed 不发布 object 指向内容的初始化
object = new Widget;
initialized.store(true, std::memory_order_relaxed);
```

另一个线程即使通过 relaxed load 看见 `true`，也不能据此安全读取非原子的 `object` 及其内容。这里应使用 release/acquire，或更直接地用一个原子指针发布对象。

## 7. Seq_cst：再增加一个全局单一顺序

标准名称是 `seq_cst`，即 sequential consistency；`sqe_cst` 是常见拼写错误。

`std::memory_order_seq_cst` 是原子操作的默认内存序。对 load，它至少具有 acquire 语义；对 store，它至少具有 release 语义；对读改写操作，它同时具有两者。更关键的是，所有 `seq_cst` 操作还必须能放入一个所有线程一致同意的全局顺序中，并满足标准规定的顺序约束。

考虑 Store Buffering 测试：

```cpp
std::atomic<int> x{0};
std::atomic<int> y{0};

// 线程 0                               // 线程 1
x.store(1, std::memory_order_seq_cst);   y.store(1, std::memory_order_seq_cst);
int r0 = y.load(std::memory_order_seq_cst);
                                       int r1 = x.load(std::memory_order_seq_cst);
```

结果 `r0 == 0 && r1 == 0` 在 `seq_cst` 下不允许。假设它发生：

- `r0 == 0` 要求线程 0 对 `y` 的 load 排在 `y.store(1)` 之前；
- `r1 == 0` 要求线程 1 对 `x` 的 load 排在 `x.store(1)` 之前；
- 每个线程自身又要求 store 排在随后的 load 之前。

把四条关系串起来会形成环，不可能放入一个全局总序。

若四个操作都改为 relaxed，这个结果可以被允许，因为只要求 `x` 和 `y` 各自的 modification order 一致，不要求跨两个原子对象形成统一顺序。

### Seq_cst 不等于“所有内存立即同步”

`seq_cst` 的全局顺序只直接约束 `seq_cst` 原子操作，并通过 happens-before 等规则影响其他访问。它不是清空所有 CPU 缓存的命令，也不会把有数据竞争的普通变量程序自动修好。

### Seq_cst 的代价

在弱内存架构上，`seq_cst` 可能需要更强屏障。在 x86-64 上，`seq_cst` store 通常也比 release store 更昂贵，因为实现必须处理 Store Buffer 带来的 store→load 可见顺序问题。实际指令选择依赖编译器和上下文，不能简单记成固定的一条 fence。

## 8. 四种内存序如何选择

| 内存序 | 保证 | 典型用途 |
|---|---|---|
| `relaxed` | 原子性与该对象的修改顺序 | 统计计数、无需发布语义的状态 |
| `release` | 此前操作不能越过发布点 | 发布数据、解锁、生产者提交 |
| `acquire` | 此后操作不能越过接收点 | 消费已发布数据、加锁成功 |
| `seq_cst` | acquire/release 等语义，加全局单一顺序 | 优先正确性、需要跨原子统一推理 |

工程上可以遵循以下顺序：

1. 默认先用 `seq_cst` 写出正确程序；
2. 确认同步模式是明确的单向发布/接收后，再考虑 release/acquire；
3. 只有当某个原子变量不承担发布职责时，才考虑 relaxed；
4. 用基准测试证明减弱内存序确实解决了实际性能问题；
5. 用 ThreadSanitizer、并发压力测试和目标架构测试辅助验证，但不要把“测试没失败”当作内存模型证明。

## 9. 一个容易遗漏的边界：fence 与原子操作不同

C++ 还提供 `std::atomic_thread_fence`。fence 可以约束 fence 两侧的操作，但仅放置一对 acquire/release fence，并不会凭空建立线程间同步；仍需要合适的原子读写作为通信载体，并满足标准对 fence 同步的条件。

因此，大多数消息发布代码优先把语义直接放在通信原子上：

```cpp
ready.store(true, std::memory_order_release);
ready.load(std::memory_order_acquire);
```

这比“relaxed 原子 + 手写 fence”更容易审查，也更不容易配错。

## 总结

Store Buffer 让核心可以先继续执行、稍后再让写入对其他核心可见；Invalidate Queue 让核心可以先确认一致性消息、稍后再处理旧缓存副本。它们提高了性能，也破坏了“源码先写，所以其他核心一定先看到”的直觉。

C++ 内存序在语言层面对这种复杂性建立了可移植规则：relaxed 只保留原子性；release 发布此前操作；acquire 接收已发布结果；两者通过读到的值建立 happens-before；seq_cst 则进一步为所有顺序一致原子操作建立全局单一顺序。

最可靠的原则不是记忆某条 CPU 指令，而是先画出程序需要的 happens-before，再选择刚好能够建立这条关系的内存序。
