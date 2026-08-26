---
title: "从朴素循环到硬件极限：GEMM / GEMV 高性能算子优化指南"
date: 2026-08-26T14:20:00+08:00
draft: false
tags: ["GEMM", "GEMV", "CUDA", "SIMD", "Tensor Core", "性能优化"]
categories: ["高性能计算"]
description: "从 Roofline、数据布局和分层分块出发，逐步分析如何在 CPU 与 GPU 上优化 GEMM/GEMV，并建立可复现的性能验证方法。"
---

GEMM 与 GEMV 看起来都只是乘加：

```text
GEMM: C = alpha * A * B + beta * C
GEMV: y = alpha * A * x + beta * y
```

但二者的最佳实现完全不同。GEMM 可以反复复用矩阵块，通常有机会逼近计算峰值；GEMV 中矩阵元素通常只读一次，往往受内存带宽限制。高性能算子的第一步不是写 SIMD 或 CUDA，而是先判断瓶颈究竟在哪里。

本文给出一条从正确基线走向高性能内核的完整路线。重点不是某段固定代码，而是每一步为什么有效、如何验证，以及何时应该停止优化。

## 一、先建立性能上限

### 1. 计算量

对于矩阵尺寸：

```text
A: M × K
B: K × N
C: M × N
```

GEMM 约执行：

```text
FLOPs = 2 * M * N * K
```

GEMV 是 `N = 1` 的特殊形态：

```text
FLOPs = 2 * M * K
```

乘法和加法各算一次浮点操作。

### 2. 算术强度

Roofline 模型使用算术强度判断内核倾向于计算受限还是带宽受限：

```text
Arithmetic Intensity = FLOPs / Bytes moved
Attainable Performance = min(Peak FLOPS, Bandwidth × Arithmetic Intensity)
```

理想 GEMM 中，A、B、C 从主存各读写一次：

```text
AI_GEMM ≈ 2MNK / element_size(MK + KN + MN)
```

当 M、N、K 同时增大，计算量按三次方增长，数据量按二次方增长，因此算术强度持续提高。

以方阵 `M=N=K=L`、FP32 为例：

```text
AI_GEMM ≈ 2L³ / (12L²) = L / 6 FLOP/Byte
```

GEMV 则不同。矩阵 A 的 `MK` 个元素通常只能贡献一次乘加：

```text
AI_GEMV ≈ 2MK / (4MK) ≈ 0.5 FLOP/Byte
```

即使忽略向量和输出流量，FP32 GEMV 的算术强度也只有约 0.5。若显存带宽为 2 TB/s，其 Roofline 上限约为 1 TFLOP/s，远低于现代 GPU 的矩阵计算峰值。

结论是：

- GEMM 的核心任务是制造数据复用，让计算单元持续工作；
- GEMV 的核心任务是把每个字节尽可能高效地搬进来，并减少额外流量。

## 二、第零步：建立可信的正确性与 Benchmark

性能优化最容易犯的错误，是测量一个错误结果或错误范围。

### 1. 保留参考实现

先写最朴素、最容易验证的三重循环：

```cpp
for (int m = 0; m < M; ++m) {
    for (int n = 0; n < N; ++n) {
        float acc = 0.0f;
        for (int k = 0; k < K; ++k) {
            acc += A[m * K + k] * B[k * N + n];
        }
        C[m * N + n] = acc;
    }
}
```

它不快，但适合作为数值参考。测试至少应覆盖：

- 非 tile 整数倍尺寸；
- `M`、`N` 或 `K` 为 1；
- 转置与非转置布局；
- `alpha`、`beta` 和累加路径；
- FP16/BF16 输入、FP32 累加；
- NaN、Inf、极大值和极小值；
- 不同 leading dimension 和非连续输入。

### 2. 正确计时

GPU kernel launch 是异步的，计时必须使用 CUDA Event 或在边界同步。还要区分：

```text
内核时间
端到端时间 = 数据准备 + 拷贝 + 内核 + 同步
```

每个尺寸先 warmup，多次重复并报告中位数或分位数。不要只测一个规则方阵；生产负载中的 skinny GEMM、small-M GEMM 和 GEMV 往往更重要。

### 3. 对比成熟库

用同精度、同布局、同 epilogue 的 BLAS 结果作为参考：CPU 对比 BLIS、OpenBLAS 或 oneDNN，GPU 对比 cuBLAS/cuBLASLt。目标不一定是击败库，而是判断自定义 kernel 距离合理上限还有多远。

## 三、第一步：修正循环顺序和数据布局

朴素 GEMM 的性能首先取决于内存访问顺序。假设矩阵按 row-major 保存，`B[k][n]` 在 n 方向连续。

`m-n-k` 循环每计算一个 C 元素，都沿 K 跳跃读取 B；更适合缓存的形式是 `m-k-n`：

