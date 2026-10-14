---
title: "RDMA 和 DPDK 都在用户态，差别到底在哪？"
date: 2026-10-14T14:00:00+08:00
draft: false
tags: ["RDMA", "DPDK", "RoCE", "内核旁路"]
categories: ["网络", "技术原理"]
summary: "从一次远端写和一次用户态收包出发，区分内核旁路、远端 CPU 旁路、内存访问语义与传输卸载，并结合 FaSST、eRPC、IRN 和 BURST 讨论性能边界。"
---

同样是绕过内核收发数据，一边调用 `ibv_post_send` 发起 RDMA Write，另一边调用 `rte_eth_tx_burst` 发送数据包，为什么不能说它们只是两种用户态网卡驱动？看**接收端的网卡把数据放到哪里，以及谁负责让这次通信成立**，差别就清楚了。

先限定比较范围：这里的 RDMA 主要指有硬件卸载的 RNIC 上，使用 RC（Reliable Connection）的远端 Read/Write；DPDK 主要指通过 Ethdev/PMD 收发普通以太网包。RDMA 还有需要远端应用参与的 Send/Receive、无可靠性的 UD；DPDK 也能调用各种硬件卸载能力。把两个词各自压成一种用法，只是为了先看清主线。

## 都在用户态，绕过的却不是同一件事

**内核旁路**说的是常见数据面操作不必每次进入内核，不是内核被卸载或网卡自动理解业务。Linux 的 userspace verbs 文档把 RDMA 分得很明白：创建资源等慢路径仍经由内核；准备就绪后，用户态 provider 可以提交 WQE（工作队列项）、敲设备 doorbell，并轮询 CQ（完成队列）。典型硬件 RDMA 快路径不需要为每次操作做系统调用。

DPDK 的 Ethdev/PMD 同样让应用在用户态管理队列和包缓冲区，用轮询和批量收发替代逐包走内核网络协议栈。它给应用的是 `rte_mbuf` 中的包以及 Tx/Rx 队列操作；网卡依然负责 DMA、队列调度和可能启用的硬件 offload。初始化、设备权限、内存 DMA 映射等也不凭空消失。

真正拉开距离的是**远端 CPU 旁路**。一次硬件 RDMA Write 可以由接收端 RNIC 验证访问权限并直接把数据 DMA 到预先注册的内存，无需远端 CPU 为这次写入逐包解析请求。一个典型 DPDK UDP 服务收到的则是 Rx 队列中的包：网卡把包 DMA 到接收缓冲区，应用必须运行起来，解析消息、执行逻辑，才能决定怎样修改自己的业务数据。两者都可能是本机零拷贝网络 I/O；只有前一种场景把这次*远端数据放置*也交给了网卡。本端应用轮询 RDMA 的 CQ 仍会耗 CPU，“远端 CPU 旁路”不是“两端 CPU 都不动”。

```text
硬件 RDMA Write / RC
  发起方：WR(remote_addr, rkey) → 本端 RNIC
  网络：  RNIC 处理 RDMA 传输；RC 负责可靠交付
  接收方：远端 RNIC 校验权限 → DMA 写入指定 MR
  远端 CPU：不用逐次执行这次数据放置

DPDK + 普通 UDP 服务
  发起方：应用组织 mbuf → 网卡发包
  网络：  传递数据包；UDP 本身不提供可靠交付
  接收方：网卡 DMA 到 Rx mbuf → 应用轮询、解析
  远端 CPU：运行服务逻辑，决定数据放置及结果
```

| 问题 | 硬件 RDMA Write / RC | DPDK Ethdev 上的普通 UDP 服务 |
| --- | --- | --- |
| 应用提交什么 | 写远端地址的工作请求，附带访问用的 `rkey` | 包缓冲区和待发送/待处理的数据包 |
| 接收端网卡做什么 | 检查 MR 权限并将数据 DMA 到指定地址 | 将到达的数据包 DMA 到预置的 Rx 缓冲区 |
| 谁理解业务消息 | 写入本身由 RNIC 执行；后续业务处理仍由应用负责 | 接收端应用解析并处理消息 |
| 可靠性由谁提供 | RC 的传输机制由 RDMA 端点实现 | UDP 不保证可靠性；需要时由应用或上层协议实现 |
| 主要受限资源 | MR、QP、CQ、网卡上下文及其缓存，也有主机 CPU 成本 | mbuf/mempool、队列、CPU 轮询与软件协议处理，也有 NIC 成本 |

