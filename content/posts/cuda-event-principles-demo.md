---
title: "CUDA Event：GPU 时间线上的事件、同步原理与实战"
date: 2026-08-31T10:30:00+08:00
draft: false
tags: ["CUDA", "GPU", "并行计算", "性能优化", "C++"]
categories: ["技术原理"]
---

CUDA kernel launch 通常是异步的：CPU 把 kernel、内存拷贝等操作提交到 stream 后便继续执行。由此产生两个常见问题：如何准确测量 GPU 工作耗时，以及如何让不同 stream 在不阻塞 CPU 的情况下建立依赖？

CUDA Event 就是解决这类问题的基础设施。它可以理解为插入 GPU stream 时间线中的一个标记：当 event 之前的工作全部完成，event 才进入完成状态。借助这个状态，我们可以做 GPU 计时、CPU 等待、跨 stream 同步和异步资源回收。

## 1. Event 不是 CPU 事件，而是 GPU 时间线标记

一个 CUDA stream 是按序执行的 GPU 工作队列：

```text
CPU 提交： Kernel A → memcpy → Event E → Kernel B
                         异步提交

GPU 执行： Kernel A ── memcpy ── E 完成 ── Kernel B
```

调用 `cudaEventRecord(event, stream)` 时，CPU 通常只是向 `stream` 提交一个 event 记录操作，并不会等待 GPU 执行到该位置。只有当这个 stream 中排在 event 前面的操作完成后，event 才会被标记为完成。

这带来三个重要性质：

1. event 描述的是 stream 的执行进度，而不是某个 kernel 本身；
2. record 是异步提交，event 不会在函数返回时自动完成；
3. 同一 stream 内有序，不同 stream 之间默认没有这种顺序保证。

最基础的生命周期如下：

```cpp
cudaEvent_t event;
cudaEventCreate(&event);

cudaEventRecord(event, stream);
cudaEventSynchronize(event);

cudaEventDestroy(event);
```

其中 `cudaEventSynchronize` 表示 CPU 等待 event 完成。若不想阻塞 CPU，可以使用 `cudaEventQuery` 查询状态。

## 2. Event 为什么能准确测量 GPU 时间

下面这段 CPU 计时代码很容易测错：

```cpp
auto begin = std::chrono::steady_clock::now();
vectorAdd<<<grid, block>>>(a, b, c, n);
auto end = std::chrono::steady_clock::now();
```

kernel launch 是异步的，`end` 很可能在 GPU 真正执行 kernel 之前就被记录。因此测到的主要是 CPU 发射 kernel 的时间，而不是 GPU 执行时间。

CUDA Event 的计时发生在 GPU 时间线上：

```text
stream: start event ── kernel ── stop event
        │<------ GPU elapsed time ------>│
```

当 stop event 完成后，`cudaEventElapsedTime` 可以计算两个 event 之间经过的 GPU 时间。它返回毫秒值。

## 3. Demo 一：用 Event 测量 kernel 执行时间

下面是一个完整、可编译的向量加法程序。它同时演示预热、event 计时和结果校验。

```cpp
// event_timing.cu
#include <cuda_runtime.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

#define CUDA_CHECK(call)                                                   \
    do {                                                                   \
        cudaError_t error = (call);                                        \
        if (error != cudaSuccess) {                                        \
            std::cerr << "CUDA error: " << cudaGetErrorString(error)       \
                      << " at " << __FILE__ << ':' << __LINE__ << '\n';    \
            std::exit(EXIT_FAILURE);                                       \
        }                                                                  \
    } while (0)

__global__ void vectorAdd(const float* a, const float* b, float* c, int n) {
    int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index < n) {
        c[index] = a[index] + b[index];
    }
}

int main() {
    constexpr int n = 1 << 24;
    const std::size_t bytes = n * sizeof(float);

    std::vector<float> hostA(n, 1.0f);
    std::vector<float> hostB(n, 2.0f);
    std::vector<float> hostC(n);

    float* deviceA = nullptr;
    float* deviceB = nullptr;
    float* deviceC = nullptr;
    CUDA_CHECK(cudaMalloc(&deviceA, bytes));
    CUDA_CHECK(cudaMalloc(&deviceB, bytes));
    CUDA_CHECK(cudaMalloc(&deviceC, bytes));

    CUDA_CHECK(cudaMemcpy(deviceA, hostA.data(), bytes, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(deviceB, hostB.data(), bytes, cudaMemcpyHostToDevice));

    constexpr int threads = 256;
    const int blocks = (n + threads - 1) / threads;

    // 预热：避免把上下文初始化、模块加载等一次性成本混入测量。
    vectorAdd<<<blocks, threads>>>(deviceA, deviceB, deviceC, n);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());

    cudaEvent_t start;
    cudaEvent_t stop;
    CUDA_CHECK(cudaEventCreate(&start));
    CUDA_CHECK(cudaEventCreate(&stop));

    CUDA_CHECK(cudaEventRecord(start));
    vectorAdd<<<blocks, threads>>>(deviceA, deviceB, deviceC, n);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaEventRecord(stop));

    // 只等待 stop；同一 stream 中 stop 之前的 kernel 也必然已经完成。
    CUDA_CHECK(cudaEventSynchronize(stop));

    float elapsedMs = 0.0f;
    CUDA_CHECK(cudaEventElapsedTime(&elapsedMs, start, stop));
    std::cout << "kernel time: " << elapsedMs << " ms\n";

    CUDA_CHECK(cudaMemcpy(hostC.data(), deviceC, bytes, cudaMemcpyDeviceToHost));
    if (std::fabs(hostC[n - 1] - 3.0f) > 1e-6f) {
        std::cerr << "verification failed\n";
        return EXIT_FAILURE;
    }

    CUDA_CHECK(cudaEventDestroy(start));
    CUDA_CHECK(cudaEventDestroy(stop));
    CUDA_CHECK(cudaFree(deviceA));
    CUDA_CHECK(cudaFree(deviceB));
    CUDA_CHECK(cudaFree(deviceC));
}
```

