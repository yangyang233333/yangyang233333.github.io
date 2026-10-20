---
title: "DeepInfer 源码拆解：DeepSeek 怎样让 GPU 少干活、少等人"
date: 2026-10-20T17:51:40+08:00
draft: false
tags: ["DeepInfer", "DeepSeek", "推理优化", "MoE", "推测解码", "KV Cache"]
categories: ["AI 基础设施", "源码阅读"]
summary: "沿 DeepInfer 的 C++ 调度器、Worker 和模型执行路径，拆解异步调度、DSpark 变长验证、SWA 有界回放、Wide-EP、专家负载均衡与融合算子，也聊聊当前开源版本和性能数据的边界。"
---

2026 年 10 月 20 日，DeepSeek 把自家的推理框架 DeepInfer 开源了。我沿着请求入口往下读了一圈，最有意思的并不是它又接上了几个快算子，而是它在反复追问同一件事：**这份活必须做吗？必须现在等它做完吗？**

主模型不必把所有草稿都验一遍，Decoder 不必把长 prompt 每层都重读，GPU 也不必停下来等 CPU 记完上一轮的账。DeepInfer 的几项关键优化，就落在这些看起来很不起眼的空隙里。

本文固定分析仓库提交 `8b16149741a4935e1dec8cea27853f273b374c5b`，对应 2026 年 10 月 20 日的源码与 Q&A。下面提到的性能收益，会区分官方披露和代码推导；我没有在 SM100 或昇腾 950 上跑端到端性能测试。

## 先看清楚，开源的是哪一层

DeepInfer 的 README 定位很明确：这是支撑 DeepSeek 大规模线上服务的推理框架，而这次开放的使用入口是**离线推理，以及规模化服务需要的引擎核心能力**。它不是拉下来就能原样复制官方 API 集群的一整套平台。

当前支持的模型是 DeepSeek-V4.1-Flash，硬件列的是 NVIDIA SM100 GPU 和华为昇腾 950 NPU。不能因为底层某个算子库支持更多芯片，就顺手推断 DeepInfer 也支持它们。模型适配同理，至少这个快照不是一个任意 Transformer 都能直接塞进去的通用模型仓库。

源码大致分成几层，顺着它们读，比较不容易迷路：

- `deepinfer/entrypoints/` 和 `engine/llm_engine.py` 接住请求，组织引擎生命周期和每一步执行。
- `csrc/engine/` 负责调度、序列状态、缓存块分配和输入元数据准备，关键批调度逻辑在 C++ 里。
- `deepinfer/worker/` 把模型计算、采样和上一轮输出处理接成流水线，也管理图捕获与设备缓存。
- `deepinfer/model_executor/` 落到 V4.1 的模型结构、MoE、稀疏注意力、草稿模型与专家重排。
- `vendor/` 固定了 FlashMLA、DeepGEMM、DeepEP、DeepSelect 等依赖的提交；自定义的小算子主要在 TileLang 目录。

这里有个容易误读的地方。README 说完整服务还需要一个 KV cache system，不代表仓库连本地 KV cache 都没有。`block_manager.hpp` 和 `cache_engine.py` 已经有分页块、每序列状态槽和设备侧布局；缺的是生产级的跨请求缓存存储与管理系统，以及完整的 VE/Prefill/Decode 分离部署和请求级负载均衡。

这条边界甚至能在代码里直接看到：`LLMEngine` 留着 `init_pd_split`、`do_save_kvcache`、`send_processed_prompts` 等扩展钩子，基类没有把外部传输、持久化服务实现进去。接口已经接到门口，门外那套基础设施没有一起搬进来。

另外，README 坦率写了它最初主要建立在 vLLM 之上，也受 SGLang 和 TensorRT-LLM 启发。与其把它想成突然冒出来的另一套万能框架，不如把它看成围绕 DeepSeek 模型和部署条件长期打磨的专用引擎。

## 异步调度：别让 GPU 等 CPU 记账

先看最普通的一步 Decode：CPU 挑请求，GPU 跑主模型和采样，CPU 取回结果，更新长度、结束状态与草稿信息，然后再挑下一批。

这套流程直观，但 CPU 做后处理和下一轮调度时，GPU 可能已经没活了。算子再快，停在那里等人也不会多吐一个 token。

