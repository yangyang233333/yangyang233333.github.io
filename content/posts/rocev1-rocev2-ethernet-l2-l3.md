---
title: "RoCEv1 与 RoCEv2：从以太网二层交换到三层路由"
date: 2026-09-29T14:59:00+08:00
draft: false
tags: ["RoCE", "RDMA", "以太网", "交换与路由"]
categories: ["网络", "技术原理"]
summary: "沿一次跨网段传输，解释 MAC 与 IP 各自负责什么、路由器如何改写以太网帧，以及 RoCEv2 为什么用 IP/UDP 替换 RoCEv1 的 GRH 封装。"
---

**RoCEv1 与 RoCEv2 最重要的区别，不是“一代慢、二代快”，而是原生报文能否通过普通 IP 路由器转发。** RoCEv1 直接承载于以太网二层；RoCEv2 使用 IP/UDP 封装，因此可以跨 IP 子网通信。两者承载的仍是 RDMA 传输协议。

理解这个变化，需要先澄清一个说法：严格来说，以太网主要提供二层帧传输；通常所说的“三层网络”，指运行在以太网等链路之上的 IP 网络。二层和三层不是两种互斥的网线，而是一次通信中承担不同职责的协议层。

## 1. 二层看 MAC，三层看 IP

| 对比项 | 二层交换 | 三层路由 |
| --- | --- | --- |
| 处理对象 | 以太网帧 | IP 包 |
| 主要转发依据 | 目的 MAC，以及所属 VLAN 等上下文 | 目的 IP，通常按最长前缀匹配选择路由 |
| 主要转发表 | MAC 地址表：哪个 MAC 在哪个端口之后 | 路由／转发表：到某个前缀走哪个下一跳和接口 |
| 通信范围 | 同一个二层广播域内 | 不同 IP 子网之间 |
| 典型设备角色 | 二层交换机、网桥 | 路由器、三层交换机 |

典型二层交换机会从收到的帧中学习**源 MAC 所在的端口**，再根据目的 MAC 决定向哪里转发。目的 MAC 尚未学到时，未知单播通常会在所属 VLAN 内泛洪，而不是立即进行 IP 路由查询。

VLAN 划分的是二层广播域，IP 子网划分的是三层地址范围。工程上经常让一个 VLAN 对应一个 IP 子网，但它们不是同一个概念。同一个二层域也可以横跨多台交换机，并不等于“接在同一台交换机上”。

“三层交换机”同样不是放弃二层：同 VLAN 的流量可以执行二层交换，跨子网的流量可以执行三层转发。二层、三层说明的是转发语义，不意味着三层流量必然由 CPU 逐包处理。

## 2. 跨网段时，为什么既需要 IP 又需要 MAC？

下面以 IPv4 为例，假设主机 A 和 B 属于不同 IP 子网，中间经过一个路由器。不考虑 NAT、隧道和策略路由；图中的地址均为符号。

```
  IP subnet 1 / L2 domain 1        IP subnet 2 / L2 domain 2

Host A ---- Switch 1 ---- [ Router R ] ---- Switch 2 ---- Host B
 MAC_A                    MAC_R1 MAC_R2                   MAC_B
 IP_A                                                      IP_B
```

一次从 A 发往 B 的传输，可以拆成四步：

1. **A 选择下一跳。** 根据路由表，A 确定 B 不在直连子网内，于是选择 R 作为下一跳。A 需要通过 ARP 解析的是 R 在本地链路上的 MAC，而不是跨网段广播查询 B 的 MAC。ARP 结果也可能已经在邻居缓存里。
2. **A 发出第一段以太网帧。** 以太网目的地址填写 `MAC_R1`，但内部 IP 包的目的地址仍然是 `IP_B`。Switch 1 依据 MAC 地址表把帧转发给 R。
3. **R 执行三层转发。** R 根据目的 `IP_B` 查询路由，选择右侧出口，将 IPv4 TTL 减一，并相应更新 IPv4 头校验和。在这个例子里，B 是出口侧的直连主机，因此 R 解析或查询 B 的 MAC。
4. **R 发出新的以太网帧。** 新帧的源 MAC 是 `MAC_R2`，目的 MAC 是 `MAC_B`，由 Switch 2 转发给 B。没有 NAT 时，IP 源、目的地址仍然是 `IP_A` 和 `IP_B`。

把两段链路上的关键字段并排看，变化就很明确：

```
A -> R: Ethernet(MAC_A  -> MAC_R1) | IPv4(IP_A -> IP_B)
R -> B: Ethernet(MAC_R2 -> MAC_B ) | IPv4(IP_A -> IP_B)
```

**IP 地址描述端到端的通信对象，MAC 地址负责当前二层链路上的交付。** 跨越路由边界时，需要重新封装适合下一段链路的二层帧；并不是把 IP 目的地址一路改成各个网关的地址。单纯经过二层交换机，则通常不会发生这样的源、目的 MAC 改写。

## 3. RoCEv1 和 RoCEv2 的报文到底差在哪里？

RDMA 规定远程内存读写、消息传递等通信语义；RoCE 则规定如何把相应的 InfiniBand 传输协议承载在以太网上。接收端 RNIC 根据 RDMA 传输头定位 QP，并根据操作类型和内存权限处理数据；交换机不需要替应用解释远程内存地址。

下面省略可选 VLAN 标记以及随 RDMA 操作变化的扩展头。GRH 是 Global Routing Header，BTH 是 Base Transport Header；iCRC 和 FCS 分别属于 RDMA 报文和以太网帧的校验机制。

