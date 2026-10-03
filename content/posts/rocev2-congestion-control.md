---
title: "RoCEv2 拥塞控制详解：ECN、CNP、DCQCN 与 PFC"
date: 2026-10-03T18:32:00+08:00
draft: false
tags: ["RoCE", "RDMA", "拥塞控制", "DCQCN", "数据中心网络"]
categories: ["网络", "技术原理"]
summary: "沿一次多发送端争用出口的过程，解释 ECN 标记、CNP 反馈、DCQCN 调速与 PFC 逐跳流控的分工，并讨论反馈时延、缓冲预算、无损与有损部署，以及计数器排障方法。"
---

**RoCEv2 的典型拥塞控制链路是：交换机用 ECN 标记拥塞，接收端网卡用 CNP 反馈，发送端网卡运行 DCQCN 等算法调整发送速率。PFC 则是另一层逐跳流控，不等于端到端拥塞控制。**

RoCEv2 使用 IP/UDP 承载 RDMA 传输协议，但 UDP 不会替它完成拥塞控制。在常见硬件实现中，反馈处理和发送节奏控制由 RNIC 承担。本文以经典 DCQCN 方案为主线，不把它当成所有 RoCEv2 网卡必须使用的唯一算法，也不把某代设备的默认参数当成协议常量。

## 1. 先分清：拥塞控制、流控和可靠传输

这三件事可以同时发生，却回答不同的问题。

| 机制 | 主要解决的问题 | 典型动作 | 不能据此推断什么 |
| --- | --- | --- | --- |
| 端到端拥塞控制 | 多条流的总发送速率超过网络承载能力 | 通知源端，调整流的注入速率 | 不能增加瓶颈链路带宽，也不能保证完全没有排队 |
| PFC 逐跳流控 | 相邻节点之间的特定优先级缓冲面临溢出 | 暂停直连上游相应优先级的发送 | 不等于按每条流公平分配带宽 |
| 可靠传输 | 丢失或错误的数据如何恢复并正确交付 | 确认、序号检查、重传等 | 能重传不等于重传没有性能成本 |

这里的可靠传输，指常见的 RC（Reliable Connection）等可靠 RDMA 传输服务，不能泛化到所有 QP 类型。可靠性由 RDMA 传输机制和端点实现承担，不是 UDP 提供的能力。

理解拥塞控制时，需要盯住两个量：**瓶颈的服务能力，以及所有竞争流的总到达速率。** 缓冲可以暂存两者的差额，但不能长期消化持续的超额输入。

## 2. 从一次出口争用看 ECN 与 CNP

假设 A、B 各以 100 Gbit/s 向 R 发送数据，两条流共享交换机 S 的一个 100 Gbit/s 出口。不考虑其他流量、协议开销和突发变化。

```
Sender A (RP) ----\
                  +--> Switch S (CP) --> Receiver R (NP)
Sender B (RP) ----/       100 Gbit/s
                         shared output

R -- CNP for flow A --> A
R -- CNP for flow B --> B
```

总输入是 200 Gbit/s，出口只能送走 100 Gbit/s，差额会使队列增长。理想化的公平稳态是两条长流各获得约 50 Gbit/s；这只是帮助理解目标的例子，不是任何拓扑、时延和流量组合下的保证。

### 2.1 CP：交换机把拥塞写进数据包

CP 是 Congestion Point，即拥塞点。交换机可以根据队列占用及配置的标记策略，对经过的 ECN-capable 报文设置拥塞标记。

IP 头中的 ECN 字段占两位：

| ECN 编码 | 含义 |
| --- | --- |
| `00` | Not-ECT，没有声明支持 ECN |
| `01` | ECT(1)，ECN-capable |
| `10` | ECT(0)，ECN-capable |
| `11` | CE，Congestion Experienced，经历了拥塞 |

**ECT 表示能够参与 ECN，CE 才表示已经经历拥塞。** 标记后的数据仍向接收端转发，因此网络不必等到丢包后才让端点知道发生了拥塞。ECN 也不是“不丢包”的承诺：标记策略不会创造额外缓冲。

以一种常见的 RED 风格策略为例，队列较短时不标记，进入标记区间后提高标记概率，压力更大时提高到更强的标记水平。具体采用瞬时还是平均队列、阈值单位、最大标记概率以及共享缓冲规则，都取决于交换机实现，不能仅凭两个阈值名称判断。

ECN 字段只提供很少的信息。一个 CE 标记不会直接告诉发送端“哪台交换机堵了多少字节、还剩多少带宽”，也不会告诉它应当精确降到多少 Gbit/s。这正是后面还需要控制算法的原因。

### 2.2 NP：接收端把标记变成反馈