DeepInfer 的 Zero-Overhead Batch Scheduler 要隐藏的就是这段空档。关键入口在 `Worker.execute_model_step()`，它把一轮执行排成：**先提交本轮主模型计算，再处理上一轮输出，接着提交本轮采样**。对于启用异步调度的 Decode，本轮输出处理延后到下一轮；Prefill 或关闭异步调度时则当场处理。

提交设备计算不等于 CPU 必须等它算完。因此，本轮前向已经在 GPU 上跑着的时候，CPU 可以处理上一轮采样结果；下一批也能在前一批仍在途时准备。这里的重点不是“多开一个线程”，而是改变等待关系。

麻烦也随之来了：下一轮提前调度时，CPU 还不知道上一轮到底接受了几个草稿 token，甚至还没拿到最新 token。它不能拿旧的长度和旧的输入就开跑。

`csrc/engine/zos.cpp` 与 Worker 的元数据修正路径处理了这件事。相邻批次之间建立 `remap`，把本轮序列对应到上一轮设备结果；GPU 根据真实结果修正输入和序列长度。需要回到 CPU 的元数据通过 pinned buffer 与事件衔接，而不是先把整轮结果同步回来，再从头准备下一轮。

还有一段很值得读的代码叫 `DeferredRelease`。异步以后，“请求已经结束”与“设备不再使用它的存储”不是同一个时刻，序列元数据、输出对象、克隆时 pin 住的缓存块，都要在安全位置回收。把同步挪开，却提前释放内存，优化就变成了偶发错误制造机。

所以，**zero-overhead 不是 CPU 开销变成零，而是尽量不让它暴露在 GPU 的关键路径上**。代码仍有必要的同步和生命周期管理，也不能保证所有负载下 GPU 都没有空泡。

## DSpark：草稿写得长，不一定更划算

推测解码的基本套路是让较轻的草稿模型先猜几个 token，再交给主模型批量验证。猜得准，一轮昂贵的主模型计算就能推进多个 token；猜得不准，后半段验证可能白花算力。

DeepInfer 对外把这套能力称为 DSpark，草稿执行代码主要在 `models/mtp/dflash.py`。一个重要细节是，草稿模型不仅给出候选 token，还给出 Confidence Head 预测的接受概率。

这份置信度不是“可信就直接输出”的通行证。**它只决定把验证算力花在哪里，候选 token 最后仍要走主模型的验证与采样路径。**

固定长度验证的缺点，是每个请求都被安排同样多的活。假设一条请求前两个草稿很靠谱，后面三个已经开始瞎猜；另一条请求五个都很稳，平均分配五个验证位就未必划算。

`csrc/engine/mtp_varlen.hpp` 做的是两层选择。

第一层，把每个位置的置信度累乘成“这个草稿前缀能走到这里”的概率。第三个 token 有价值的前提，是前两个也能通过；不能看到第三个位置置信度高，就跳过前面的依赖。代码用的估计可以写成 `prefix_probability[j] = confidence[0] × … × confidence[j]`。

第二层，把所有请求的候选验证位放到一起，按预期收益排序，尝试不同的总验证预算。它不是选能塞进显存的最大批，而是比较：

`预期有效输出 token 数 / 这批验证预计花费的时间`

时间也不是拍一个固定常数。引擎启动时会 profile Decode 性能曲线，不同 token 数要落到不同的已捕获 graph 档位，padding 也有成本；运行中再用前向与采样耗时的反馈做修正，中位数估计降低偶发波动的影响。最后选择有效吞吐最高的预算。

这就让策略有了一个很实在的判断：再验一个草稿能多挣多少 token？为了它跨到更大的 graph 档位，值不值？

它和异步调度的配合更有意思。CPU 提前排下一批时，最新置信度还没回传，于是 CPU 先用较早的统计确定总预算；GPU 再用设备上较新的置信度决定每个请求分到几个验证位。`schedule_mtp_varlen.py` 里能看到累乘、全局排名计算，以及通过 `remap` 写出每条序列长度的过程。没必要为了分配长度，先把流水线停住。

README 给的开启方式是：

```bash
export DEEPINFER_ENABLE_MTP_VARLEN=1
export DEEPINFER_USE_NEXTN_DECODE=5
```

