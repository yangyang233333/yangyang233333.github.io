---
title: "分布式 AI 训练通信原语：AllReduce、AllGather、ReduceScatter 与 AllToAll"
date: 2026-09-02T11:29:42+08:00
draft: false
tags: ["分布式训练", "PyTorch", "NCCL", "AllReduce", "AllGather", "AllToAll"]
categories: ["AI 系统"]
summary: "从数据流、张量形状、通信量和训练场景出发，系统解释 Broadcast、Reduce、Gather、Scatter、AllReduce、AllGather、ReduceScatter 与 AllToAll，并给出可运行的 torch.distributed 示例。"
---

分布式训练中的通信原语，本质上是在回答两个问题：**数据从哪些 Rank 来，到哪些 Rank 去；途中是否需要做求和、平均、最大值等归约。**

最容易混淆的一组概念是：Master 收集所有 Worker 的数据叫 `Gather`；只有当每个 Worker 最终都拿到所有 Worker 的数据时，才叫 `AllGather`。理解这一点后，其他原语也可以沿着“谁发送、谁接收、是否归约”来判断。

本文以 4 个进程为例，解释常见集合通信，并用 `torch.distributed` 给出最小演示。这里的进程也常被称为 Rank，通常一个 GPU 对应一个 Rank。

# 一、先建立统一模型：Rank、World Size 与 Process Group

假设启动 4 个训练进程：

```text
world_size = 4
rank = 0, 1, 2, 3

Rank 0 ─ GPU 0
Rank 1 ─ GPU 1
Rank 2 ─ GPU 2
Rank 3 ─ GPU 3
```

- `world_size`：通信组中的进程总数；
- `rank`：进程在通信组中的编号；
- `process group`：参与某次集合通信的 Rank 集合；
- `src` / `dst`：点名源 Rank 或目标 Rank；
- `root`：一些论文或通信库对中心 Rank 的称呼。

“Master”和“Worker”是控制层面的角色，不等同于通信原语的定义。集合通信只关心 Rank 之间的数据关系。

# 二、先看总表

| 原语 | 输入 | 每个 Rank 的输出 | 是否归约 | 典型用途 |
| --- | --- | --- | --- | --- |
| Broadcast | 一个 Rank 的数据 | 同一份数据 | 否 | 同步参数、配置、随机种子 |
| Reduce | 每个 Rank 一份数据 | 仅目标 Rank 得到归约结果 | 是 | 汇总指标到主进程 |
| AllReduce | 每个 Rank 一份同形状数据 | 每个 Rank 得到归约结果 | 是 | 数据并行梯度同步 |
| Gather | 每个 Rank 一个分片 | 仅目标 Rank 得到全部分片 | 否 | 主进程收集预测结果 |
| AllGather | 每个 Rank 一个分片 | 每个 Rank 得到全部分片 | 否 | 参数恢复、特征汇聚 |
| Scatter | 一个 Rank 持有全部分片 | 每个 Rank 得到一个分片 | 否 | 分发输入或任务 |
| ReduceScatter | 每个 Rank 一组数据 | 每个 Rank 得到归约结果的一片 | 是 | ZeRO、FSDP 梯度分片 |
| AllToAll | 每个 Rank 持有发往各 Rank 的分片 | 每个 Rank 收到来自各 Rank 的分片 | 否 | MoE Token Dispatch |

# 三、中心式通信：Broadcast、Reduce、Gather、Scatter

这四种原语都存在一个被点名的 Rank。

## 1. Broadcast：一份数据发给所有 Rank

Rank 0 持有 `A`：

```text
执行前：R0=A  R1=?  R2=?  R3=?
执行后：R0=A  R1=A  R2=A  R3=A
```

典型场景包括初始化时同步模型参数，或让所有进程获得一致的控制信息。

## 2. Reduce：所有 Rank 归约到一个 Rank

如果使用求和：

```text
执行前：R0=1  R1=2  R2=3  R3=4
执行后：R0=10 R1=?  R2=?  R3=?
```

只有目标 Rank 的结果有定义。它适合把 loss、样本数等指标汇总到主进程。

## 3. Gather：所有 Rank 的分片收集到一个 Rank

```text
执行前：R0=A  R1=B  R2=C  R3=D
执行后：R0=[A,B,C,D]
```

这正是“Master 收集所有 Worker”的通信语义。数据只是按 Rank 收集或拼接，没有求和。

## 4. Scatter：一个 Rank 把不同分片分给各 Rank