NP 是 Notification Point，即通知点。接收端 RNIC 看到 CE 标记后，为相应流向原发送端发送 CNP，完整名称是 Congestion Notification Packet。

这里容易混淆方向：**典型 RoCEv2 路径是交换机标记数据，接收端生成 CNP，而不是交换机直接生成端到端 CNP。** CNP 是拥塞反馈报文，也不是数据已经可靠交付的确认。

反馈通常有抑制或合并机制，不能认为每收到一个 CE 数据包就必然产生一个 CNP。经典 DCQCN 设计限制每条流的 CNP 生成频率。因此，接收的 CE 包数量和发出的 CNP 数量本来就不应要求相等。

本文的“发送端”与“接收端”始终按**数据流方向**定义。执行 RDMA Read 时，发起读取请求的机器通常是数据接收方，不能把应用操作的发起者机械地当作需要降速的一方。

### 2.3 RP：发送端改变数据注入速度

RP 是 Reaction Point，即反应点。发送端 RNIC 收到 CNP 后，由相应的拥塞控制器更新状态并调整发送节奏。经典 DCQCN 以流为控制对象；具体硬件的状态与 QP、流及其他上下文如何关联，应以设备文档为准。

于是，闭环可以概括成：

1. 总输入大于出口能力，交换机队列积压。
2. 交换机标记数据包，接收端反馈 CNP。
3. 源端调低注入速率，队列增长放缓或开始消退。
4. 控制器之后逐步探测可用带宽，避免长时间过度退让。

“反馈到了”与“限速生效了”是两个可分别观察的环节，排障时不应合并判断。

## 3. DCQCN 如何决定降多少、何时恢复？

ECN/CNP 是信号通道，DCQCN 是利用这些信号的控制算法。阅读经典算法时，可以先理解三个状态：当前发送速率、用于恢复的目标速率，以及拥塞程度估计值 `alpha`。

### 3.1 降速不是固定砍半

2015 年 DCQCN 论文给出的核心更新可用下面的示意表达。它不包含完整初始化、事件抑制、计时器和硬件速率边界，不能直接作为可部署控制器：

```python
target_rate = current_rate
current_rate = current_rate * (1 - alpha / 2)
alpha = (1 - gain) * alpha + gain
```

`alpha` 反映近期拥塞反馈，`gain` 控制平滑速度。按这条降速式计算，若当前 `alpha` 为 1，速率降为原来的 50%；若为 0.1，速率降为原来的 95%。这两个数只是公式示例，不能脱离初始化与反馈历史当成固定配置。

这里有两点值得注意：

- 更新目标速率，是为了给之后的恢复保留参照，不是把线路标称带宽作为永远不变的目标。
- `alpha` 是根据反馈过程维护的状态，不能仅凭名称就解释为“当前 RTT 内 CE 包或字节的精确比例”。反馈已经经过生成间隔与合并策略处理。

### 3.2 恢复不是立刻回到线速

无新 CNP 的时间段会使拥塞估计衰减。经典 DCQCN 随后借助时间和发送字节数触发恢复：先向已有目标速率靠近，再通过加性增加等阶段继续探测更高的可用带宽。

从控制系统角度看，保存目标值与逐步探测分别回答两个问题：“刚才退让掉的带宽是否可以拿回来”，以及“当前竞争关系是否允许进一步增加”。恢复太激进会重新制造积压；太保守则可能让瓶颈链路闲置。这是理解参数取舍的方式，不是为所有设备给出同一组最优数值。

### 3.3 原论文的时间参数，不是协议常量

原论文部署示例采用约 50 微秒的 CNP 生成间隔，以及约 55 微秒的无反馈衰减时间参数。这些数字描述当时的实现与环境，**不是 RoCEv2 标准规定所有网卡都必须使用的固定值，也不是今天可直接照抄的默认参数。**

反馈合并会降低处理开销，也改变发送端看到的反馈节奏。因此，网络 RTT、CNP 生成策略、发送端计时器和速率恢复速度必须放在一起理解，不能只调其中一个旋钮。

## 4. 为什么端到端调速仍然需要缓冲？

回到 A、B 的例子。假设从队列开始告警，到发送端降速真正影响瓶颈，经过了 20 微秒，并且这段时间内总输入仍维持 200 Gbit/s。

以十进制单位计算，额外积压约为：

```
excess_rate = 200 Gbit/s - 100 Gbit/s = 100 Gbit/s
feedback_delay = 20 us

extra_buffer = excess_rate * feedback_delay / 8
             = 250,000 bytes
             ≈ 244 KiB
```

这是忽略包开销和流量变化的预算示例，**不是“缓冲设为 250 KB 就足够”的配置建议**。真实占用还包括原有队列、在途报文、其他竞争流和突发。