第二个参数控制最大草稿长度，不是承诺每一步都验证五个。官方 Q&A 披露，在 EP64/DP64、每 rank 约 60 并发的运行点，相比 fixed-5，动态长度验证的吞吐收益约为 4%–7%。这是官方对特定配置的结果，不是本文实测，也不是换一个负载仍然成立的常数。

我更在意的是它的优化目标：不是想办法让主模型接受“最多的草稿”，而是让每毫秒验证得到更多有效输出。

## SWA 有界回放：有些历史，只需补最后一段

这项优化叫 SWA Bounded Replay，但理解它最好别从环境变量开始，要先看 V4.1 的模型结构。

代码里的 YoCo 把主模型分成前面的 producer 层和后面的 consumer 层。后半段可以消费前半段产生的共享信息，压缩的长程 KV 不必靠每个 consumer 层都完整跑一次 prompt 才得到；不过，各层自己的滑动窗口注意力状态仍需要准备。

SWA 的窗口有限，不代表恢复状态天然只用回放一个窗口。多层叠起来，历史依赖会继续向前传。官方 Q&A 举的是 20 个 Decoder 层、128-token 窗口：精确重建需要处理最后 2560 个 token。

Bounded Replay 做了一个明确的取舍：只回放较短的尾段，并把 SWA 的可见范围也限制在回放区间里，恢复**近似的**窗口状态。它不再为恢复这份局部状态，把整条很长的上下文一路送过 Decoder。

这个“近似”不能略掉。它不是对任意输入都逐位等价的数学变形，也不是只换个缓存格式。模型结构给了它省计算的空间，状态重建方式则引入了需要评估的近似。

开源实现还有一个关键区分：`mask` 和 `compact`。

- `mask` 收窄 SWA 可见范围，但保留全部 query 行，consumer 层照样对这些行做计算，主要用于对齐和排查。
- `compact` 真正移除不需要的 query 行，同时调整 hidden states、残差和元数据，才能省掉后续 consumer 层的计算。

`deepseek.py` 在边界切换 consumer 元数据，`v4_modules/yoco.py` 收紧后半段输入，同时把 full-span hidden 与元数据留给仍需完整区间的 KV 生产路径。这不是给完整计算结果套一个 mask 就算优化完成。如果开了 CP，保留 token 在各 rank 上的分布也会变化，代码还要在边界重新 gather 和分配。

README 推荐的 V4.1 配置是：

```bash
export DEEPINFER_YOCO_PREFILL_TRUNCATION_MODE=compact
export DEEPINFER_YOCO_PREFILL_KEEP_TOKENS=140
```

为什么讲的是 128-token 窗口，配置却是 140？因为还得留出推测解码回退的余量。代码明确检查 `keep_tokens >= window_size + max_backtracking_tokens`；默认最多五个草稿时，回退下限是六个 token，140 覆盖了窗口和这份余量。它不是另一个模型窗口大小，也不是随便填得越小越快。

同样，保留 140 个尾部 query，**不等于把整条序列的长程 KV cache 截成 140 个 token**。被省掉的是特定层的 Prefill 工作，已经准备好的压缩 KV 仍然承担长程信息。

官方系统还把这个思路用在前缀缓存命中后、SWA 状态缺失的恢复上：复用 Block KV，只补尾段的窗口状态。开源仓库能看到对应状态和元数据支持，但不能据此认为完整的跨请求缓存服务也开源了。

Q&A 提到 Decoder 有界回放使总 Prefill 计算量近乎减半。理解成“少做了大量 Decoder 工作”比较准确，不该直接翻译成端到端延迟必然减半；Encoder、通信、排队等成本都还在。

## Wide-EP 和 EPLB：别让最忙的专家拖住所有人

MoE 很容易给人一种错觉：每个 token 只激活少数专家，当然就便宜了。问题是，一条请求每步只带来一点 token，专家权重却很大，GPU 可能花很多时间搬权重，矩阵乘也不够饱满。

DeepInfer 的基本组织方式是 Attention 做 DP，MoE 做 EP。Attention 的不同副本处理不同请求，到了专家层，再把这些 token 按路由汇到放置相应专家的设备上。

Wide-EP 的价值，一边是让每卡负责的专家权重更少，减少每步专家部分的访存；另一边是让更多 DP 副本的 token 汇入专家计算，改善矩阵乘的批量。官方部署建议里，Prefill 倾向较小的 EP/DP，Decode 倾向 64–128 卡的 Wide-EP，前提是处在适合的高带宽互联域里。

