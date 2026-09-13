---
title: "C++ 实现 SPSC 与 MPSC：从所有权交接到内存序"
date: 2026-09-13T22:48:00+08:00
draft: false
description: "用可编译的 C++17 实现讲清 SPSC 环形队列与 MPSC 链式队列：双向 release/acquire、生产者竞争、发布缺口、节点回收与进度保证。"
tags: ["C++", "并发", "内存模型", "队列"]
categories: ["C++"]
---

SPSC 是单生产者、单消费者；MPSC 是多生产者、单消费者。它们描述的是线程访问约束，不代表一定无锁，也不决定队列必须用数组还是链表。

实现并发队列，关键不是把下标换成 `atomic`，而是回答三个问题：**谁能写这块内存？写完怎样交给别人？什么时候可以复用或释放？**

本文实现两个存储 `std::uint64_t` 的 C++17 队列：有界 SPSC 环形队列，以及基于原子交换的无界 MPSC 链式队列。固定元素类型是为了把注意力放在同步协议上，而不是泛型对象的构造、异常和析构。两者都要求构造完成后再交给工作线程，析构前停止并等待所有访问者退出。

## 一、SPSC：把共享队列拆成两块单写者状态

环形队列维护两个下标：`write_index` 指向下一个可写槽位，`read_index` 指向下一个可读槽位。只有生产者修改前者，只有消费者修改后者；另一方只读取。

这种单写者约束消除了“两个线程抢同一个写下标”的问题，因此更新下标不需要 CAS，也不需要 `fetch_add`。但下标仍然被跨线程读写，需要是原子变量。

使用 `Slots` 个物理槽位，始终留一个空位：

| 状态 | 判定 |
| --- | --- |
| 空 | `read_index == write_index` |
| 满 | `next(write_index) == read_index` |
| 实际容量 | `Slots - 1` |

例如物理槽位为 8 个，当前读下标是 2、写下标是 5：

```text
slot:          0    1    2    3    4    5    6    7
contents:      .    .    A    B    C    .    .    .
                        ^              ^
                   read_index     write_index

readable:      [2, 5)
next push:     slot 5
next pop:      slot 2
```

留空位不是同步手段，只是用最简单的编码区分空和满。真正的同步发生在下标发布时。

### 两条方向相反的交接链

生产者先写入元素，再以 release 更新写下标；消费者用 acquire 读取这个下标，确认元素已发布后才读取槽位。

```text
producer                              consumer
write slots[position]
write_index.store(next, release) ---> write_index.load(acquire)
                                      read slots[position]
```

这里的箭头有条件：acquire 必须读到对应 release 发布的值，或符合内存模型规则的后续发布。它不是“任意 release 和任意 acquire 自动配对”。由此建立的 happens-before 关系，保证消费者不会在尚未获准访问时读取该元素。

还有一条经常被忽略的反向链：消费者读完之后，要通知生产者这个槽位可以重新写入。

```text
consumer                              producer
read slots[position]
read_index.store(next, release) ----> read_index.load(acquire)
                                      overwrite released slot
```

第一条链防止“没写完就读”，第二条链防止“没读完就覆盖”。所以槽位本身可以是普通 `uint64_t`：冲突的访问已经按交接协议排序，而不是靠“单次整数读写通常不可分割”来碰运气。

### 完整实现

```cpp
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

template <std::size_t Slots>
class SpscQueue {
    static_assert(Slots >= 2);

    static std::size_t next(std::size_t index) noexcept {
        return index + 1 == Slots ? 0 : index + 1;
    }

    alignas(64) std::array<std::uint64_t, Slots> slots_{};
    alignas(64) std::atomic<std::size_t> write_index_{0};
    alignas(64) std::atomic<std::size_t> read_index_{0};

public:
    bool try_push(std::uint64_t value) noexcept {
        const auto write = write_index_.load(std::memory_order_relaxed);
        const auto next_write = next(write);
        if (next_write == read_index_.load(std::memory_order_acquire)) {
            return false;
        }

        slots_[write] = value;
        write_index_.store(next_write, std::memory_order_release);
        return true;
    }

    bool try_pop(std::uint64_t& value) noexcept {
        const auto read = read_index_.load(std::memory_order_relaxed);
        if (read == write_index_.load(std::memory_order_acquire)) {
            return false;
        }

        value = slots_[read];
        read_index_.store(next(read), std::memory_order_release);
        return true;
    }
};
```

