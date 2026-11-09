---
title: "Rust 迭代器：从 next 到零成本抽象"
date: 2026-11-09T22:42:00+08:00
draft: false
tags: ["Rust", "Iterator", "所有权", "零成本抽象"]
categories: ["Rust", "编程语言"]
summary: "从 next 的状态推进出发，理解 Iterator 与 IntoIterator、借用与移动、惰性求值、短路消费，以及迭代器零成本抽象的边界。"
---

Rust 的迭代器不难用，难的是解释清楚：为什么 `filter` 的参数有时是双重引用？为什么调用了 `map`，闭包却没运行？为什么 `collect` 可以得到一个 `Result`，而不只是一个容器？

把迭代器看成**一个保存状态、按需交出下一个值的对象**，这些问题就能串起来。方法链只是外观，真正值得跟踪的是三件事：状态走到了哪里，交出的值是什么类型，以及谁在推动计算。

## 从 next 看：迭代器不是容器

`Iterator` 的核心接口只有一个关联类型和一个必需方法。省略默认方法后，可以写成：

```rust
trait Iterator {
    type Item;

    fn next(&mut self) -> Option<Self::Item>;
}
```

`Item` 决定每次交出的值是什么；`&mut self` 说明取下一个值通常会推进内部状态；`Option` 则告诉调用方，这次有没有取到值。

看一个切片迭代器：

```rust
let values = [10, 20, 30];
let mut cursor = values.iter();

assert_eq!(cursor.next(), Some(&10));
assert_eq!(cursor.next(), Some(&20));
assert_eq!(cursor.as_slice(), &[30]);
assert_eq!(values, [10, 20, 30]);
```

改变的是 `cursor` 的位置，不是原数组。这个迭代器的 `Item` 是 `&i32`，因此交出的是引用。这里的 `mut` 修饰迭代器本身，不代表允许修改元素。

容器负责存储，迭代器负责推进，两者不是一回事。范围可以生成数字，字符串可以按字符迭代，自定义对象也可以按需计算结果，背后不一定有一个已经装满元素的容器。

`IntoIterator` 解决的是另一个问题：**怎样把一个值变成迭代器**。它的 `into_iter(self)` 返回某个实现了 `Iterator` 的具体类型。

`for item in expression` 会先对 `expression` 调用 `IntoIterator::into_iter`，再反复调用 `next`，遇到 `None` 就退出。已经实现 `Iterator` 的类型，也有相应的 `IntoIterator` 实现，所以集合和迭代器都能放在 `for` 后面。

这里先留一个边界：普通 `Iterator` 不保证第一次返回 `None` 之后永远返回 `None`。这个更强的承诺属于后面会讲到的 `FusedIterator`。

## 所有权问题，先看 Item 和接收者

对 `Vec<T>`，最常用的三种遍历方式是：

| 写法 | 迭代器的 Item | 对原 Vec 的影响 |
| --- | --- | --- |
| `values.iter()` | `&T` | 共享借用，不能通过该引用修改元素 |
| `values.iter_mut()` | `&mut T` | 独占借用，可以修改已有元素 |
| `values.into_iter()` | `T` | 移动 Vec，逐个交出元素的所有权 |

对应的 `for` 写法分别是 `for value in &values`、`for value in &mut values` 和 `for value in values`。借用产生的限制持续到相关借用不再被使用；如果把元素引用收集起来，借用也可能随着结果继续存在。

但不要把表格记成“`into_iter` 永远拿走容器”。`(&values).into_iter()` 的接收者是 `&Vec<T>`，它交出的仍然是 `&T`。方法名相同，真正参与 trait 实现选择的是接收者类型。

`iter()`、`iter_mut()` 是常见集合提供的方法约定，不是 `Iterator` 要求每个类型实现的两个方法。写泛型接口时，若希望调用方可以传入集合或现成的迭代器，通常可以接收 `IntoIterator`，再约束它的 `Item`。

### 为什么 filter 会多一层引用？