```text
执行前：R0=[A,B,C,D]
执行后：R0=A  R1=B  R2=C  R3=D
```

Scatter 可以看作 Gather 的反方向。

# 四、AllReduce：数据并行梯度同步的核心

AllReduce 对每个 Rank 的同形状张量做归约，并把结果交给每个 Rank。使用求和时：

```text
执行前：
R0=[1, 10]
R1=[2, 20]
R2=[3, 30]
R3=[4, 40]

执行后，每个 Rank 都得到：
[10, 100]
```

它在语义上近似：

```text
Reduce(to one rank) + Broadcast(to all ranks)
```

但实际通信库通常不会真的先集中到 Master。NCCL 等库会使用 Ring、Tree 等拓扑算法，让通信负载分散到各设备。

## 为什么 DDP 使用 AllReduce

数据并行中，每张 GPU 保存完整模型，但处理不同 mini-batch：

```text
GPU 0: batch_0 → local_gradient_0
GPU 1: batch_1 → local_gradient_1
GPU 2: batch_2 → local_gradient_2
GPU 3: batch_3 → local_gradient_3
```

参数更新前需要得到全局梯度：

```text
global_gradient = sum(local_gradient_i) / world_size
```

PyTorch DistributedDataParallel 通常对梯度桶执行 AllReduce。归约完成后，每个 Rank 拥有一致梯度，因此执行相同优化器步骤后，模型参数仍保持一致。

需要注意，通信库常做的是 `SUM`。是否再除以 `world_size`，由框架的梯度语义处理。

# 五、AllGather：每个 Rank 都拿到全部分片

AllGather 不做数值归约，只收集所有 Rank 的数据：

```text
执行前：R0=A  R1=B  R2=C  R3=D
执行后：
R0=[A,B,C,D]
R1=[A,B,C,D]
R2=[A,B,C,D]
R3=[A,B,C,D]
```

## 典型场景一：FSDP / ZeRO 参数恢复

参数分片后，每个 Rank 平时只保存一部分参数：

```text
R0: W0  R1: W1  R2: W2  R3: W3
```

某一层计算前，可以通过 AllGather 临时恢复完整权重 `W=[W0,W1,W2,W3]`。计算结束后释放完整副本，继续只保存本地分片，以降低常驻显存。

## 典型场景二：对比学习收集全局特征

每张 GPU 计算本地 batch 的 embedding。AllGather 后，每个 Rank 都能用全局 batch 的特征构造正负样本。

## AllGather 与 Gather 的关键区别

- Gather：只有目标 Rank 拿到完整数据；
- AllGather：所有 Rank 都拿到完整数据。

如果只有 Master 使用结果，却调用 AllGather，那么其他 Rank 也会接收完整数据，可能造成额外通信和显存开销。

# 六、ReduceScatter：先归约，再把结果分片

ReduceScatter 可以理解为：

```text
AllReduce + Scatter
```

但高效实现会把归约和分片结合起来，避免每张卡都暂存完整归约结果。

假设每个 Rank 输入 4 个元素，并按位置求和：

```text
R0: [ 0,  1,  2,  3]
R1: [10, 11, 12, 13]
R2: [20, 21, 22, 23]
R3: [30, 31, 32, 33]

逐元素求和：[60, 64, 68, 72]
```

ReduceScatter 的输出是：

```text
R0: [60]
R1: [64]
R2: [68]
R3: [72]
```

它特别适合 ZeRO 和 FSDP：反向传播产生的梯度先跨 Rank 归约，但每个 Rank 最终只保留自己负责的梯度分片。这样既完成全局梯度聚合，又避免每张 GPU 保存完整梯度。

AllGather 与 ReduceScatter 经常成对出现：

```text
前向/反向计算前：参数分片 ──AllGather──> 完整参数
反向计算后：完整梯度 ──ReduceScatter──> 归约后的梯度分片
```

# 七、AllToAll：每个 Rank 都向每个 Rank 发送不同数据

AllToAll 不是让大家获得同一份完整数据，而是进行“全互换”。每个 Rank 将输入切成 `world_size` 份，第 `j` 份发送给 Rank `j`。

```text
R0 输入：[A0, A1, A2, A3]
R1 输入：[B0, B1, B2, B3]
R2 输入：[C0, C1, C2, C3]
R3 输入：[D0, D1, D2, D3]
```

通信后：

```text
R0 输出：[A0, B0, C0, D0]
R1 输出：[A1, B1, C1, D1]
R2 输出：[A2, B2, C2, D2]
R3 输出：[A3, B3, C3, D3]
```

