---
title: "SR-IOV RDMA 虚拟化原理：从 VF 直通到内存隔离"
date: 2026-09-28T10:28:00+08:00
draft: false
tags: ["SR-IOV", "RDMA", "虚拟化", "IOMMU", "RoCE"]
categories: ["网络", "技术原理"]
summary: "从 PF/VF 的职责分工出发，沿一次 RDMA Write 解释虚拟机如何直接使用网卡，以及 MR、地址转换和 IOMMU 如何共同建立隔离边界。"
---

SR-IOV RDMA 虚拟化的核心，不是给每台虚拟机模拟一块 RDMA 网卡，而是让支持 RDMA 的物理网卡暴露多个可独立分配的 PCIe 功能。虚拟机通过这些硬件入口直接使用 RDMA，宿主机负责资源分配和隔离，不再用软件逐条中转数据面请求。

真正需要讲清楚的是：**网卡怎样找到虚拟机的内存，以及“直接访问”为什么不等于“可以访问任意内存”。** 下文以 Linux KVM/VFIO、支持 RDMA 的 VF 和普通主机内存为例，不讨论 GPU 显存直通。

## 1. SR-IOV 切分的是设备入口，不是物理网卡

SR-IOV（Single Root I/O Virtualization）是 PCIe 的一种能力，定义了两类功能：

- **PF（Physical Function）**：具有完整配置能力的物理功能。宿主机通过 PF 驱动启用 VF，并管理设备级资源和策略；PF 也可以承载自己的业务流量。
- **VF（Virtual Function）**：轻量级 PCIe 功能，具有独立的 PCIe 身份、配置空间和设备寄存器入口，可以分配给不同虚拟机。

VF 不是 QEMU 用软件模拟出来的网卡，而是物理设备实现的硬件接口。客户机仍然需要与它匹配的驱动，只是管理权限通常比 PF 小。

```text
VM 1                              VM 2
App / libibverbs                   App / libibverbs
       |                                 |
       | queues + doorbells              | queues + doorbells
       v                                 v
+----------------------------------------------------+
|                    RDMA NIC                        |
|        VF 1 context          VF 2 context           |
|          Shared RDMA engines / PCIe / port          |
+-------------------------+--------------------------+
                          |
                       Network

Host PF driver -- configuration / policy --> NIC
Host VFIO      -- device assignment ------> VMs
```

图中的 VF 上下文在逻辑上分开，但底层端口、PCIe 带宽、RDMA 执行引擎和部分缓存仍然共享。创建多个 VF，不会凭空增加多份物理带宽，也不保证每个 VF 自动获得等额资源。

还要区分两个正交概念：**SR-IOV 解决设备如何分配，RDMA 解决数据如何传输。** PCIe SR-IOV 规范本身不保证 VF 具备 RDMA 能力。只有网卡、固件和驱动向 VF 暴露了相应功能，客户机才能使用硬件 RDMA；普通以太网 VF 并不会因此自动变成 RDMA 网卡。

## 2. 控制面仍然存在，数据面不再逐请求中转

一条可用的 VF RDMA 链路，通常经历这些准备步骤：

1. 宿主机通过 PF 驱动启用 VF，并配置资源、网络身份和相关策略。
2. 虚拟化管理层通过 VFIO 等机制把 VF 分配给客户机，建立设备访问、中断和 IOMMU DMA 映射。
3. 客户机加载网卡驱动和 RDMA 驱动，用户态安装 `libibverbs` 及匹配的硬件 provider。
4. 应用通过 verbs 创建通信资源、注册内存并建立连接。这些资源管理操作仍然需要客户机内核和设备驱动参与。

理解后面的数据路径，只需要认识四种 RDMA 对象：

- **QP（Queue Pair）**：队列对。常见 RC QP 包含发送队列和接收队列；应用向队列提交工作请求。
- **CQ（Completion Queue）**：完成队列，用于获取需要报告的工作完成及状态。
- **PD（Protection Domain）**：RDMA 保护域，把允许相互使用的 QP、MR 等对象关联起来。
- **MR（Memory Region）**：注册内存区域，记录地址范围、访问权限和地址转换信息，并提供本地访问使用的 `lkey`、远端访问使用的 `rkey`。

这些对象属于相应 VF 和应用上下文，但不意味着它们的所有数据都存放在网卡上。例如，队列缓冲区可以位于客户机内存，设备保存或缓存执行所需的上下文。

准备完成后，用户态 provider 可以把工作请求编码成 WQE（Work Queue Element），写入队列，再通过映射的设备寄存器等方式通知网卡，这通常被称为“敲 doorbell”。网卡处理请求并更新 CQ，应用可以直接轮询完成。

因此，常见硬件 verbs 快速路径既不需要宿主机软件逐条转发 WQE，也不需要客户机为每次发送、轮询都陷入内核。但“内核旁路”不代表内核消失：初始化、内存管理、事件处理和资源回收依然存在，提交请求和轮询也仍然消耗 CPU。