```
RoCEv1
  Ethernet (EtherType = 0x8915)
    -> GRH
    -> IB transport headers (BTH, ...)
    -> payload -> iCRC -> Ethernet FCS

RoCEv2
  Ethernet (EtherType = 0x0800 for IPv4, 0x86DD for IPv6)
    -> IPv4 / IPv6
    -> UDP (destination port = 4791)
    -> IB transport headers (BTH, ...)
    -> payload -> iCRC -> Ethernet FCS
```

### RoCEv1：以太网认识它，普通 IP 路由器不会原生路由它

RoCEv1 的 EtherType 是 `0x8915`。二层交换机不必理解 RDMA，只需按 MAC 和 VLAN 等信息转发帧，因此 RoCEv1 可以经过多台交换机。

容易误解的是中间的 **GRH**：名字里虽然有“Routing”，也使用 IPv6 风格的地址字段，但整个帧并没有因此变成 EtherType 为 IPv6 的标准 IP 报文。不能指望普通 IP 路由器把它当作 IPv6 包直接转发。

所以，RoCEv1 原生依赖二层可达性。可以通过 VXLAN 等机制承载和延伸二层网络，但那是额外的外层封装，不是 RoCEv1 自己获得了原生 IP 路由能力。

### RoCEv2：用标准 IP 头穿过路由网络

RoCEv2 用 **IP 头和 UDP 头替换原先的 GRH 封装**，不是在完整的 RoCEv1 帧前简单再叠一层 UDP。

回到前面的跨网段例子：R 能识别标准 IP 包，因此按目的 IP 查询路由、更新 TTL 或 Hop Limit，并为下一段链路重新封装以太网帧。内部 RDMA 请求仍交给目标 RNIC 处理，路由器不需要理解 QP 或远程内存操作。

UDP 目的端口 `4791` 用于标识 RoCEv2。UDP 源端口可以提供流标识，供网络设备的 ECMP 哈希使用。这样，不同流可以分散到不同等价路径，而不要求交换机专门解析内部 RDMA 传输头。

这让不同机架可以采用独立 IP 子网，再通过三层网络互通，而不必仅为原生 RoCE 通信，把所有节点都放进同一个二层广播域。

## 4. 能路由，不等于可靠性和拥塞问题自动解决

首先，**UDP 封装不等于普通 UDP socket 通信**。常见硬件 RoCE 数据路径由 RNIC 生成和消费报文，不需要把数据先送进内核 UDP socket 再交给应用。

其次，RoCE 的封装版本与 RDMA 的传输服务类型是两个维度。使用常见的 RC（Reliable Connection）模式时，可靠传输由 RDMA 传输机制及 RNIC 承担，而不是由 UDP 提供。不能把 v1 到 v2 理解成“从不可靠升级成可靠”。

最后，大规模并发还会造成交换机排队和丢包。部署中常见的两个工具，恰好也工作在不同层次：

- **PFC 是二层、逐跳、按优先级生效的流控。** 某条链路接近缓冲区上限时，可以暂停上游相应优先级的流量。它不是从接收应用直接发给发送应用的端到端限速指令，也不是 RoCEv1 独有的功能。
- **RoCEv2 可以利用 IP 层的 ECN 标记反馈拥塞。** 接收端看到拥塞标记后发送 CNP，发送端可通过 DCQCN 等算法调整速率，尽量减少持续拥塞和对暂停机制的依赖。

PFC 并非 RoCE 能够运行的绝对前提：也存在有损和半无损的部署方式。应该结合网卡能力、拥塞控制、负载和性能目标决定网络方案，而不是仅凭“用了 RoCEv2”就认为必须零丢包，或者认为丢包已经无关紧要。

## 小结

二层解决当前广播域里的帧转发，三层解决跨子网的 IP 转发。**RoCEv2 的关键，是让 RDMA 报文获得原生 IP 可路由性；它没有取消以太网二层，也没有把 RDMA 内存语义交给路由器。**

## 参考资料

- [Cisco：Unicast Flooding in Switched Campus Networks，MAC 学习、未知单播与 VLAN 转发](https://www.cisco.com/c/en/us/support/docs/switches/catalyst-6000-series-switches/23563-143.html)
- [RFC 826：An Ethernet Address Resolution Protocol，ARP 地址解析](https://www.rfc-editor.org/rfc/rfc826.html)
- [RFC 1812：Requirements for IP Version 4 Routers，下一跳选择、转发和 TTL 处理](https://www.rfc-editor.org/rfc/rfc1812.html)
- [NVIDIA：RDMA over Converged Ethernet，RoCEv1/v2 封装、GRH 替换与 UDP 端口](https://networking-docs.nvidia.com/mlnxofedswum/24.10-5.1.6.1lts/rdma-over-converged-ethernet-roce)
- [NVIDIA：RDMA Aware Networks Programming Guide，RDMA 资源与传输服务](https://networking-docs.nvidia.com/doca/archive/3-5-0/rdma-aware-networks-programming-guide)
- [RFC 7348：Virtual eXtensible Local Area Network，在 IP 网络上承载二层网络](https://www.rfc-editor.org/rfc/rfc7348.html)
- [NVIDIA Onyx：RDMA over Converged Ethernet，ECN、拥塞控制及不同丢包策略](https://networking-docs.nvidia.com/onyxum/3.10.4800/rdma-over-converged-ethernet-roce)