```cpp
for (int m = 0; m < M; ++m) {
    for (int k = 0; k < K; ++k) {
        float a = A[m * K + k];
        for (int n = 0; n < N; ++n) {
            C[m * N + n] += a * B[k * N + n];
        }
    }
}
```

现在 B 和 C 都在最内层连续访问，A 的一个值被整行复用。编译器也更容易对 n 循环自动向量化。

如果业务反复使用同一个权重矩阵，预先转置或 pack 的成本可以被多次调用摊销。高性能库通常不会直接在原始矩阵上完成全部计算，而是把数据变换成适合微内核访问的面板布局。

## 四、第二步：分块，让工作集进入缓存

仅调整循环顺序仍会在矩阵较大时不断逐出缓存。Blocking 将问题拆成小块：

```text
for jc in N with block NC
  for pc in K with block KC
    pack B[pc:pc+KC, jc:jc+NC]
    for ic in M with block MC
      pack A[ic:ic+MC, pc:pc+KC]
      macro_kernel(packed_A, packed_B, C_block)
```

典型目标是：

- B 的 `KC × NC` panel 驻留 LLC；
- A 的 `MC × KC` panel 驻留较近缓存；
- 微内核使用的 A、B 小片段来自 L1；
- C 的 `MR × NR` tile 尽量驻留寄存器。

参数不是越大越好。一个实用约束是：工作集加上其他活跃数据，应明显小于目标缓存容量，并考虑缓存组冲突、TLB 和多线程共享。

## 五、第三步：设计寄存器微内核

CPU GEMM 的核心不是外层循环，而是计算 `MR × NR` 输出 tile 的微内核：

```text
C[MR × NR] += A[MR × K] × B[K × NR]
```

微内核沿 K 循环，每轮：

1. 加载 A 的若干标量或向量；
2. 加载 B 的一个 SIMD 向量；
3. 用 FMA 更新多个 C 累加器；
4. K 结束后一次性写回 C。

以 AVX-512 为例，一个向量保存 16 个 FP32。若 `NR=16`，每个 A 标量可广播后与一整行 B 做 FMA。多个 `MR` 行并行累加，既复用 B，又增加独立指令链以隐藏 FMA 延迟。

微内核尺寸受寄存器数量约束：

```text
accumulators + A operands + B operands + addresses < architectural registers
```

`MR × NR` 太小，数据复用不足；太大则寄存器溢出到栈，性能断崖式下降。

高性能 CPU GEMM 因此形成五层循环与一个架构专用微内核。BLIS 将 micro-kernel 作为清晰接口，外围 packing 与 blocking 基本保持通用。

## 六、第四步：并行化，但不要破坏局部性

GEMM 可以沿 M、N 或 batch 维度并行。线程划分要尽量满足：

- 每个线程写不同 C tile，避免 false sharing；
- 共享只读 packed B，减少重复 packing；
- NUMA 环境中让内存靠近执行线程；
- 小矩阵不要启动过多线程；
- 避免线程数、BLAS 内部线程和上层并发三重过度订阅。

大矩阵通常适合二维划分输出矩阵，小 M 或小 N 时应选择仍有足够并行度的方向。Batch 中存在大量小矩阵时，跨 batch 并行往往优于拆分单个矩阵。

## 七、GPU 第一步：合并访存与 Shared Memory Tiling

朴素 CUDA GEMM 常让一个线程计算一个 C 元素。虽然简单，但每个输出都从全局内存重复读取 A 行和 B 列。

标准改进是让一个 thread block 负责 `BM × BN` 输出 tile，并分段遍历 K：

```text
for k_tile in K:
    global -> shared: A[BM × BK]
    global -> shared: B[BK × BN]
    synchronize
    shared -> registers: accumulate C tile
    synchronize
```

一个 A 元素可被 BN 方向多个输出复用，一个 B 元素可被 BM 方向多个输出复用。理想情况下，全局内存流量相比朴素实现下降约一个 tile 维度。

加载阶段必须满足：

- warp 中线程访问连续地址，形成 coalesced transaction；
- 向量化加载满足地址对齐；
- shared-memory layout 避免 bank conflict；
- 边界 tile 使用 predicate，而不是让整个 warp 严重分歧。

## 八、GPU 第二步：线程级分块与寄存器复用

若每个线程只计算一个 C 元素，从 shared memory 读取数据的次数仍然过多。让每个线程计算 `TM × TN` 小 tile，可把 A、B 片段装入寄存器后重复使用。

层次变成：

```text
CTA tile  : BM × BN × BK
Warp tile : WM × WN × WK
Thread tile: TM × TN
```

每向下一级，数据从更慢、更大的存储移动到更快、更小的存储：

```text
HBM -> L2 -> Shared Memory -> Registers -> FMA/Tensor Core
```