## 3. 网卡怎样找到虚拟机的内存

虚拟机里的应用拿到的是客户机虚拟地址 GVA；客户机操作系统看到的是客户机物理地址 GPA；真正的内存位于宿主机物理地址 HPA。设备发起 PCIe DMA 时使用的地址，则通常称为 IOVA。

先采用一个常见的简化模型：**客户机没有启用虚拟 IOMMU，普通 MR 的设备 DMA 地址按 GPA 建模，宿主机 IOMMU 再将其转换为 HPA。** 不把 ODP、ATS 或嵌套 IOMMU 等扩展混入这条基本链路。

```text
CPU load / store:
GVA -- guest page table --> GPA -- EPT / NPT --> HPA

RNIC DMA:
MR virtual address + key
          |
          v
RNIC: MR permission check + address translation
          |
          v
PCIe DMA address: IOVA (= GPA in this model)
          |
          v
Host IOMMU: VF DMA domain + page table
          |
          v
         HPA
```

RNIC 指具备 RDMA 能力的网卡。这里有两套不能混淆的转换：

**第一套发生在 RNIC 的 MR 地址空间内。** 普通 `ibv_reg_mr()` 注册路径中，客户机内核固定用户缓冲区对应的页面，驱动建立设备可用的页映射和权限信息。网卡拿到某个 MR 内的虚拟地址后，根据注册时建立的信息，得到需要发起 DMA 的地址，而不是直接把应用指针当成宿主机物理地址。

**第二套发生在宿主机 IOMMU 中。** 设备发起 DMA 时，平台根据 VF 的 PCIe 请求身份及其所属隔离域，使用宿主机控制的映射把 IOVA 转换为 HPA，并检查访问权限。客户机不能仅凭在描述符中填写一个地址，就绕过这层边界。

宿主机的 DMA 映射通常以分配给虚拟机的内存为基础，并不等于为每个应用 MR 都建立一个独立的 IOMMU 域。MR 负责更细的 RDMA 访问约束；IOMMU 负责把设备限制在被授权的宿主机内存范围内。

这也解释了一个常见误区：**CPU 的 EPT/NPT 不会自动替设备 DMA 做地址转换。** 设备直通需要单独管理 IOMMU 映射。启用客户机虚拟 IOMMU 后，IOVA 也不必等于 GPA，具体转换可能通过影子映射或嵌套转换实现，不能把图中的等式推广到所有配置。

## 4. 沿一次 RDMA Write 看完整链路

假设 VM A 和 VM B 分别位于两台宿主机，双方已经建立可靠连接 RC QP。下面只看普通、非 inline 的 RDMA Write，不讨论立即数通知等变体。

**第一步：准备可访问的内存。** A 注册发送缓冲区，得到本地 `lkey`。B 注册目标 MR，开放远端写权限，并通过应用协议把目标地址和 `rkey` 交给 A。这是授权与连接准备，不是让 A 获取 B 的宿主机物理地址。

**第二步：A 提交工作请求。** 请求包含本地地址、长度、`lkey`，以及目标地址、`rkey`。用户态 provider 把它写入发送队列，通知 A 所使用的 VF。

**第三步：A 的网卡读取数据并发送。** 网卡根据 QP、PD、MR 等上下文检查本地访问，经 MR 转换和宿主机 IOMMU 映射 DMA 读取 A 的缓冲区，再通过 InfiniBand 或 RoCE 等链路发送 RDMA 报文。

**第四步：B 的网卡校验并落入内存。** B 的网卡根据连接和目标 QP 找到接收上下文，检查 `rkey`、PD 关联、地址范围及远端写权限，再把目标地址转换成 DMA 地址，通过 B 宿主机的 IOMMU 写入目标页面。报文中的目标地址有意义，是因为 B 事先建立了 MR 映射，而不是因为两台机器共享物理地址空间。

**第五步：A 获取工作完成。** 如果这次请求要求产生完成通知，A 从 CQ 中取得对应的 CQE（Completion Queue Entry）。完成语义由 verbs 和所用传输类型定义，不等于“B 的业务代码已经消费了数据”。普通 RDMA Write 也不会自动在 B 侧生成一个接收 CQE，应用仍需设计自己的通知与同步协议。

数据搬运可以由两端网卡执行，B 的 CPU 不必为每个 Write 运行一段接收拷贝代码；但 B 的内存注册、权限配置和后续业务处理仍然需要软件参与。所谓“远端 CPU 不参与”，指的是这段特定的数据搬运路径，而不是整个应用生命周期。

## 5. 三层隔离各自解决什么问题

“每个租户一个 VF”不是完整的安全论证。至少需要区分以下三个层次：

| 层次 | 约束的对象 | 不能替代什么 |
| --- | --- | --- |
| VF 与设备资源归属 | 哪个功能、上下文可以创建或使用哪些设备资源 | 不替代宿主机的 DMA 地址隔离 |
| RDMA 的 PD、MR 和访问权限 | 某个 QP 能否使用某个 MR，访问是否越界、是否允许远端读写 | 不替代网络加密和对端身份认证 |
| 宿主机 IOMMU | 某个设备隔离域能够 DMA 到哪些宿主机页面 | 不理解某个页面中的业务字段和应用级授权 |