编译运行：

```bash
nvcc -O3 event_timing.cu -o event_timing
./event_timing
```

一次测量容易受到 GPU 升频、其他进程和缓存状态影响。正式 benchmark 应预热后循环多次，报告中位数或分位数，而不是只报告一次结果。

Event 计时也不是端到端耗时：上面的范围没有包含 host 端数据准备，也没有包含 event 范围之外的内存拷贝。如果要衡量用户请求延迟，仍应在 CPU 侧测量完整路径，并在结束前进行必要同步。

## 4. 两种等待：CPU 等待与 GPU 等待

CUDA Event 最容易混淆的地方，是 `cudaEventSynchronize` 和 `cudaStreamWaitEvent` 看起来都在“等待”，但等待者完全不同。

| API | 等待者 | 作用 |
|---|---|---|
| `cudaEventSynchronize(event)` | CPU 线程 | CPU 阻塞到 event 完成 |
| `cudaEventQuery(event)` | 无阻塞 | CPU 查询 event 是否完成 |
| `cudaStreamWaitEvent(stream, event)` | GPU stream | 在设备端建立 stream 间依赖 |
| `cudaStreamSynchronize(stream)` | CPU 线程 | CPU 等待指定 stream 已提交工作 |
| `cudaDeviceSynchronize()` | CPU 线程 | CPU 等待整个设备之前提交的工作 |

高性能流水线通常更偏爱 `cudaStreamWaitEvent`。它不会把控制权拉回 CPU，也不会粗暴地同步整个设备。

## 5. Demo 二：跨 Stream 构造 copy-compute 依赖

假设 copy stream 负责把输入传到 GPU，compute stream 负责执行 kernel。kernel 必须等输入拷贝完成，但 CPU 没有必要停下来等待。

```cpp
// event_pipeline.cu
#include <cuda_runtime.h>

#include <cstdlib>
#include <iostream>

#define CUDA_CHECK(call)                                                   \
    do {                                                                   \
        cudaError_t error = (call);                                        \
        if (error != cudaSuccess) {                                        \
            std::cerr << "CUDA error: " << cudaGetErrorString(error)       \
                      << " at " << __FILE__ << ':' << __LINE__ << '\n';    \
            std::exit(EXIT_FAILURE);                                       \
        }                                                                  \
    } while (0)

__global__ void scale(float* data, int n) {
    int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index < n) {
        data[index] *= 2.0f;
    }
}

int main() {
    constexpr int n = 1 << 20;
    const std::size_t bytes = n * sizeof(float);

    // 异步 H2D/D2H 要想真正与其他工作重叠，host 内存应为 pinned memory。
    float* hostData = nullptr;
    float* deviceData = nullptr;
    CUDA_CHECK(cudaMallocHost(&hostData, bytes));
    CUDA_CHECK(cudaMalloc(&deviceData, bytes));

    for (int index = 0; index < n; ++index) {
        hostData[index] = 1.0f;
    }

    cudaStream_t copyStream;
    cudaStream_t computeStream;
    cudaEvent_t copyDone;
    cudaEvent_t computeDone;

    CUDA_CHECK(cudaStreamCreateWithFlags(&copyStream, cudaStreamNonBlocking));
    CUDA_CHECK(cudaStreamCreateWithFlags(&computeStream, cudaStreamNonBlocking));
    CUDA_CHECK(cudaEventCreateWithFlags(&copyDone, cudaEventDisableTiming));
    CUDA_CHECK(cudaEventCreateWithFlags(&computeDone, cudaEventDisableTiming));

    CUDA_CHECK(cudaMemcpyAsync(
        deviceData, hostData, bytes, cudaMemcpyHostToDevice, copyStream));
    CUDA_CHECK(cudaEventRecord(copyDone, copyStream));

    // GPU 侧依赖：不阻塞 CPU，也不等待 copyStream 中 copyDone 之后的操作。
    CUDA_CHECK(cudaStreamWaitEvent(computeStream, copyDone));
    scale<<<(n + 255) / 256, 256, 0, computeStream>>>(deviceData, n);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaEventRecord(computeDone, computeStream));

    // 回传也通过 event 建立精确依赖。
    CUDA_CHECK(cudaStreamWaitEvent(copyStream, computeDone));
    CUDA_CHECK(cudaMemcpyAsync(
        hostData, deviceData, bytes, cudaMemcpyDeviceToHost, copyStream));

    // 最终结果要被 CPU 使用，此处才需要 CPU 等待。
    CUDA_CHECK(cudaStreamSynchronize(copyStream));
    std::cout << "result: " << hostData[n - 1] << '\n';

    CUDA_CHECK(cudaEventDestroy(copyDone));
    CUDA_CHECK(cudaEventDestroy(computeDone));
    CUDA_CHECK(cudaStreamDestroy(copyStream));
    CUDA_CHECK(cudaStreamDestroy(computeStream));
    CUDA_CHECK(cudaFree(deviceData));
    CUDA_CHECK(cudaFreeHost(hostData));
}
```