`map` 的闭包接收 `Self::Item`；`filter` 的谓词接收 `&Self::Item`，但筛选后交出的仍是原来的 `Item`。后者只借用当前值做判断，不把值交给谓词处置。

所以 `values.iter()` 的 `Item` 已经是 `&i32` 时，`filter` 的参数就成了 `&&i32`：

```rust
let values = [3, 6, 9, 12];
let selected: Vec<i32> = values
    .iter()
    .filter(|value: &&i32| **value >= 9)
    .copied()
    .collect();

assert_eq!(selected, vec![9, 12]);
```

这里故意写出了类型。`filter` 之后仍然得到 `&i32`，直到 `copied()` 才变成 `i32`。如果先调用 `copied()`，后面的 `filter` 参数就会是 `&i32`。

`copied()` 要求元素满足 `Copy`；`cloned()` 则调用 `Clone`。对 `String` 之类的元素，克隆可能涉及分配和字节复制。只需要借用就不要克隆；确实需要拥有筛选结果时，先筛选再克隆，也能避免为被丢弃的元素做额外工作。

## 惰性求值，是下游向上游要数据

`map`、`filter`、`take` 这类适配器，主要是在构造新的迭代器对象，并不会因为方法链写完了就把整批数据处理完。真正推动它们的是后面的 `next`、`for`、`sum`、`collect` 等操作。

下面用 `inspect` 记录源头实际被访问的元素：

```rust
let mut visited = Vec::new();
let selected: Vec<_> = (1..=6)
    .inspect(|value| visited.push(*value))
    .filter(|value| value % 2 == 0)
    .map(|value| value * 10)
    .take(2)
    .collect();

assert_eq!(selected, vec![20, 40]);
assert_eq!(visited, vec![1, 2, 3, 4]);
```

不是先筛完整个范围，再全部乘以 10，最后截取两个结果。下游需要第一个结果时，筛选器跳过 1、接受 2，映射器交出 20；需要第二个结果时，同样得到 40。`take(2)` 已满足要求，源头的 5 和 6 就没有被访问。

这个按需拉取的模型很有用，但别把它理解成实现必须逐层调用一次 `next`。标准库可以通过专门实现的 `fold`、`try_fold` 等路径完成整段消费，保留相应语义，同时给优化器更好的代码形状。

还有两个容易混淆的边界。第一，惰性指的是这类适配器对元素的处理，不代表构造数据源、计算方法参数、捕获闭包环境都没有成本。第二，它不是异步，也不是并行：这里的闭包仍由消费迭代器的代码同步执行。

因此，单独写一条 `values.iter().map(...)` 并丢掉结果，不能指望闭包里的副作用发生。只想执行副作用，用 `for` 或 `for_each`；想观察中间状态，可以用 `inspect`，但要记住它只观察实际被消费的那一段。

## “消费迭代器”不等于“遍历完全部元素”

谈消费时，需要分开两件事：方法是否拿走迭代器对象，以及它实际上推进了多少元素。

`map`、`take` 接收 `self`，会拿走原迭代器，返回一个包装后的新迭代器，但它们本身仍是惰性的。`find`、`any`、`all` 则通过 `&mut self` 推进迭代器，并且可能提前停止。仅凭“拿走所有权”或者“借用”中的一个，不能判断数据是否已经被遍历完。

如果想把一个迭代器分段使用，可以借助 `by_ref()`：

```rust
let mut cursor = (1..=6).into_iter();

let first: Vec<_> = cursor.by_ref().take(2).collect();
assert_eq!(first, vec![1, 2]);

let middle: Vec<_> = cursor
    .by_ref()
    .take_while(|value| *value < 5)
    .collect();

assert_eq!(middle, vec![3, 4]);
assert_eq!(cursor.next(), Some(6));
```

`by_ref()` 返回对原迭代器的可变引用；因为 `&mut I` 本身也能实现 `Iterator`，后面的 `take`、`collect` 拿走的是这个引用，而不是 `cursor`。它不复制、不重置迭代器，前面的消费仍然会推进原状态。

