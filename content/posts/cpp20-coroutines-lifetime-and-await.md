---
title: "C++20 协程：理解 co_await、promise 与生命周期"
date: 2026-09-26T22:26:11+08:00
draft: false
description: "用一个可编译的同步生成器，讲清协程帧、promise、等待协议、final_suspend 与句柄所有权，并验证提前退出、移动和异常路径。"
tags: ["C++", "C++20", "协程", "RAII", "异步编程"]
categories: ["C++"]
---

理解 C++20 协程，最重要的是分清三个事件：**挂起、完成、销毁。挂起不等于完成，完成也不一定意味着协程状态已经销毁。**

语言提供的三个关键字是 `co_await`、`co_yield` 和 `co_return`。下面用一个整数生成器串起它们背后的协议，重点回答：状态存在哪里、谁能恢复执行、结果如何交给调用者，以及最后由谁回收资源。

## 一、先分清五个角色

C++20 的协程是一种无栈协程：编译器为当前协程组织可挂起、可恢复的状态，而不是在每次暂停时保存整条普通函数调用栈。

| 角色 | 作用 | 容易混淆的地方 |
| --- | --- | --- |
| 协程帧，coroutine frame | 保存恢复执行需要的状态、promise、参数副本等 | 不等于每次 `co_await` 都把全部局部变量复制到堆上 |
| promise 对象 | 定制启动、产值、完成、异常等行为 | 不是 `<future>` 中的 `std::promise<T>` |
| `std::coroutine_handle<Promise>` | 引用一个协程，提供恢复、观察和销毁操作 | 它不是自动管理生命周期的智能指针 |
| 返回对象 | 提供调用者使用的接口，例如后文的 `IntGenerator` | 协程函数的返回类型不必是 `coroutine_handle` |
| awaiter | 实现一次等待的就绪判断、控制权交接和结果获取 | 源表达式中的 awaitable 不一定直接就是 awaiter |

编译器通过 `std::coroutine_traits<R, Args...>::promise_type` 确定 promise 类型。把 `promise_type` 嵌套在返回类型 `R` 中是常见的默认用法，不是唯一可能；成员协程的类型推导还涉及隐式对象参数。

协程通常需要保存跨挂起点仍要使用的状态，但协程帧不意味着必然发生一次堆分配：编译器可以在满足条件时消除分配。更重要的是，**帧的存在不会取消局部变量原有的作用域和析构规则**。

## 二、co_await 是一套等待协议

对普通 `co_await expression`，编译器可能先调用 promise 的 `await_transform`，再通过 `operator co_await` 得到 awaiter。下面假定 awaiter 已经确定，只看三个核心方法。

1. **`await_ready()`：结果是否已经就绪？** 返回 `true` 时，不进入挂起交接，直接调用 `await_resume()`。
2. **`await_suspend(handle)`：如何交接控制权？** 当 `await_ready()` 返回 `false`，协程在进入这个方法之前就已被视为挂起。
3. **`await_resume()`：等待表达式得到什么？** 它在直接就绪或恢复执行后运行，返回值及值类别决定 `co_await` 表达式的结果，也可以在这里抛出操作异常。

`await_suspend` 不只是一个“保存句柄的回调”。其正常返回时的行为取决于返回类型：

| 返回形式 | 控制流含义 |
| --- | --- |
| `void` | 不通过返回值立即恢复当前协程；控制权交还调用者或恢复者 |
| `bool`，值为 `true` | 与上述挂起交接路径相同 |
| `bool`，值为 `false` | 立即继续当前协程，进入 `await_resume()` |
| `std::coroutine_handle<...>` | 转去恢复返回句柄指向的协程，常用于对称转移 |

因此，不能说“只有显式调用当前句柄的 `resume()`，`co_await` 才能继续”，也不能说“`await_suspend` 返回时绝不会回到当前协程”。就绪路径和返回 `false` 的路径都不符合这种简化。

这套协议也不自带线程池或 I/O 引擎。异步库可以在 `await_suspend` 中注册完成事件，在事件到达后恢复协程；同步生成器则可以把恢复时机完全交给调用者。**`co_await` 本身不保证切换线程，也不会把阻塞调用自动变成异步操作。**

## 三、一个带明确所有权的同步生成器