对应的时间线如下：

```text
copyStream:     H2D copy ── copyDone ───────── wait computeDone ── D2H copy
                              │                       ▲
                              ▼                       │
computeStream:            wait copyDone ── scale ── computeDone

CPU:           提交上述操作后继续运行，最终消费结果前才同步
```

这里的关键不是多开两条 stream，而是准确表达数据依赖。没有 `copyDone`，compute stream 可能在输入尚未准备好时读取数据；如果改成 `cudaDeviceSynchronize`，结果虽然正确，却会阻塞 CPU，并破坏无关工作的并行机会。

## 6. Event 的实现原理：记录完成点，传播依赖

从编程模型看，event 可以拆成三个阶段。

### 6.1 Record：向 stream 插入记录命令

```cpp
cudaEventRecord(event, producerStream);
```

运行时把记录操作排在 producer stream 当前已提交工作的后面。它代表的不是“record 调用发生的 CPU 时刻”，而是 GPU 执行到这个队列位置的时刻。

### 6.2 Complete：GPU 到达标记位置

当 producer stream 中排在 event 之前的工作完成后，设备更新 event 的完成状态。若 event 启用了 timing，运行时还会保留计时所需的信息。

### 6.3 Observe 或 Wait：消费完成状态

消费方有两类：

- CPU 通过 query 或 synchronize 观察状态；
- 另一个 GPU stream 通过 `cudaStreamWaitEvent` 把后续命令挂在该状态之后。

因此 event 的本质不是“执行一次回调”，而是一个可被 CPU 和其他 stream 观察的 GPU 完成点。它类似异步系统中的 future/fence，但语义严格绑定 CUDA 的设备执行时间线。

## 7. 常见使用场景

### 7.1 GPU 微基准与算子性能分析

用 start/stop event 包围 kernel、多个 kernel 或内存拷贝，可以测量设备侧执行时间。典型场景包括 GEMM、attention、通信算子和自定义 CUDA kernel benchmark。

为了让结果可信，应做到：

- 测量前预热；
- start 和 stop 放在目标工作实际所在的 stream；
- 循环多次，避免一次性噪声；
- 明确是否包含数据传输；
- 不要在每个 kernel 后做全局同步。

### 7.2 计算与数据传输流水线

双缓冲系统常把数据分块，在不同 stream 上重叠 H2D、计算和 D2H：

```text
时间 ──────────────────────────────────────────────>
copy:     H2D(0)      H2D(1)      H2D(2)
compute:        K(0)        K(1)        K(2)
return:              D2H(0)      D2H(1)      D2H(2)
```

每个 buffer 槽位配套 event，可以表达“数据已到达”“计算已结束”和“槽位可以复用”。

### 7.3 异步内存池与资源回收

kernel 发射后，CPU 不能立即把它使用的 buffer 交给另一个请求。资源池可以在最后一次使用之后 record event：

```cpp
kernel<<<grid, block, 0, stream>>>(buffer);
cudaEventRecord(bufferReadyToRecycle, stream);
```

后台回收器通过 `cudaEventQuery` 检查完成状态，完成后再把 buffer 放回 free list。这样既避免 use-after-free，也避免为了回收内存调用全局同步。

深度学习框架的 caching allocator、推理引擎的 workspace 池和 KV Cache 管理，都可能使用类似机制。