这个表是**两条特定数据路径**的对照，不是给全部 RDMA/DPDK 功能下定义。尤其不能把“RDMA”直接翻译成“远端 CPU 永远不参与”：RDMA Send/Receive 是双边操作，接收方要预投递缓冲区并处理到达的数据；即使用单边 Write，远端应用如果要响应新数据，也得有通知、轮询或其他同步协议。普通 Write 并不会自动把业务事件交给远端应用；在接收方预投递接收请求后，用 Write with Immediate 或 Send 才是常见的通知方式。

## 内存、可靠性：两笔容易漏算的账

DPDK 的 mbuf 通常来自 mempool，网卡可直接 DMA 到这些接收缓冲区。因此“DPDK 一定要多拷贝一次，RDMA 才零拷贝”是错的。反过来，网卡把包零拷贝送入 mbuf，**也不等于**远端 CPU 可以不看包就把数据写进业务对象。是否需要再拷贝，取决于应用怎样布局接收缓冲区；是否需要运行远端 CPU，取决于谁来解释请求。

RDMA 的远端内存访问先要建立权限和地址关系：应用注册 MR（Memory Region），设置本地/远端的访问权限，把可访问的远端地址和 `rkey` 通过控制面交给对端。RNIC 不是拿到一个任意指针就能改写整台机器的内存。内存注册、映射和地址/权限管理有成本；常规注册可能涉及页固定，支持 ODP 等机制的设备又有不同取舍。DPDK 的 DMA 缓冲区同样需要设备可访问，但“网卡能 DMA 某块本地缓冲区”和“对端可以用 `rkey` 指定远端 MR 的地址”不是一种权限模型。

再看丢包：RoCEv2 外面有 IP/UDP 头，却不是“DPDK 发个 UDP 包就是 RDMA”。标准 RNIC 要识别符合协议的 RDMA 报文，维护 QP 状态，按所选传输服务处理确认、重传和访问控制。DPDK 给你的普通 UDP 包不会凭空得到这些语义；应用若想做可靠 RPC，得自己建立消息、超时、重试、拥塞控制等机制。反过来，RDMA 的 UD 是 *Unreliable Datagram*，不能把 RC 的可靠性推广到所有 RDMA 操作。

RoCE 常和 PFC 一起出现，但“RDMA 协议必然要求交换机开启 PFC”也不严谨。IRN 的研究针对当时的 RoCE 网卡设计，展示了改进丢包处理后，不依赖 PFC 也能获得良好性能。这是对**方案可行性**的论证，不意味着今天随意关闭现有网卡的 PFC 都不会掉速；具体仍要看端点的拥塞与丢包恢复能力。

## 谁负责维护状态，谁就承担扩展成本

把可靠传输和远端内存操作交给 RNIC，能节约远端逐请求执行的软件开销，却会消耗网卡的 QP 上下文、缓存和内存映射资源。连接数多、访问分散时，NIC 上下文可能频繁未命中。eRPC 在 2019 年论文的 ConnectX-5 测试里，随机选择连接做小块 RDMA Read，约 5000 个连接时观察到相对峰值约 50% 的吞吐下降；他们转而用包 I/O 和主机 CPU 管理 RPC 状态。**这是那组网卡与工作负载的测量，不是 2026 年所有 RNIC 的连接数上限。**

DPDK 路线把更多状态和协议处理放在主机侧，换来可编排的报文处理和较灵活的软件状态，但远端服务每处理一个请求通常要消耗 CPU。代价不只是平均延迟，还包括忙轮询占用的核、突发负载下的尾延迟、丢包恢复，以及可观测性。硬件 RDMA 则不能只量稳定态的小包延迟：MR 注册、QP 建立、内存占用、故障恢复和访问模式也算在内。