`SpscQueue<1024>` 实际容纳 1023 个元素。失败不会改变队列；`try_pop` 失败也不会修改输出参数。下标显式回绕，不依赖无限增长的计数器，也不要求槽位数量是 2 的幂。

读取自己负责的下标只需 relaxed：同一线程知道自己上次写到了哪里，不需要从这个读取操作取得对方的数据。读取对方下标则用 acquire，因为它是使用槽位的许可。

即使读到对方较旧的下标，也只会保守地暂时判满或判空，不会据此越过安全边界。这里还依赖原子变量的单一修改顺序与同一线程连续访问的 coherence 约束，不能把这一点理解为允许任意倒退、任意读取旧一轮下标。

`alignas(64)` 只是按常见的 64 字节缓存行布局，减少两个频繁写入的下标发生伪共享，不参与正确性证明；缓存行大小不是 C++ 保证的常量，也不消除真正共享的数据流量。

## 二、为什么不能把 SPSC 的下标改成 fetch_add 就得到 MPSC

多生产者首先面临所有权竞争。两个线程都读到写下标为 5，就可能同时写槽位 5。`fetch_add` 确实能分配不同的逻辑位置，但**取得位置不等于写完数据**。

```text
P1: reserve position 5 ---- paused ---- write slot 5
P2: reserve position 6 -> write slot 6 -> return
C : sees reservation cursor == 7
    slot 5 may still be uninitialized
```

SPSC 的写下标可以兼任发布边界，是因为唯一生产者按顺序写完后才推进它。多个生产者会乱序完成，这两个含义必须拆开。

环形 MPSC 的常见办法是为每个槽位增加带轮次的 sequence：生产者先取得位置，写完后 release 发布该槽位；消费者 acquire 确认这一轮的数据准备好，读取后再发布下一轮可复用状态。简单的布尔 ready 不足以独自解决槽位多轮复用和所有权分配；满队列处理、计数器回绕以及领取位置后暂停也都需要协议。

下面改用链表，把“领取位置”和“连接可读数据”显式写成两步，更容易看清多生产者的核心难点。

## 三、MPSC：用 exchange 决定前驱，用 next 发布数据

队列中始终保留一个哨兵节点。`producer_head_` 指向生产者最近取得的节点，多个生产者共同更新；`consumer_tail_` 是消费者当前哨兵，仅消费者访问。

```text
consumer_tail_                          producer_head_
      |                                       |
      v                                       v
   [dummy] ----------> [A] ----------> [B]

next pop returns A
old dummy is deleted; node A becomes the new dummy
```

生产者先分配、初始化新节点，然后执行：

1. `producer_head_.exchange(node)`：原子地把自己放到末端，并拿到唯一前驱。
2. `previous->next.store(node, release)`：把前驱和自己连接起来，让消费者能够沿链表读到数据。

原子 RMW 总是读取该原子对象修改顺序中的紧邻前值，所以不同生产者拿到的前驱自然组成一个顺序，不会同时抢到同一条待连接的边。同一个生产者连续调用的入队顺序也得到保留。

### 完整实现

```cpp
#include <atomic>
#include <cstdint>

class MpscQueue {
    struct Node {
        std::atomic<Node*> next{nullptr};
        std::uint64_t value;

        explicit Node(std::uint64_t initial) noexcept : value(initial) {}
    };

    alignas(64) std::atomic<Node*> producer_head_;
    alignas(64) Node* consumer_tail_;

public:
    enum class PopResult { item, empty, retry };

    MpscQueue() : producer_head_(new Node(0)),
                  consumer_tail_(producer_head_.load(
                      std::memory_order_relaxed)) {}

    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    ~MpscQueue() {
        Node* current = consumer_tail_;
        while (current != nullptr) {
            Node* next = current->next.load(std::memory_order_relaxed);
            delete current;
            current = next;
        }
    }

    void push(std::uint64_t value) {
        Node* node = new Node(value);
        Node* previous = producer_head_.exchange(
            node, std::memory_order_acq_rel);
        previous->next.store(node, std::memory_order_release);
    }

    PopResult try_pop(std::uint64_t& value) noexcept {
        Node* previous = consumer_tail_;
        Node* next = previous->next.load(std::memory_order_acquire);

        if (next == nullptr) {
            if (producer_head_.load(std::memory_order_acquire) == previous) {
                return PopResult::empty;
            }
            return PopResult::retry;
        }

        value = next->value;
        consumer_tail_ = next;
        delete previous;
        return PopResult::item;
    }
};
```