### 7.4 多 Stream 算子依赖

一个任务可能由通信、计算和后处理组成：

```text
communication stream: all-reduce ── event
                                      │
compute stream:                  wait event ── next layer
```

Event 可以只同步真正有依赖的边，而不冻结整个设备。这是通信计算重叠、流水线并行和多 stage 推理调度的重要基础。

### 7.5 CPU 异步任务完成通知

CPU 可以周期性调用 `cudaEventQuery`，在 GPU 工作未完成时处理网络、调度或其他请求：

```cpp
cudaError_t status = cudaEventQuery(done);
if (status == cudaSuccess) {
    consumeResult();
} else if (status != cudaErrorNotReady) {
    CUDA_CHECK(status);
}
```

这比立即 `cudaEventSynchronize` 更适合事件循环，但要避免无休止忙轮询；实际系统通常会结合任务队列、退避或专门的完成线程。

## 8. Event 创建选项

默认创建的 event 支持计时：

```cpp
cudaEventCreate(&event);
```

如果只用于同步，通常应关闭 timing：

```cpp
cudaEventCreateWithFlags(&event, cudaEventDisableTiming);
```

这清楚表达了用途，也可降低不必要的计时开销。

如果 CPU 会调用 `cudaEventSynchronize` 且等待时间较长，可以使用：

```cpp
cudaEventCreateWithFlags(
    &event,
    cudaEventBlockingSync | cudaEventDisableTiming);
```

`cudaEventBlockingSync` 影响 host 等待方式，不会把异步 record 变成同步提交。具体等待策略和成本还会受到操作系统与 CUDA 运行时实现影响。

## 9. 容易踩的坑

### 9.1 Record 后立即认为 event 完成

```cpp
cudaEventRecord(event, stream);
useResultOnCpu();  // 错误：GPU 可能还没执行到 event
```

Record 只提交标记。CPU 使用 GPU 结果前仍需要 synchronize，或者通过 query 确认完成。

### 9.2 每一步都调用 cudaDeviceSynchronize

全局同步适合调试和程序最终收尾，但在热路径中频繁使用会让本可重叠的 stream 串行化。优先用 event 表达局部依赖。

### 9.3 用 Event 计时却测错 Stream

若 kernel 在 `workerStream`，event 却记录在另一个无依赖 stream，时间范围就无法包围目标工作：

```cpp
kernel<<<grid, block, 0, workerStream>>>();
cudaEventRecord(stop, anotherStream);  // 没有依赖，测量语义错误
```

最简单可靠的方式，是把 start、目标工作和 stop 放到同一 stream。多 stream benchmark 则应先构造汇合依赖，再记录结束 event。

### 9.4 复用 Event 时误解其含义

同一个 event 可以多次 record，后一次记录会更新它代表的完成点。资源池复用 event 时，必须确保旧一轮依赖已经不再需要它，否则可能把消费者绑定到错误的代次。

### 9.5 只关闭计时，却仍调用 elapsedTime

用 `cudaEventDisableTiming` 创建的 event 适合同步，不适合传给 `cudaEventElapsedTime`。计时 event 和同步 event 最好在命名与封装上明确区分。

### 9.6 忽略异步错误

kernel launch 错误可通过 `cudaGetLastError` 检查；执行期错误常在后续同步 API 中暴露。因此示例中的同步调用也必须检查返回值，而不是默认 event 一定成功完成。

## 10. 如何选择同步方式

可以按下面的决策顺序考虑：

```text
是否需要 CPU 立刻使用结果？
├─ 是：等待精确完成点
│     ├─ 已有 event → cudaEventSynchronize
│     └─ 只关心单条 stream → cudaStreamSynchronize
└─ 否：是否只是另一条 GPU stream 依赖它？
      ├─ 是 → cudaEventRecord + cudaStreamWaitEvent
      └─ 否 → 保持异步，必要时 cudaEventQuery
```

`cudaDeviceSynchronize` 不是不能用，而是同步范围最大。初始化、调试、测试程序退出前或确实需要设备全局完成时，它非常方便；在追求并行度的核心路径上，则应尽量用 event 和 stream 构造精确依赖。

## 总结

CUDA Event 是插入 stream 时间线的 GPU 完成标记。它主要解决四类问题：

- 用 GPU 时间线准确测量设备工作耗时；
- 让 CPU 查询或等待一段 GPU 工作；
- 在不同 stream 之间建立非阻塞依赖；
- 判断异步资源何时可以安全回收和复用。

真正掌握 Event 的关键，是区分“CPU 等待 GPU”和“GPU stream 等待另一个 GPU stream”。前者会影响 host 控制流，后者只在设备时间线上增加一条依赖边。高性能 CUDA 程序通常不是消灭同步，而是把粗粒度全局同步改写成尽可能精确的 event 依赖。