这个例子还藏着一个常见坑：**`take_while` 会取走第一个不满足条件的元素。** 它必须先拿到 5，才能判断 5 不小于 5；这个元素不会交给结果，也不会自动放回去，所以后续从 6 开始。

如果分隔符还要留给下一段解析，可以考虑 `peekable()`，先用 `peek()` 查看，再决定是否调用 `next()`。注意 `peek()` 为了缓存值，也可能提前推进底层迭代器；它保证的是下次从这个 `Peekable` 取值时仍能拿到缓存项，不是底层完全没被访问。

## collect 的行为，由目标类型参与决定

`collect` 并不等于“创建一个 `Vec`”。它把迭代器交给目标类型的 `FromIterator` 实现，目标类型决定怎么收集。这也是为什么经常需要写 `let output: Vec<_> = ...collect()`，或者用 `collect::<Vec<_>>()` 提供类型信息。

比如，把一串可能失败的解析结果，收集成一个整体的 `Result`：

```rust
let mut input = ["12", "bad", "30"].into_iter();
let parsed: Result<Vec<u64>, _> = input
    .by_ref()
    .map(|text| text.parse::<u64>())
    .collect();

assert!(parsed.is_err());
assert_eq!(input.next(), Some("30"));
```

每次 `parse` 返回 `Result<u64, _>`。目标类型是 `Result<Vec<u64>, _>` 时，标准库的收集实现会把成功值放进容器；遇到第一个 `Err` 就返回错误，不再继续拉取输入。前面已经收集的部分结果不会作为成功容器交给调用方。

把目标换成 `Vec<Result<u64, _>>`，含义就变成“把每一次成功或失败都保存下来”。换成 `filter_map(|text| text.parse::<u64>().ok())`，则是在明确丢弃解析错误。这三种写法看起来很接近，错误处理策略却完全不同。

只想计算一个汇总值时，也不必先建 `Vec`。`fold` 可以逐项累积，`try_fold` 则可以在闭包返回失败或中止信号时短路。例如，用 `Option` 表示加法溢出：

```rust
let mut values = [250_u8, 20, 7].into_iter();
let total = values.try_fold(0_u8, |total, value| {
    total.checked_add(value)
});

assert_eq!(total, None);
assert_eq!(values.next(), Some(7));
```

累加到 20 时溢出，结果变成 `None`，而后续的 7 还留在迭代器里。`try_fold` 也支持 `Result`，适合“逐个处理，任何一步失败就结束”的任务。

## 自己实现一个迭代器，需要承诺什么？

先保存状态，再实现 `next`，就能接入大部分现成适配器。下面这个倒计时迭代器交出正整数，不包含 0：

```rust
struct Countdown {
    remaining: usize,
}

impl Iterator for Countdown {
    type Item = usize;

    fn next(&mut self) -> Option<Self::Item> {
        if self.remaining == 0 {
            return None;
        }

        let value = self.remaining;
        self.remaining -= 1;
        Some(value)
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        (self.remaining, Some(self.remaining))
    }
}

impl ExactSizeIterator for Countdown {}
impl std::iter::FusedIterator for Countdown {}

let mut countdown = Countdown { remaining: 3 };
assert_eq!(countdown.len(), 3);
assert_eq!(countdown.next(), Some(3));
assert_eq!(countdown.len(), 2);
assert_eq!(countdown.by_ref().collect::<Vec<_>>(), vec![2, 1]);
assert_eq!(countdown.next(), None);
assert_eq!(countdown.next(), None);
```

`size_hint()` 返回剩余元素数量的下界和可选上界，不一定是精确长度。比如筛选之前有十个元素，筛选之后可能一个都没有，`filter` 通常只能保留上界，并把下界设为 0。它不应该为了得到长度而提前执行谓词。

这个 `Countdown` 可以准确知道剩余数量，所以给出相同的上下界，并实现 `ExactSizeIterator`，得到 `len()`。这是实现者需要维护的正确性约定，不是编译器自动证明出来的事实，也不能让 `unsafe` 代码盲目信任一个安全 trait 的实现。