真正的优化目标不是“少一次 load”，而是让一个字节在离计算单元最近的位置被消费尽可能多次。

## 九、GPU 第三步：流水线隐藏访存延迟

完成 tiling 后，加载下一块数据与计算当前块仍可能串行：

```text
load tile 0 -> compute tile 0 -> load tile 1 -> compute tile 1
```

双缓冲或多 stage pipeline 将其改成：

```text
load tile 0
compute tile 0 || load tile 1
compute tile 1 || load tile 2
```

CUDA 的异步 global-to-shared copy 可减少中间寄存器使用，并允许数据搬运与计算重叠。CUTLASS 的 GEMM mainloop 正是围绕多级 pipeline 组织，较新架构还会使用 TMA、warp specialization 和更深的 producer-consumer 管线。

stage 数并非越多越好。更多 stage 会消耗更多 shared memory，降低 occupancy。应以“是否足以覆盖内存延迟”为目标，而不是追求最大缓冲深度。

## 十、GPU 第四步：使用 Tensor Core

FP16、BF16、TF32、FP8 或部分整数 GEMM 应优先使用 Tensor Core 指令。Tensor Core 以小矩阵片段执行 MMA：

```text
D = A × B + C
```

高性能实现需要同时满足：

- tile 尺寸符合 MMA 指令形状；
- shared-memory layout 适合 warp/warpgroup 装载；
- K 维和地址满足对齐要求；
- 使用足够大的 tile 摊销指令与调度开销；
- 累加精度符合数值要求。

直接写 PTX 通常不是第一选择。更现实的开发路径是：

1. 用 cuBLASLt 建立性能上限；
2. 用 CUTLASS 组合 tile、pipeline 与 epilogue；
3. 用 Triton 快速搜索 block size、warp 数和 stage 数；
4. 只有框架无法表达关键优化时，再编写更底层内核。

## 十一、Epilogue Fusion 往往比继续抠 GEMM 更值

真实模型很少只计算裸 `A × B`。Linear 层后面可能还有 bias、activation、residual、quantization 或 gated operation。

若每步都独立启动 kernel：

```text
GEMM -> write C -> read C -> bias -> write
     -> read -> activation -> write
```

融合 epilogue 可以让累加结果仍在寄存器时完成后处理，只写回一次：

```text
accumulator -> bias -> activation -> cast -> store
```

这不仅减少 HBM 流量，也减少 launch overhead。对于中小 GEMM，融合带来的端到端收益可能高于进一步提高主循环的峰值 FLOPS。

cuBLASLt 和 CUTLASS 都把 epilogue 视为一等能力；自定义 kernel 也应从完整算子边界衡量性能。

## 十二、GEMV 必须走另一条优化路线

把高性能 GEMM kernel 的 `N` 设成 1，通常得不到高性能 GEMV。原因是大量 tiling 和同步开销无法通过数据复用摊销。

### 1. 优先保证连续读取

每个 warp 或线程块处理矩阵的一段连续区域，使用宽加载读取 A。向量 x 应尽量驻留缓存、constant cache 或 shared memory，但不要为了复制一个很大的 x 引入过高同步成本。

### 2. 做好归约

常见映射是多个线程共同计算一行输出：

```text
thread 0: a[0] * x[0] + a[32] * x[32] + ...
thread 1: a[1] * x[1] + a[33] * x[33] + ...
...
warp reduce -> y[row]
```

优先使用 warp shuffle 完成寄存器归约，跨 warp 时再使用少量 shared memory。不要在每个元素上做 atomic add。

### 3. 增加批量，恢复矩阵复用

推理 Decode 中常见的 matrix-vector，实际可以通过连续批处理变成 matrix-matrix：多个请求或多个 token 同时计算，让权重被复用。

因此优化 GEMV 的最高杠杆有时不在 kernel 内，而在调度层：

```text
单请求 GEMV -> 多请求 batched GEMV -> small-N GEMM
```

只要延迟预算允许，提高 batch size 通常能显著提升权重带宽利用率和 Tensor Core 使用率。

### 4. 压缩权重减少字节数

GEMV 受带宽限制，FP16、INT8、FP8 或 INT4 权重量化可直接减少主存流量。但反量化必须与乘加融合，否则中间张量写回会抵消收益。

一个理想的 weight-only 路径是：

```text
load packed quantized weights
  -> register 中解包/反量化
  -> 与 activation 相乘并累加
  -> 一次写回输出
```

GEMV 的优化指标应优先看有效带宽，而不是峰值 FLOPS。

## 十三、处理不规则尺寸

只优化 4096 的整数倍会制造漂亮但无用的 benchmark。生产矩阵尺寸来自 hidden size、head size、MoE expert、LoRA rank 和 batch，形状差异很大。

常用方法包括：