这不是“卡数翻倍就自然更快”。EP 把一些访存和计算问题换成了通信问题；互联跟不上，省出来的时间也可能被 token 分发吃掉。仓库支持 balanced CP，但当前 Q&A 描述的线上策略又明确说暂未用 TP、CP，框架支持与生产实际启用不能混为一谈。

另外，专家的活不会平均落下来。几个热点专家挤在同一张卡，整组就得等它。DeepInfer 的 EPLB，也就是 Expert-Parallel Load Balancer，处理的是**专家放置和副本负载**，不是给用户请求挑服务副本的网关负载均衡。

`eplb/policy.py` 把策略拆成两步：先按历史 token 负载，为热点 logical expert 分配额外的 physical expert 副本；再把这些副本安排到各 rank 的固定容量里。复制的是同一位专家的权重，不是新添一位有新知识的专家。

放置目标也比“各卡平均 token 数接近”更细。`_batch_greedy_indices()` 保留了多个历史 step 的负载向量，尝试最小化各 step 的最大 rank 负载之和。两位专家总负载相同，但如果每次都同时忙，把它们塞进同一张卡会比交错忙的两位更糟。

运行时会汇总统计，定期重新计算映射，再搬运专家权重。`mlp.py` 的 gate 把 logical-to-physical 映射和副本数传给路由算子，因此新增副本真正参与分流。重排本身也有成本，历史窗口和重排周期需要跟流量变化匹配，不是每一步洗牌都划算。

我单独加载了未修改的策略模块，运行仓库原有的 EPLB 测试：26 项 CPU 用例通过，6 项 CUDA 用例跳过。验证覆盖了结构一致性、确定性、边界分布以及放置目标；它能给策略分析补一份证据，不能替代真实集群上的重排和性能验证。

## 算子层：大活合并，小活也别来回搬

读到这一层，会发现 DeepInfer 不是只把几个外部库 import 进来。模型执行路径、权重排布、低精度格式和通信缓冲都在为这些算子配合。

### Mega MoE 把通信也算进融合

常规 MoE 路径大致是分发 token、第一层线性变换、SwiGLU、第二层线性变换，再合并结果。每段分别 launch，通信与计算之间就会留下切换和等待。

DeepGEMM 的 Mega MoE 把 EP dispatch、两层线性变换、激活和 EP combine 融进一个 mega-kernel，在里面重叠 NVLink 通信与 Tensor Core 计算。DeepInfer 在 `mega_moe_forward()` 里接这条路径，使用 symmetric memory buffer，并直接从执行过程中累计专家接收统计。

Gate 还有独立的 Mega Gate 调用，不要把名字听成“连路由也全在同一个 MoE kernel 里”。模型加载后，专家权重会预先变换到 Mega MoE 需要的布局；FP8 输入与 scale 也尽量由前面的 norm/cast 直接写进目标缓冲，减少再复制一次。

普通的 dispatch/GEMM/combine 路径也保留着，通过 DeepEP 的通信能力衔接。是否走 Mega MoE，有硬件、数据类型和配置条件，不是所有平台都使用完全一样的内核。

### 注意力不只要算得快，也要少选、少搬

FlashMLA 负责注意力核心计算。适用路径还会把 Q 侧处理、RoPE、注意力、输出 RoPE 和 FP8 cast 合并，减少小 kernel 的启动以及中间张量往返。为了接上这条融合路径，`attn.py` 会提前重排相关投影权重：把准备工作移到加载时，而不是每轮都忙一次。

稀疏注意力还要挑值得读取的 KV。`index_attn.py` 调用 DeepGEMM 做 indexer 评分，再交给 DeepSelect 做 top-k；配置支持候选块限制时，先缩小候选范围，后续评分与选择只处理那部分。跨层的 index、KV 和候选结果共享，则由 `cross_layer.py` 统一描述和校验。

这里节省的不只有最终 attention 的算术量。长上下文下，选哪些 KV、读取哪些 KV、是否每层重新选一次，本身就可能很贵。这些优化又依赖模型的稀疏与共享结构，不能拿掉相应配置，给任意模型直接套上去。

### 低精度是分部位安排的，不是一键全变 FP4