PD 与 IOMMU domain 是不同层次的“域”，不能混为一谈。尤其当客户机内核本身不可信时，不能仅依赖客户机完成内存注册这件事；宿主机仍然需要掌握最终 DMA 映射的控制权。

设备能否安全独立分配，还取决于 PCIe 拓扑、ACS 等条件和实际 IOMMU group。**不同 PCIe 地址不必然代表可以安全分给互不信任的租户。** 不能为了让直通配置成功，就把绕开 DMA 隔离的设置当作生产隔离方案。

网络侧同样需要独立设计。`rkey` 是 RDMA 访问检查的一部分，不是密码学意义的加密或身份认证；VLAN、InfiniBand 分区、访问策略等也不会因为创建 VF 就自动配置好。宿主机普通软件交换机或防火墙未必处在 VF 的 RDMA 快速路径上，策略必须部署在实际经过的数据路径中。

## 6. 性能收益与工程边界

性能收益主要来自减少宿主机数据面中转、软件调度和额外缓冲处理，而不是“虚拟化开销从此为零”。IOMMU 转换缓存、NUMA 布局、PCIe 竞争、网卡上下文缓存、VF 之间的带宽争用，以及应用的 doorbell 和 CQ 轮询方式，仍会影响结果。接近裸机是需要测量的结果，不是 SR-IOV 的性能保证。

在网络配置上，还要区分两种常见场景：

- **RoCE**：VF 的以太网身份、全局标识 GID、IP/VLAN、MTU 和网络拥塞管理需要正确配合。尤其 RoCEv2 仍依赖底层 IP 网络可达，直通不能修复网络配置错误。
- **InfiniBand**：需要考虑虚拟端口的 GUID、分区键 P_Key、端口状态，以及 Subnet Manager 的虚拟化支持和配置，不能直接照搬以太网 VF 的配置思路。

可以先在客户机做两个只读检查：

```bash
rdma link show && ibv_devinfo
```

它们帮助确认 RDMA 设备及端口是否被正确暴露，但不能单独证明端到端 RDMA 已可通信，更不能证明隔离或性能达标。`lspci` 看见一个 VF、普通 IP 流量可通，也都不等于硬件 verbs 链路已经可用。

另一个边界是**热迁移**。除了 CPU 和内存，迁移还要处理 VF 的设备状态、活跃 RDMA 对象，以及设备 DMA 写内存带来的脏页。已有支持 VF 热迁移的硬件和软件方案，但必须核对具体网卡、固件、驱动、虚拟机管理器及 RDMA 工作负载的支持条件；既不能笼统说“SR-IOV 永远不能热迁移”，也不能从“支持 SR-IOV”推导出“活跃 RDMA 连接可以无缝迁移”。

## 小结

把这套机制串起来，就是：**SR-IOV 提供可分配的硬件入口，RDMA 提供队列化的数据访问，MR 建立访问范围和地址映射，IOMMU 守住宿主机内存边界。** 快速路径可以绕开软件中转，但资源管理、权限检查和共享硬件的约束并没有消失。

## 参考资料

- [Linux 内核：PCI Express I/O Virtualization Howto，PF/VF 与 VF 启用机制](https://docs.kernel.org/PCI/pci-iov-howto.html)
- [Linux 内核：Userspace verbs access，慢速资源管理路径、快速路径与内存固定](https://docs.kernel.org/infiniband/user_verbs.html)
- [Linux 内核：VFIO，DMA 隔离、IOMMU group 与设备分配](https://docs.kernel.org/driver-api/vfio.html)
- [rdma-core：ibv_reg_mr 手册，MR 地址空间与本地、远端访问权限](https://github.com/linux-rdma/rdma-core/blob/bcd725bec72794527086f9490094ec8691f17ce8/libibverbs/man/ibv_reg_mr.3)
- [rdma-core：ibv_post_send 手册，工作请求、signaled completion 与 inline 语义](https://github.com/linux-rdma/rdma-core/blob/bcd725bec72794527086f9490094ec8691f17ce8/libibverbs/man/ibv_post_send.3)
- [NVIDIA DOCA 3.4：SR-IOV，VF 配置与 InfiniBand 虚拟化](https://networking-docs.nvidia.com/doca/archive/3-4-0/sr-iov)
- [NVIDIA DOCA 3.5：RDMA over Converged Ethernet，GID、网络配置与硬件卸载](https://networking-docs.nvidia.com/doca/archive/3-5-0/rdma-over-converged-ethernet)
- [NVIDIA DOCA 3.5：SR-IOV Live Migration，设备迁移的适用范围与前置条件](https://networking-docs.nvidia.com/doca/archive/3-5-0/sr-iov-live-migration)