- 主 kernel 处理完整 tile，专门的 residue kernel 处理边界；
- 使用 predicated load/store；
- 为 small-M、small-N、split-K 和 batched 场景准备不同配置；
- 对固定模型尺寸离线 autotune；
- 将 layout、dtype、alignment 和 epilogue 纳入 dispatch key。

Split-K 可让多个 CTA 并行处理同一个输出 tile 的不同 K 区间，在 M、N 很小而 K 很大时增加并行度；代价是额外归约或 atomic 写入。

## 十四、Autotune 应搜索什么

一个通用 GEMM 配置至少包括：

```text
BM, BN, BK
warps per CTA
pipeline stages
instruction shape
swizzle / cluster shape
split-K factor
```

最佳配置与 GPU 架构、dtype、矩阵形状、布局和 epilogue 都相关。Triton 教程通过多个 config 进行自动调优；cuBLASLt 提供 heuristic 和算法选择；CUTLASS profiler 可枚举 kernel 组合。

Autotune 不是无限搜索。应先用硬件约束剪枝：

- shared memory 不得超限；
- 寄存器压力不能导致严重 spill；
- CTA 数需足以占满设备；
- tile 长宽应匹配矩阵形状；
- pipeline 深度应与计算/访存比例匹配。

生产系统还需要缓存调优结果，避免首次请求承担长时间搜索。

## 十五、用 profiler 判断下一步，而不是凭感觉

优化循环应是：

```text
测量 -> 提出瓶颈假设 -> 只改一个变量 -> 再测量
```

GPU 上至少关注：

| 指标 | 暗示的问题 |
|---|---|
| DRAM throughput 接近峰值 | 带宽受限，减少字节或提升合并访问 |
| Tensor/FMA pipe 利用率低 | tile、并行度或指令选择不足 |
| Long scoreboard stall 高 | 全局内存延迟未被覆盖 |
| Short scoreboard stall 高 | shared memory 依赖或 bank conflict |
| Register spill | thread tile 或 pipeline 过大 |
| Occupancy 低 | 寄存器/shared memory/CTA 形状受限 |
| Launch 数量多 | 需要 fusion 或 persistent kernel |

CPU 上对应检查 IPC、SIMD 利用率、缓存 miss、TLB miss、内存带宽、NUMA remote access 与线程扩展效率。

Occupancy 不是最终目标。一个使用更多寄存器、occupancy 较低但数据复用更好的 GEMM，可能明显更快。最终指标始终是实际形状上的延迟或吞吐。

## 十六、一条实际可执行的优化顺序

建议按以下顺序推进，避免过早进入汇编细节：

1. 写正确参考实现，建立随机与边界测试；
2. 用 Roofline 判断计算或带宽上限；
3. 修正布局、循环顺序和连续访问；
4. 做 cache/shared-memory blocking；
5. 引入寄存器 tile 与 SIMD/FMA；
6. 调整线程、warp 和 CTA 映射；
7. 使用双缓冲或异步流水线；
8. 切换 Tensor Core 或目标 ISA 的矩阵指令；
9. 融合 bias、activation、quantization 等 epilogue；
10. 为特殊形状添加 split-K、batched 或 GEMV kernel；
11. Autotune 并按形状 dispatch；
12. 用端到端工作负载验证，而不是只看方阵峰值。

每一步都应同时检查正确性、性能、资源占用和适用范围。若自定义 kernel 只在一个尺寸领先，却在其他尺寸严重回退，就需要调度器，而不是宣称得到“通用最优实现”。

## 十七、极致性能的真正含义

高性能 GEMM 的本质是构造分层数据复用：

```text
主存中的一个 tile
 -> 被一个 CTA 复用
 -> 被多个 warp 复用
 -> 被多个线程寄存器累加器复用
 -> 由矩阵指令一次完成大量 FMA
```

高性能 GEMV 的本质则是承认复用有限：

```text
减少权重字节数
+ 连续宽加载
+ 低成本归约
+ 融合后处理
+ 尽可能通过 batching 转回 GEMM
```

所谓“优化到极致”，不是把一个 kernel 写得最复杂，而是逼近该形状、精度和完整算子链的真实 Roofline。先减少不必要的数据移动，再增加有效并行，最后才是手工指令级优化。

## 参考资料

- NVIDIA CUDA C++ Programming Guide：Shared Memory、异步拷贝与 Tensor Core 编程模型
- NVIDIA CUTLASS Documentation：Hierarchical GEMM、Pipelining 与 Collective Mainloop
- NVIDIA cuBLASLt Documentation：Matmul heuristic、算法选择与 epilogue fusion
- Triton Matrix Multiplication Tutorial：Block-level GEMM 与 autotune
- BLIS：GotoBLAS 风格分块、packing 与 micro-kernel 架构
- NVIDIA Nsight Compute Profiling Guide：Roofline 与 GPU 性能指标