下面的例子只生成 `int`，只允许单线程驱动，不实现迭代器、取消或异步 I/O。调用者每执行一次 `next()`，就推进到下一次产值或结束；函数体内的异常会在 `next()` 中重新抛出。

完整程序如下，也可以从文末下载同一份源文件。

```cpp
#include <coroutine>
#include <exception>
#include <iostream>
#include <optional>
#include <utility>

class IntGenerator {
public:
    struct promise_type;
    using Handle = std::coroutine_handle<promise_type>;

    struct promise_type {
        int current_value{};
        std::exception_ptr error;

        IntGenerator get_return_object() noexcept {
            return IntGenerator{Handle::from_promise(*this)};
        }
        std::suspend_always initial_suspend() const noexcept { return {}; }
        std::suspend_always final_suspend() const noexcept { return {}; }
        std::suspend_always yield_value(int value) noexcept {
            current_value = value;
            return {};
        }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {
            error = std::current_exception();
        }
        void await_transform() = delete;
    };

    IntGenerator(const IntGenerator&) = delete;
    IntGenerator& operator=(const IntGenerator&) = delete;
    IntGenerator& operator=(IntGenerator&&) = delete;

    IntGenerator(IntGenerator&& other) noexcept
        : handle_(std::exchange(other.handle_, {})) {}

    ~IntGenerator() {
        if (handle_) {
            handle_.destroy();
        }
    }

    std::optional<int> next() {
        if (!handle_ || handle_.done()) {
            return std::nullopt;
        }
        handle_.resume();
        auto& promise = handle_.promise();
        if (promise.error) {
            std::rethrow_exception(promise.error);
        }
        if (handle_.done()) {
            return std::nullopt;
        }
        return promise.current_value;
    }

private:
    explicit IntGenerator(Handle handle) noexcept : handle_(handle) {}
    Handle handle_;
};

IntGenerator count(int limit) {
    for (int value = 0; value < limit; ++value) {
        co_yield value;
    }
    co_return;
}

int main() {
    auto numbers = count(3);
    while (auto value = numbers.next()) {
        std::cout << *value << '\n';
    }
    return 0;
}
```

这里有几个有意作出的限制和选择：

- **先返回对象，再运行函数体。** `initial_suspend()` 返回 `std::suspend_always`，所以 `count(3)` 先建立状态并返回生成器，第一次 `next()` 才开始执行循环。
- **只保留一个所有者。** 删除复制和赋值，只提供移动构造；移动时清空原对象的句柄，避免两个析构函数销毁同一个协程帧。
- **不暴露协程内部引用。** `next()` 返回 `std::optional<int>`，把本次产出的整数复制出去；没有值表示序列结束，而整数 `0` 仍是有效结果。
- **不接受任意挂起。** 删除 `await_transform`，阻止在生成器函数体中使用普通 `co_await`。它不影响初始挂起、最终挂起和 `co_yield` 生成的等待，因此 `next()` 不会把一次与产值无关的暂停误当成新数据。

在本文验证的 GCC 13.3.0 环境中，可以这样编译运行：

```bash
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic int_generator.cpp -o int_generator
./int_generator
```

输出依次是 `0`、`1`、`2`。这只是该示例的已验证环境，不代表某个编译器版本完整支持全部 C++20 特性；旧教程中的 Coroutines TS 编译参数也不应直接作为通用配置照搬。

## 四、产值、完成和释放不是同一件事

在这个生成器中，`co_yield value` 通过 `promise.yield_value(value)` 保存值，然后进入等待协议。`yield_value()` 返回 `std::suspend_always`，所以每次产值后都会暂停，等待调用者再次推进。

`co_return;` 则调用 `promise.return_void()`，随后进入最终挂起流程。若使用非 `void` 表达式的 `co_return value;`，对应的是 `return_value(value)`，而不是函数调用最初得到的那个返回对象。promise 不能同时声明 `return_void` 和 `return_value`。

因为本例提供了 `return_void()`，自然执行到函数体末尾也可以表示结束；不能把这个结论无条件套到所有协程上。

`count(3)` 的完整推进过程是：

```
count(3)
  -> create frame / promise
  -> get_return_object()
  -> initial_suspend                 [paused]
  -> next() -> yield 0               [paused]
  -> next() -> yield 1               [paused]
  -> next() -> yield 2               [paused]
  -> next() -> co_return -> final_suspend [done]
  -> owner destructor -> destroy()   [released]
```