这是无固定容量上限的教学实现，不是无限内存。每次 `push` 分配一个节点，分配失败会在修改队列之前抛出异常；成功出队删除旧哨兵，最后一个哨兵由析构函数释放。析构要求所有生产者和消费者已经退出，不能与入队、出队并发。

### 两类同步不能混为一谈

`exchange(acq_rel)` 负责生产者之间的交接。release 把新节点的初始化交给后继生产者；后继通过 acquire 拿到它作为前驱后，才可以访问其 `next` 成员。初始化原子成员及其对象生命周期本身也需要正确排序，不能认为“`next` 是 atomic，初始化就无需发布”。

`previous->next.store(release)` 与消费者的 `next.load(acquire)` 负责数据发布。消费者沿这条链接取得新节点后，才读取普通成员 `value`。只 acquire 读取生产者端的 head，不能证明从消费者当前位置到 head 的整条链都已经接好。

### 为什么出队删除的是旧哨兵

当消费者读取到 `previous->next != nullptr`，说明负责这条边的生产者已经完成链接发布，并且之后不再访问 `previous`。head 也已经前进，未来生产者不会再拿它当自己的前驱，因此旧哨兵可以释放。

但刚刚返回数值的 `next` 节点不能立即删除：它可能仍是 head，后续生产者还要往它的 `next` 写链接。所以它要留下来充当新哨兵，等自己的后继接好后再释放。

这一回收规则依赖单消费者、生产者发布链接后不再触碰前驱、没有额外窥视节点的读者。它不能直接推广到 MPMC，也不能通过随手加一个节点池就假定所有回收问题都消失了。

## 四、MPSC 的发布缺口：有数据，却暂时取不到

`exchange` 和连接前驱不是同一个原子操作，中间可能被调度器打断：

```text
initial: tail -> dummy; head -> dummy

P1: exchange(A) returns dummy
P1: paused before dummy.next = A

P2: exchange(B) returns A
P2: A.next = B; push(B) returns

visible links:  tail -> dummy -> null
unreachable:          A -> B -> null
head:                      ^
```

此时 B 的入队调用已经返回，但消费者仍然不能越过 dummy。缺的不是 B 的数据，而是 `dummy -> A` 这条连接。

因此接口用三个返回值，而不是把所有失败都叫“空”：

| 返回值 | 含义 | 调用方处理 |
| --- | --- | --- |
| `item` | 读到一个元素 | 使用输出参数 |
| `empty` | 本次观察到 tail 没有后继且 head 仍是 tail | 暂无可读元素，不代表未来不会入队 |
| `retry` | tail 没有可见后继，但 head 已前进 | 稍后重试，不能当作已排空 |

`retry` 也可能来自跨两次读取的并发变化：读 `next` 时链接尚未可见，读 head 时生产者已经接完。因此它表达的是“这次没拿到，需要重试”，不是对某个精确瞬间结构状态的强断言。

如果直接把缺口返回成普通 `empty`，就不能宣称满足严格的线性化 FIFO 空队列语义：上面的 B 已在本次出队开始前成功入队，出队却说没有元素。保留 `retry` 是承认协议的中间状态，不是通过换一个名字就得到普通线性化队列接口。

消费者得到元素的顺序仍然遵循生产者 exchange 的顺序，但暂时取不到元素时，它可能依赖某个生产者恢复运行。不能看到 `retry` 就永久退出或无条件睡眠；真正的阻塞封装还需要配套通知和避免丢失唤醒的检查协议。

### 没有 mutex，不等于整个队列 lock-free

这里需要区分三个说法：

- wait-free：每个操作都能在有界的自身步骤内完成。
- lock-free：允许某些线程饥饿，但系统整体仍保证操作取得进展。
- blocking：某个线程停住，可能阻止其他线程完成所需进展；不一定使用 mutex。

SPSC 的单次 `try_push`、`try_pop` 没有重试循环，也没有动态分配。在原子 load/store 具有相应有界执行保证的实现上，队列协议是 wait-free 的；“不断重试直到成功入队”的外层循环则不是。C++ 的原子类型并非在所有平台上都 lock-free，需要检查目标实现。

MPSC 的生产者链接协议固定执行一次 exchange 和一次 store，没有算法层 CAS 重试循环；但 `new`、原子操作的底层实现，以及消费者侧的 `delete` 都可能影响端到端进度。尤其是 P1 若一直不恢复，消费者就一直无法取到后面的 A、B。即使每次轮询都能迅速返回 `retry`，也不能据此把它包装成严格 lock-free 的成功出队保证。