## 为什么 MoE 需要 AllToAll

Mixture of Experts 模型中，不同 Expert 通常部署在不同 GPU。Router 为每个 Token 选择 Expert 后，Token 必须被发往对应 GPU：

```text
本地 Tokens
    |
    | Router 按目标 Expert 分组
    v
AllToAll：发送到 Expert 所在 Rank
    |
    v
Expert 计算
    |
    v
AllToAll：结果发回原 Rank
```

与 AllReduce 相比，AllToAll 的流量更依赖 Token 路由是否均衡。如果大量 Token 被分配给少数 Expert，对应 Rank 会成为热点，因此 MoE 还需要容量限制、负载均衡损失和高效 Token 排列。

`all_to_all_single` 还支持不等长分片。真实 MoE 中各 Rank 发往不同 Expert 的 Token 数往往不同，此时需先交换 split sizes，再执行变长 AllToAll。

# 八、通信量与性能直觉

设通信组有 `P` 个 Rank，每个 Rank 的输入张量大小为 `N` 字节。忽略协议开销，并以常见 Ring 算法理解每个 Rank 的逻辑网络流量：

| 原语 | 每个 Rank 典型发送/接收量级 | 主要压力 |
| --- | --- | --- |
| AllReduce | 约 `2(P-1)N/P` | 带宽、归约计算 |
| AllGather | 约 `(P-1)N` | 带宽、输出显存 |
| ReduceScatter | 约 `(P-1)N/P` | 带宽、归约计算 |
| AllToAll | 约 `(P-1)N/P` 发出并接收 | 全互连、拥塞、负载均衡 |

这里要小心 `N` 的定义。AllGather 中若 `N` 是每个 Rank 的本地分片大小，最终输出大小是 `P×N`；ReduceScatter 中若 `N` 是每个 Rank 的完整输入大小，输出大小是 `N/P`。

实际耗时不只由字节数决定，还受以下因素影响：

- 节点内是 NVLink、NVSwitch 还是 PCIe；
- 节点间是 InfiniBand、RoCE 还是普通以太网；
- 张量是否连续，消息是大量小包还是少量大包；
- 通信拓扑、Rank 映射和跨节点比例；
- 是否用独立 CUDA Stream 将通信与计算重叠；
- 慢 Rank、网络拥塞与 MoE 路由倾斜。

因此，“通信量更少”不一定代表端到端更快，但它是判断瓶颈的第一步。

# 九、一个可运行的 PyTorch 演示

下面的程序使用 CPU 和 Gloo 后端，因此没有 GPU 也能运行。每种原语前后都用 Barrier 保证输出顺序更容易阅读。

```python
# collectives_demo.py
import argparse
import os

import torch
import torch.distributed as dist


def ordered_print(message: str) -> None:
    for current_rank in range(dist.get_world_size()):
        dist.barrier()
        if dist.get_rank() == current_rank:
            print(f"rank {current_rank}: {message}", flush=True)
    dist.barrier()


def demo_all_reduce() -> None:
    rank = dist.get_rank()
    tensor = torch.tensor([rank + 1.0])
    dist.all_reduce(tensor, op=dist.ReduceOp.SUM)
    ordered_print(f"all_reduce -> {tensor.tolist()}")


def demo_all_gather() -> None:
    rank = dist.get_rank()
    tensor = torch.tensor([rank], dtype=torch.int64)
    output = [torch.empty_like(tensor) for _ in range(dist.get_world_size())]
    dist.all_gather(output, tensor)
    ordered_print(f"all_gather -> {[item.item() for item in output]}")


def demo_reduce_scatter() -> None:
    rank = dist.get_rank()
    world_size = dist.get_world_size()
    input_tensor = torch.arange(world_size, dtype=torch.float32) + rank * 10
    output = torch.empty(1, dtype=torch.float32)

    # reduce_scatter_tensor 要求输入第 0 维能被 world_size 整除。
    dist.reduce_scatter_tensor(output, input_tensor, op=dist.ReduceOp.SUM)
    ordered_print(
        f"reduce_scatter input={input_tensor.tolist()} -> {output.tolist()}"
    )


def demo_all_to_all() -> None:
    rank = dist.get_rank()
    world_size = dist.get_world_size()
    input_tensor = torch.tensor(
        [rank * 10 + destination for destination in range(world_size)],
        dtype=torch.int64,
    )
    output = torch.empty_like(input_tensor)
    dist.all_to_all_single(output, input_tensor)
    ordered_print(f"all_to_all input={input_tensor.tolist()} -> {output.tolist()}")


def demo_gather() -> None:
    rank = dist.get_rank()
    tensor = torch.tensor([rank], dtype=torch.int64)
    gather_list = (
        [torch.empty_like(tensor) for _ in range(dist.get_world_size())]
        if rank == 0
        else None
    )
    dist.gather(tensor, gather_list=gather_list, dst=0)
    if rank == 0:
        print(f"rank 0: gather -> {[item.item() for item in gather_list]}")
    dist.barrier()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "operation",
        choices=["all_reduce", "all_gather", "reduce_scatter", "all_to_all", "gather"],
    )
    args = parser.parse_args()

    dist.init_process_group(backend="gloo")
    operations = {
        "all_reduce": demo_all_reduce,
        "all_gather": demo_all_gather,
        "reduce_scatter": demo_reduce_scatter,
        "all_to_all": demo_all_to_all,
        "gather": demo_gather,
    }
    operations[args.operation]()
    dist.destroy_process_group()


if __name__ == "__main__":
    os.environ.setdefault("OMP_NUM_THREADS", "1")
    main()
```