MoE 路由专家可以走 FP8 激活乘 FP4 权重，Gate 投影仍用 BF16。缓存也不是统一一种格式：代码定义了 FP4 Main KV 的 per-16 scale 布局、MXFP8 Main/SWA 的 per-32 布局，Indexer 则有自己的 FP4 数据与 scale 存储方式。

具体格式由模型配置选择；例如 SWA 在这里走 MXFP8，而不是因为专家用了 FP4 就跟着全部压成四位。压缩权重、缩短 KV、减少数据搬运会互相帮忙，但它们不是同一个开关。算子还必须理解 scale 的排布、对齐和缓存物理格式，才接得上这条执行链。

## 跑得快之外，还得能复现、能约束输出

批量推理里，一个很烦的现象是同样的请求，换了同批的邻居，结果就略有变化。浮点归约顺序和算子选择的变化，都可能让数值差异一路传下去。

DeepInfer 把 Batch-Invariant Inference 放进了公开特性。`configure_worker_kernels()` 会在开启后要求 DeepGEMM 和 TileKernels 使用确定性算法，注意力路径也传递相关配置。它解决的是不同 batch 组成下的模型数值一致性，不能仅靠给随机数设个 seed 替代。

README 对确定性采样给的是这组条件：NVIDIA 平台、开启 batch invariant、关闭变长验证，再给 `SamplingParams` 固定的 `seq_seed`。

```python
from deepinfer import SamplingParams

sampling_params = SamplingParams(seq_seed=42)
```

对应环境变量是 `DEEPINFER_BATCH_INVARIANT=1`、`DEEPINFER_ENABLE_MTP_VARLEN=0`。关闭变长验证不等于关闭推测解码；要完全关闭草稿，另设 `DEEPINFER_USE_NEXTN_DECODE=0`。这些保证当前不能直接延伸到昇腾平台。

仓库还提供 batch invariant、KL 对比和采样分布测试；采样测试使用随机化 PIT 与 KS 检验，不只是检查输出看着像正常文字。本文没有执行这些需要模型和设备的测试。

结构化输出则接了 XGrammar，支持 JSON schema、正则和 EBNF。`SOMatcherBuilder` 用异步任务先编译约束，请求准备好之后才进入调度；生成期间逐步维护匹配状态，用合法 token mask 约束采样。大模型不是写完一个 JSON 再由调用方祈祷它能解析，而是在写的时候就受到语法限制。当然，格式正确仍不等于字段内容正确。

这次开源介绍里一并提到的 deepseek-recipe 属于另一层：它负责主流 API 协议、对话格式和模型 token 的编解码，不参与模型计算。想让自建服务的协议行为对齐官方，这一层同样重要；它也不等于 DeepInfer 已经自带完整在线 API 服务。

## 性能图很漂亮，但要看那行小字

README 同时放了 Prefill 和 Decode 曲线，测试规模分别是 4 张和 64 张 SM100 GPU，各自位于一个 scale-up domain。它们展示的是不同阶段的吞吐与延迟权衡，不能当作同一套卡数下的直接对照。

Decode 图的条件尤其值得看：EP64/DP64、131072-token 上下文、固定三个草稿，按 75% 的逐 token 接受率折算。图里的每步有效 token 数是 `1 + 3 × 0.75 = 3.25`，两条轴都据此缩放。

追进 `examples/benchmark.py` 和 `sweep/plot_pareto.py`，这份口径会更清楚。固定长度测试的原始吞吐先按全部接受计数，再通过给定接受率换算；它不是对任意真实请求流直接计数得出的有效输出吞吐。变长验证改变了每步工作量，绘图脚本还明确拒绝把同一套折算公式用到 VARLEN 数据上。

测试脚本也设置了 `DEEPINFER_FORCE_LOAD_BALANCE=1`。沿着 gate 路径能看到，它会使用强制的随机均衡路由，而不是保留真实请求的专家热点；稳态测量窗口还会剔除负载统计 flush 的影响，sweep 时关闭 profiler。这些做法有助于测引擎本身，但并不等于线上请求路由、真实专家偏斜和运维开销都包含在结果里。

因此，我会把公开曲线当作明确条件下的引擎性能画像，不据此宣布它全面超过某个框架。严肃比较至少要对齐模型版本、芯片与互联、上下文长度、实际接受率、并发、精度、缓存命中、TTFT 和每 token 延迟目标，不能只挑最大 tokens/s。