把所有内存序改成 `seq_cst` 也填不上这个缺口：更强的排序不会把尚未执行的 `dummy.next = A` 自动执行掉。这是算法分成两步造成的，不是缓存刷新问题。

## 五、怎样验证，而不是只看“运行没崩”

测试至少要覆盖数据正确性、槽位复用和人为制造的中间状态。

SPSC 用容量很小的队列反复回绕：先验证空、满、失败不改变输出，再让生产者发送递增序列，消费者逐项比较，检查是否丢失、重复或乱序。

MPSC 给每个生产者编码独立编号和递增序号，验证总数量、唯一性、各生产者内部顺序。不能按多个线程记录日志的先后，凭空推断全局入队顺序：真正的排序发生在原子 exchange，而不是某一行打印语句。

发布缺口要通过确定性测试覆盖：只在测试副本里加暂停点，让 P1 完成 exchange 后停住；让 P2 完整入队；此时断言消费者返回 `retry`，再恢复 P1，验证按 A、B 顺序取出。随机压力测试不保证碰到这个窗口。

把本文两个类保存到同一个 `queue_test.cpp`，再加入自己的测试入口，可以分别做优化构建、内存检查和数据竞争检查；不要把 ASan 与 TSan 合并到一个二进制里。

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread \
  queue_test.cpp -o queue_test
./queue_test

g++ -std=c++17 -O1 -g -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  queue_test.cpp -o queue_asan
./queue_asan

g++ -std=c++17 -O1 -g -pthread \
  -fsanitize=thread -fno-omit-frame-pointer \
  queue_test.cpp -o queue_tsan
./queue_tsan
```

Sanitizer 无报错也不等于内存模型证明：某些错误只会在特定硬件或交错下暴露。最终仍要逐条检查普通内存访问对应的 happens-before，以及节点释放时是否还有访问者。

本文用 GCC 13.3、C++17 验证：SPSC 在 2、3、8、1024 个物理槽位下分别传输 20 万个元素，MPSC 在 1、2、8 个生产者下分别验证每线程 5 万个元素，并通过边界、析构与强制发布缺口测试。优化构建、ASan/UBSan 和 TSan 检查均通过；由于运行环境不支持 LeakSanitizer，本次 ASan 设置了 `ASAN_OPTIONS=detect_leaks=0`，不声称通过泄漏检测。测试结果只能覆盖实际执行的路径，不能替代上述推导。

## 六、实现选择的边界

若只是需要正确传递任务，`std::mutex + std::queue` 完全可以作为 MPSC 的第一版：锁覆盖队列操作，需要阻塞等待时再加入带谓词的条件变量。先证明锁竞争是瓶颈，再承担复杂协议的成本。

| 方案 | 适用前提 | 主要代价 |
| --- | --- | --- |
| SPSC 环形队列 | 恰好一个生产者、一个消费者 | 容量固定，满时需要背压 |
| 本文 MPSC 链式队列 | 多个生产者、一个消费者，能处理 retry | 分配与回收成本、发布缺口、内存增长 |
| 多条 SPSC，由消费者轮询 | 能为生产者分配独立队列 | 轮询和公平性成本，没有天然跨队列总 FIFO |
| 加锁队列 | 更重视简单接口和阻塞等待 | 竞争时需要等待持锁线程 |

如果改成存储泛型 `T`，还必须明确构造和移动失败时的状态，以及已消费对象何时析构；如果改成有界 MPSC，则必须补齐满队列与预留位置的协议。这些不是把 `uint64_t` 替换成模板参数或把链表换成数组就能自动获得的。

记住这条主线就够了：**SPSC 用单写者下标交接槽位；MPSC 还要解决生产者之间的所有权竞争，并把预留与发布分开。** 内存序只负责让既定协议成立，不能替代协议本身。

## 参考资料

- [C++ 标准草案：原子操作的顺序、一致性与 release/acquire 同步](https://eel.is/c++draft/atomics.order)
- [C++ 标准草案：多线程执行、happens-before 与数据竞争](https://eel.is/c++draft/intro.races)
- [Boost.Lockfree 的 SPSC 实现：spsc_queue.hpp](https://github.com/boostorg/lockfree/blob/develop/include/boost/lockfree/spsc_queue.hpp)
- [gRPC 的 MPSC 实现：mpscq.cc 中的 Push、Pop 与发布缺口处理](https://github.com/grpc/grpc/blob/master/src/core/util/mpscq.cc)

参考实现用于核对设计思路；本文是围绕固定数值类型重新编写的教学版本，接口和节点回收方式不与这些库逐项等同。