更重要的是，这里的反馈延迟并不只是链路 RTT。它还可能包含标记数据到接收端的时间、反馈生成等待、CNP 的返回路径，以及 RNIC 控制器和发送队列响应时间。

这也解释了为什么“平均吞吐没有超线速”仍不足以证明不会拥塞：平均值可以掩盖短时间的集中到达。容量规划和验证需要观察时间尺度与业务突发，而不只是端口速率的长期平均数。

## 5. PFC 是逐跳流控，不是另一个 DCQCN

PFC（Priority-based Flow Control）由 IEEE 802.1Qbb 引入。它允许节点请求直连上游暂停某些优先级的流量，而不是像传统全链路 PAUSE 那样暂停全部流量。

其作用边界是**一跳链路与优先级类别**，不是“远端某条应用流”：

```
Upstream device -- data --> Downstream device
                <-- PFC --

Pause selected priorities on this link;
resume according to the pause state and subsequent control frames.
```

无损部署通常希望 ECN 和端到端调速尽早发挥作用，让 PFC 主要应对反馈来不及消退的突发。PFC 发出后，上游也不会在零时间内停止，因此还需要为暂停生效前继续到达的数据保留 headroom。这是一个局部停止过程的缓冲预算，不能直接等同于上一节的端到端拥塞反馈预算。

PFC 并不知道同一优先级里的哪条流“更应该退让”。如果某条流导致暂停，其他共享该优先级与受影响链路的流也可能一起被挡住。下游暂停还可能使上游继续积压，从而形成更多逐跳暂停。

**暂停可以限制缓冲溢出，却不能自动获得理想的流级公平性。** 这也是只看“没有丢包”，仍可能遗漏队头阻塞、吞吐失衡和尾延迟恶化的原因。

## 6. 参数为什么不能只比较两个阈值？

工程上常说“让 ECN 先发挥作用，PFC 后兜底”。这个目标有用，但不能直接简化成一个对所有交换机都成立的数值不等式。

ECN 可能依据出口队列占用进行标记，PFC 可能依据入口优先级组或共享缓冲水位触发。它们的观察对象、计数单位和共享池规则未必相同。**先确认两个阈值到底在量什么，再讨论谁应当更早触发。**

核对配置时，可以按下面的顺序检查：

| 环节 | 要核对的问题 | 常见误判 |
| --- | --- | --- |
| 分类与队列映射 | RDMA 数据和反馈报文进入了预期的流量类吗？ | 只设置一个 DSCP 值，就认为整条路径的映射一致 |
| ECN 标记 | 哪个队列计数触发标记？阈值和概率含义是什么？ | 将不同设备上的同名参数当作同一含义 |
| 反馈回程 | CNP 能及时到达源端吗？调度与暂停策略合适吗？ | 只检查大流量的数据方向，忽略反馈方向 |
| RNIC 控制器 | 接收反馈、处理反馈和限速功能是否正常？ | 看到 CNP 计数增加，就认定所有流都已正确降速 |
| 缓冲预算 | 是否覆盖实际链路、突发和控制响应时间？ | 从另一个端口速率或拓扑照抄字节阈值 |

不宜只追求“ECN 越少越好”或“PFC 次数必须为零”。一些标记可能是正常闭环工作的信号；反过来，完全没有标记也可能只是没有负载，或标记路径没有配置好。评价配置需要同时看业务吞吐、队列占用、暂停持续时间、丢包恢复和尾延迟。

## 7. 不开 PFC，RoCEv2 能不能运行？

不能把“RoCEv2 必须无损”当成脱离设备与部署条件的绝对判断。例如，NVIDIA Cumulus 的 RoCE 配置明确区分启用 ECN 与 PFC 的 lossless 模式，以及启用 ECN、不开 PFC 的 lossy 模式。

但交换机有这个开关，不代表任意端点与任意负载都适合关闭 PFC。需要核对 RNIC 的丢包恢复能力、拥塞控制实现，以及实际突发下的吞吐和尾延迟。RC 能恢复数据，也不意味着恢复时的停顿、重复传输或带宽开销可以忽略。

做选型与压测时，至少应覆盖多对一集中发送、长短流混合，以及接近实际部署规模的竞争关系。判断依据应是业务目标能否满足，而不是只凭“网卡支持 RoCEv2”这个标签。

## 8. 用计数器检查闭环，而不是猜测哪里堵了

一些 Mellanox/NVIDIA RNIC 暴露了下面的硬件计数器。名称、作用范围与可用性受驱动和固件影响，不能假定所有网卡都有同样的接口。