`FusedIterator` 承诺返回一次 `None` 后，后续永远返回 `None`。我们的状态停在 0，因此满足这个要求。对任意来源的迭代器，如果调用方需要这样的行为，可以使用 `.fuse()`；不要把普通 `Iterator` 的一次 `None` 当成它必然永久结束的证据。

还有一些能力是单独表达的。`DoubleEndedIterator` 提供 `next_back()`，支持从另一端取值，`rev()` 也依赖它；精确知道长度和能反向取值并不是同一件事。不要为了让方法“能调用”，就给自定义迭代器加上没有兑现的能力声明。

## 零成本抽象，不是每条方法链都自动最快

`map`、`filter` 组合起来，通常形成包含上游迭代器和闭包的具体类型。链上每一层并不天然对应一次堆分配，也不需要因为用了闭包就通过虚函数调用。

在泛型静态分发的路径上，编译器通过单态化处理具体类型，再结合内联和优化，有机会把这些包装消解成紧凑的循环。这就是迭代器能够成为零成本抽象的原因：表达方式更高层，不代表运行时必须保留同样层数的抽象。

返回 `impl Iterator<Item = ...>` 也仍然是在隐藏某个具体类型，而不是改成动态分发。换成 `Box<dyn Iterator<Item = ...>>`，通常就引入了装箱和通过虚表分发的成本；优化器有时能消除其中一些开销，但不能先假定它一定会。

即便坚持静态分发，数据处理本身的成本也不会凭空消失。`collect::<Vec<_>>()` 要物化结果并管理存储，可能发生分配，也可能复用已有存储；`cloned()` 可能复制大对象；适配器的顺序决定了要做多少工作。有限范围里的筛选再求和，与对无限迭代器直接 `collect`，更不是同一种终止条件。

性能敏感时，应当在优化构建下，用真实数据比较等价实现，并观察分配、分支和最终生成的代码。本文验证的是示例的类型与行为，没有把它们包装成性能测试，也没有据此给出“迭代器总比循环快”的结论。

顺序处理、筛选、映射和聚合，用迭代器往往很清楚；分支很多、需要 `break`、`continue` 或跨步骤修改状态时，直接写 `for` 也很自然。`for` 本来就建立在迭代协议上，不是另一套低级替代品。

本文代码按 Rust 2024 edition 编写，使用 `rustc 1.96.0` 验证；只使用稳定接口。

## 参考资料

- [标准库 std::iter：迭代模型、适配器与三种遍历方式](https://doc.rust-lang.org/std/iter/index.html)
- [Iterator trait：next、filter、by_ref、take_while、collect 与 try_fold](https://doc.rust-lang.org/std/iter/trait.Iterator.html)
- [IntoIterator trait：集合到迭代器的转换](https://doc.rust-lang.org/std/iter/trait.IntoIterator.html)
- [Rust Reference：for 循环及其展开语义](https://doc.rust-lang.org/reference/expressions/loop-expr.html#iterator-loops)
- [Result：FromIterator 实现的短路行为](https://doc.rust-lang.org/std/result/enum.Result.html#impl-FromIterator%3CResult%3CA,+E%3E%3E-for-Result%3CV,+E%3E)
- [ExactSizeIterator：精确长度及安全性边界](https://doc.rust-lang.org/std/iter/trait.ExactSizeIterator.html)
- [FusedIterator：耗尽后的行为约定](https://doc.rust-lang.org/std/iter/trait.FusedIterator.html)
- [DoubleEndedIterator：双向迭代](https://doc.rust-lang.org/std/iter/trait.DoubleEndedIterator.html)
- [The Rust Programming Language：循环与迭代器的性能](https://doc.rust-lang.org/book/ch13-04-performance.html)
- [Rust Compiler Development Guide：单态化](https://rustc-dev-guide.rust-lang.org/backend/monomorph.html)
- [Rust Reference：impl Trait 返回类型](https://doc.rust-lang.org/reference/types/impl-trait.html)
- [Rust Reference：trait object 与虚表分发](https://doc.rust-lang.org/reference/types/trait-object.html)