保存后，用 `torchrun` 启动 4 个本地进程：

```bash
torchrun --standalone --nproc-per-node=4 collectives_demo.py all_reduce
torchrun --standalone --nproc-per-node=4 collectives_demo.py all_gather
torchrun --standalone --nproc-per-node=4 collectives_demo.py reduce_scatter
torchrun --standalone --nproc-per-node=4 collectives_demo.py all_to_all
torchrun --standalone --nproc-per-node=4 collectives_demo.py gather
```

几个关键输出如下：

```text
# AllReduce：1 + 2 + 3 + 4 = 10
rank 0: all_reduce -> [10.0]
rank 1: all_reduce -> [10.0]
rank 2: all_reduce -> [10.0]
rank 3: all_reduce -> [10.0]

# AllGather：每个 Rank 都拿到 [0, 1, 2, 3]
rank 0: all_gather -> [0, 1, 2, 3]
...

# AllToAll：Rank 2 收到每个源 Rank 发给目标 2 的数据
rank 2: all_to_all input=[20, 21, 22, 23] -> [2, 12, 22, 32]
```

如果在 NVIDIA GPU 上运行，可将后端改成 `nccl`，并在初始化后绑定本地设备：

```python
local_rank = int(os.environ["LOCAL_RANK"])
torch.cuda.set_device(local_rank)
dist.init_process_group(backend="nccl")
tensor = tensor.cuda(local_rank)
```

NCCL 集合通信操作在 CUDA 上通常是异步入队的。需要与后续计算建立明确依赖时，可保存 `async_op=True` 返回的 Work 并调用 `wait()`；做性能测量时还要正确使用 CUDA Event 或同步，不能只测 Python 调用返回时间。

# 十、如何快速判断应该使用哪个原语

可以按三个问题判断：

1. **最终是一个 Rank 需要结果，还是所有 Rank 都需要？** 一个 Rank 对应 Gather/Reduce，所有 Rank 对应 AllGather/AllReduce。
2. **数据要不要做求和、平均、最大值等归约？** 要归约选 Reduce 系列，不归约选 Gather 系列。
3. **每个目标 Rank 收到的是同一结果，还是不同分片？** 同一归约结果常用 AllReduce；每个 Rank 接收不同来源分片常用 AllToAll。

最常见的训练映射可以记成：

```text
数据并行梯度同步        -> AllReduce
FSDP/ZeRO 参数恢复      -> AllGather
FSDP/ZeRO 梯度分片      -> ReduceScatter
MoE Token 路由          -> AllToAll
Master 收集 Worker 数据 -> Gather
Master 同步数据给大家   -> Broadcast
```

# 总结

集合通信的名字不是由“有没有 Master”决定，而是由最终数据分布决定：

- `Reduce` 做归约，`Gather` 做收集；
- 带 `All` 表示每个 Rank 都得到结果；
- `ReduceScatter` 得到归约结果的分片；
- `AllToAll` 让每个 Rank 向每个 Rank 发送不同分片。

理解每个操作前后的张量形状和数据所有权，比背 API 更重要。分析分布式训练系统时，只要画出“通信前每张卡有什么、通信后每张卡需要什么”，通常就能判断应使用哪种通信原语。