注意最后一次 `next()`：最后一个值已经产出，不等于函数体已经走到结尾。还要再恢复一次，才能执行 `co_return` 并到达最终挂起点。

### final_suspend 决定完成后的交接方式

| 最终等待器 | 完成后发生什么 |
| --- | --- |
| `std::suspend_always` | 停在最终挂起点，帧仍存在。可以由所有者检查完成状态并最终 `destroy()`，但不能再次 `resume()` |
| `std::suspend_never` | 不停在最终挂起点，继续走出协程并销毁状态。外部保存的句柄可能已经悬空，不能再拿它查询或销毁 |

本文选择第一种，让返回对象负责回收。若只把 `final_suspend()` 改成 `std::suspend_never`，却保留 `next()` 中的 `promise()`、`done()` 和析构中的 `destroy()`，就破坏了所有权协议，可能访问已销毁的状态。

`bool(handle)` 只检查地址是否非空，不检查协程是否完成，更不是悬空检测器。`done()` 的前提是句柄仍然有效，并且协程处于挂起状态；它判断的是是否停在最终挂起点。

还需要区分帧和帧内对象的寿命。正常退出函数体时，局部变量会按作用域规则析构，不能因为帧仍停在 `final_suspend` 就继续使用指向这些局部变量的指针。若在中途挂起时销毁协程，仍处于作用域内的自动对象则由销毁过程清理。

异常路径也必须纳入同一套设计。本例的 `unhandled_exception()` 保存函数体内的异常，`next()` 重新抛出；把这个钩子写成空函数，会让调用者难以区分正常结束和失败。

## 五、从语言机制到异步系统，还缺哪些约束

这个生成器的析构安全，建立在“单线程驱动、没有外部待执行回调、最终保持挂起”的明确约定上。不能直接把这种析构方式照搬到任意异步 Task。

- **被引用对象必须活得足够久。** 协程保存引用参数、裸指针或 `this`，不等于拥有它们指向的对象。本例把 `limit` 按值传入，避免了这类外部寿命依赖。
- **交出句柄后要考虑立即恢复。** 进入 `await_suspend` 前，协程已经被视为挂起。如果接收句柄的一方立即恢复甚至销毁协程，awaiter 可能在 `await_suspend` 返回前就被销毁；不能继续无条件访问它的成员。
- **取消需要处理尚未执行的恢复动作。** 不能一边销毁帧，一边让事件队列仍保留可恢复它的裸句柄。排队的续体、并发恢复和销毁之间，需要库提供同步与生命周期协议。

理解这些边界后，promise 就不再是为了让代码通过编译而拼出来的样板：它和返回对象共同定义了协程的运行及资源管理方式。

本文对完整示例验证了正常结束、空序列、十万次产值、未启动即销毁、提前停止、移动所有权，以及产值前后抛异常；另用独立程序检查了就绪路径、`await_suspend` 的 `void`、两种 `bool` 结果和句柄转移路径。测试通过 GCC 13.3.0 编译检查及 ASan/UBSan 检查。当前验证环境不支持 LeakSanitizer 的运行条件，因此关闭了其泄漏检测，**不把这些结果宣称为完整的泄漏或并发安全证明**。

## 参考资料

- [C++20 草案 N4861：协程定义、promise、参数副本与状态销毁](https://timsong-cpp.github.io/cppwp/n4861/dcl.fct.def.coroutine)
- [C++20 草案 N4861：co_await 的等待协议及控制流](https://timsong-cpp.github.io/cppwp/n4861/expr.await)
- [C++20 草案 N4861：coroutine_handle 的观察、恢复和销毁前提](https://timsong-cpp.github.io/cppwp/n4861/coroutine.handle)
- [C++20 草案 N4861：co_return 与 return_void、return_value](https://timsong-cpp.github.io/cppwp/n4861/stmt.return.coroutine)
- [David Mazières：My tutorial and take on C++20 coroutines](https://www.scs.stanford.edu/~dm/blog/c++-coroutines.html)
- [LLVM：协程表示、降低与分配消除](https://llvm.org/docs/Coroutines.html)
- [GCC：C++ 特性支持状态](https://gcc.gnu.org/projects/cxx-status.html)
- [本文完整可编译示例：int_generator.cpp](https://yangyang233333.github.io/code/cpp20-coroutines/int_generator.cpp)