当前构建门槛也不低：README 要求 Linux、GCC 13 及以上、Python 3.12，NVIDIA 侧还要相应的 CUDA 环境。没有匹配的硬件，读源码和跑 CPU 策略测试能理解机制，却不能冒充完成了性能复现。

读下来，DeepInfer 给我的启发不是“又多了一招推理加速”，而是几项优化确实接到了同一条执行链上：变长验证要知道 graph 成本，也要适应异步调度；有界回放要保留回退余量，也要照顾 KV 的生产与消费关系；Wide-EP 要有通信内核，还得有人替热点专家分担工作。某个点单独看很漂亮，能一起运转才算数。

## 参考资料

- [DeepInfer 官方仓库与本文固定分析的提交：开源范围、支持条件和特性配置](https://github.com/deepseek-ai/DeepInfer/tree/8b16149741a4935e1dec8cea27853f273b374c5b)
- [官方《DeepSeek V4.1 Flash 推理部署 Q&A》：部署策略、DSpark、SWA Bounded Replay 与性能披露](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/doc/DeepSeek%20V4.1%20Flash%20%E6%8E%A8%E7%90%86%E9%83%A8%E7%BD%B2%20Q%26A.pdf)
- [C++ Scheduler：批调度、序列状态与 MTP 置信度累乘](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/csrc/engine/scheduler.hpp)
- [Worker.execute_model_step：Forward、上一轮 Post-process 与 Sample 的异步编排](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/deepinfer/worker/worker.py)
- [LLMEngine：请求生命周期、DeferredRelease 与缓存和 PD 分离扩展钩子](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/deepinfer/engine/llm_engine.py)
- [MTPVarlen：基于置信度、graph 性能曲线和运行反馈选择验证预算](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/csrc/engine/mtp_varlen.hpp)
- [GPU 变长验证长度分配：置信度累乘、全局排名与 remap](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/deepinfer/model_executor/layers/tilelang_ops/cuda/schedule_mtp_varlen.py)
- [YoCo 模型执行与 compaction 边界](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/deepinfer/model_executor/models/deepseek.py)
- [YoCo 输入元数据：可见范围、compact query 与回退余量检查](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/csrc/engine/input_metadata/prepare_tensor_input_v4.cpp)
- [EPLB 策略与原有测试：冗余专家副本、跨 step 峰值目标和结构验证](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/tests/models/test_eplb_policy.py)
- [专家放置算法 rebalance_experts_global 与 _batch_greedy_indices](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/deepinfer/model_executor/eplb/policy.py)
- [MoE 执行路径：Mega Gate、Mega MoE、低精度与通信缓冲](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/deepinfer/model_executor/models/v4_modules/mlp.py)
- [DeepInfer 固定依赖的 DeepGEMM：Mega MoE 通信与计算融合说明](https://github.com/deepseek-ai/DeepGEMM/tree/1fc9c66975ee322d628b3288534ca0e9ce12a64d)
- [注意力执行：融合路径与投影权重重排](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/deepinfer/model_executor/models/v4_modules/attn.py)
- [稀疏 Indexer：候选块限制、评分、top-k 与跨层复用](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/deepinfer/model_executor/models/v4_modules/index_attn.py)
- [缓存格式定义：FP4 Main KV 与 MXFP8 Main/SWA 的数据和 scale 布局](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/deepinfer/model_executor/layers/tilelang_ops/cache_common.py)
- [Worker kernel 配置：NVIDIA PDL、确定性算法与平台边界](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/deepinfer/worker/runtime_config.py)
- [结构化输出 SOMatcherBuilder：异步编译与请求入队](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/deepinfer/structure_output/so_matcher_builder.py)
- [Benchmark：稳态窗口、强制均衡路由与原始吞吐计数](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/examples/benchmark.py)
- [Pareto 绘图：固定长度接受率折算与 VARLEN 限制](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/examples/sweep/plot_pareto.py)
- [官方 Decode 性能图：EP64/DP64、128K 上下文、固定三个草稿](https://github.com/deepseek-ai/DeepInfer/blob/8b16149741a4935e1dec8cea27853f273b374c5b/doc/images/decode_pareto.png)
- [deepseek-recipe 官方仓库：API 协议与模型 token 编解码](https://github.com/deepseek-ai/deepseek-recipe)