FaSST 提醒我们，**即使选 RDMA，也不必总选单边操作**：它用 RDMA 的双边 UD 消息做 RPC，让远端 CPU 执行事务逻辑，换取更灵活的业务处理和扩展方式。eRPC 则用可靠的用户态 RPC 构建在普通 UDP 包 I/O 之上，论文中单核小 RPC 测试达到了最高约一千万次/秒。这些结果都属于特定实验，不能拿来证明“DPDK 比 RDMA 快”或相反；它们说明，**比较的单位应是实现相同业务语义的整个系统**，而不是两个不同 API 各跑一个峰值。

## 更有意思的是：它们还能叠在一起

如果说 RDMA 是一组远端内存与传输语义，DPDK 是收发包的用户态工具，那么用后者实现前者并不矛盾。2026 年 NSDI 的 BURST 就用 DPDK 做软件数据面，在普通以太网卡侧实现兼容 verbs 的软件 RDMA，并与硬件 RNIC 互通。论文在其 400G 测试场景下报告了 98.7% 的线速带宽；这证明了软件实现的潜力，**不等于普通 DPDK 应用自动拥有硬件 RDMA 的 CPU 卸载**。软件端仍得为 RDMA 协议和数据移动付费。

另一方向也成立：DPDK 的 mlx5 PMD 使用 Linux 内核驱动与 `rdma-core` 等组件，同一设备可以由内核和 DPDK 的 bifurcated driver 协作管理。eRPC 在 Mellanox 以太网卡上甚至通过 `libibverbs` 直接发送 UDP 包，而不经 DPDK 封装。**用了 verbs 库，不保证请求就是 RDMA Write；用了 DPDK，也不妨碍上层实现 RDMA 语义。** API 名称、底层驱动和线上协议，不能彼此替代。

因此选型时，我更愿意先问三件事：需要网卡直接读写远端已注册内存，还是需要远端运行代码解释请求？谁承担可靠性与拥塞控制？能否接受 MR/QP 管理成本，或是持续占用 CPU 核处理包？远端放置数据、远端业务处理、用户态包 I/O 是三个不同问题，没必要押一个“用户态更快”的口号。

真要测，至少让两边实现**相同业务操作**，固定网卡、网络和数据规模，同时看两端 CPU、MR/QP 建立时间、连接扇出、丢包与拥塞下的 p99。用 `ib_write_bw` 的单边写峰值去比 `testpmd` 的转发吞吐，测到的不是“RDMA vs DPDK”，而是两种完全不同的任务。

## 参考资料

- [Linux 内核：Userspace verbs access，RDMA 慢路径、快路径与内存固定](https://docs.kernel.org/infiniband/user_verbs.html)
- [rdma-core：ibv_post_send 手册，Send/Read/Write 工作请求和远端地址](https://man7.org/linux/man-pages/man3/ibv_post_send.3.html)
- [rdma-core：ibv_reg_mr 手册，MR 的本地与远端访问权限](https://man7.org/linux/man-pages/man3/ibv_reg_mr.3.html)
- [DPDK：Ethernet Device API，PMD 队列、mbuf 与轮询收发](https://doc.dpdk.org/guides/prog_guide/ethdev/ethdev.html)
- [DPDK：mlx5 Ethernet PMD，内核驱动、rdma-core 与 bifurcated driver](https://doc.dpdk.org/guides/nics/mlx5.html)
- [FaSST，OSDI 2016：双边 RDMA UD 事务 RPC](https://www.usenix.org/conference/osdi16/technical-sessions/presentation/kalia)
- [IRN，SIGCOMM 2018：Revisiting Network Support for RDMA](https://arxiv.org/abs/1806.08159)
- [eRPC，NSDI 2019：Datacenter RPCs can be General and Fast](https://www.usenix.org/conference/nsdi19/presentation/kalia)
- [BURST，NSDI 2026：Seeking High-performance, Interoperability and Scalability in Soft-RDMA](https://www.usenix.org/conference/nsdi26/presentation/shen)