| 计数器 | 从什么角色观察 | 能说明什么 |
| --- | --- | --- |
| `np_ecn_marked_roce_packets` | 数据接收端／NP | 收到了带 ECN 拥塞标记的 RoCE 数据 |
| `np_cnp_sent` | 数据接收端／NP | 生成并发送了 CNP |
| `rp_cnp_handled` | 数据发送端／RP | 控制器处理了 CNP |
| `rp_cnp_ignored` | 数据发送端／RP | 有 CNP 未被采用，原因需要结合设备实现判断 |

可以先确认本机的 RDMA 设备与端口，再只读地检查存在的计数器。以下 `mlx5_0` 和端口 `1` 只是示例，不是现场配置要求：

```bash
rdma_device=mlx5_0
rdma_port=1
counter_dir="/sys/class/infiniband/${rdma_device}/ports/${rdma_port}/hw_counters"

for counter in np_ecn_marked_roce_packets np_cnp_sent rp_cnp_handled rp_cnp_ignored; do
    if [ -r "${counter_dir}/${counter}" ]; then
        printf '%s: ' "$counter"
        cat "${counter_dir}/${counter}"
    fi
done
```

诊断时看**同一时间窗口内的增量**，并对齐数据方向。CE 与 CNP 数量不相等可能只是正常的反馈合并；被忽略的 CNP 也不能仅凭名称就判定为故障。

如果接收端持续看到 CE，却没有相应反馈，检查通知功能和反馈抑制条件；如果接收端反馈明显增加，而发送端处理计数不匹配，检查回程路径、配置及计数器口径；如果反馈和处理都存在，仍出现长时间 PFC 暂停，则继续检查实际限速效果、控制响应与缓冲配置。

最后把这些端点信号与交换机队列、ECN 标记、PFC 暂停持续时间和业务延迟放在同一时间线上。它们能帮助定位闭环断在哪一段，但任何一个累计计数都不能独立证明“拥塞控制已经正常”。

## 小结

**ECN 负责标记，CNP 负责反馈，DCQCN 等算法负责端到端调速，PFC 负责局部逐跳暂停。** 真正需要调好的，是这些机制与分类、缓冲、反馈时延、丢包恢复共同构成的系统，而不是某一个“开启拥塞控制”的开关。

## 参考资料

- [RFC 3168：The Addition of Explicit Congestion Notification to IP，ECN 字段与 CE 语义](https://www.rfc-editor.org/rfc/rfc3168.html)
- [Zhu 等：Congestion Control for Large-Scale RDMA Deployments，SIGCOMM 2015；重点参考第 3 节的 DCQCN 算法与原始时间参数](https://conferences.sigcomm.org/sigcomm/2015/pdf/papers/p523.pdf)
- [Juniper：Data Center Quantized Congestion Notification，DCQCN 与交换机 ECN、PFC 的协作](https://www.juniper.net/documentation/us/en/software/junos/traffic-mgmt-qfx/topics/topic-map/cos-qfx-series-DCQCN.html)
- [Juniper：Introduction to Congestion Control in AI/ML Networks，拥塞控制与数据中心部署考虑](https://www.juniper.net/documentation/us/en/software/nce/congestion-control-ai-ml/congestion-control-ai-ml.pdf)
- [IEEE 802.1Qbb：Priority-based Flow Control，按优先级进行逐跳暂停的标准项目说明](https://www.ieee802.org/1/pages/802.1bb.html)
- [NVIDIA Cumulus Linux 5.9：RoCE，lossless／lossy 模式、流量类与 ECN/PFC 配置示例](https://docs.nvidia.com/networking-ethernet-software/cumulus-linux-59/Layer-1-and-Switch-Ports/Quality-of-Service/RDMA-over-Converged-Ethernet-RoCE/)
- [Juniper：Configure CoS PFC with Congestion Notification Profiles，入口／出口流控配置及链路参数；此处 profile 的 CNP 缩写不同于 RoCEv2 的反馈报文](https://www.juniper.net/documentation/us/en/software/junos/traffic-mgmt-qfx/topics/task/cos-congestion-notification-qfx-series-cli.html)
- [Oracle Linux：RoCEv2 Congestion Counters Explained，NP/RP 计数器与反馈方向](https://blogs.oracle.com/linux/rocev2-congestion-counters-explained)
- [Linux v6.12：mlx5 RDMA counters.c，硬件拥塞计数器名称及统计实现](https://github.com/torvalds/linux/blob/v6.12/drivers/infiniband/hw/mlx5/counters.c)
- [RoCEv1 与 RoCEv2：从以太网二层交换到三层路由，封装与传输语义的背景](https://yangyang233333.github.io/posts/rocev1-rocev2-ethernet-l2-l3/)
