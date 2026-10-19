---
title: "OCP 2026 观察：AI 数据中心开始像一台巨型机器一样思考"
date: 2026-10-19T14:35:00+08:00
draft: false
tags: ["OCP 2026", "AI Infrastructure", "800VDC", "Liquid Cooling", "CPO", "CXL", "OpenBMC"]
categories: ["AI 基础设施"]
summary: "从 800VDC、液冷、CPO、CXL 内存池到 OpenBMC 和安全供应链，OCP 2026 的各条技术主线正在被 AI 数据中心重新拧成一根绳。"
---

上周 OCP 2026 在圣何塞举行了，这场开放计算社区的年度大会结束后留下了一个很明显的信号：AI 数据中心不再是一堆服务器加一堆机房设备，而是一台从电网、水管、光模块、HBM、固件一直连到 Token 输出的巨型机器。它有点像一个刚学会深呼吸的运动员：肺活量变成了供电，血管变成了液冷，神经变成了网络，记忆变成了内存和存储，免疫系统则是安全与可观测性。

今年官方录播页目前公开了约 559 条内容，分布在 35 个方向。这不是“厂商轮流讲新品”的展会，而是社区在给 AI 工厂补一张系统级施工图。下面按方向聊聊各自的最新进展。

![OCP 2026 AI 数据中心主线，把供电、冷却、网络、计算、内存存储、安全与运维放在同一张系统分层图中](/images/ocp2026/ocp-2026-ai-data-center.png)

## 一、AI Open Data Center：从“建得快”到“能被电网接受”

AI Open Data Center 方向最热闹的词已经不是 GPU，而是 **800 VDC、储能、负载波动和部署闭环**。

以前数据中心更像一个安静的大负载：电来了，服务器吃掉，冷了，结束。现在 AI 训练和推理像一群脾气不太稳定的运动员，负载会在瞬间大幅波动，还可能把这种波动传回电网。于是社区开始认真讨论 `Fault Ride-through`、负荷平滑、大负载接入、储能安全和 250 MW 级参考架构。

这里的转变很直接：AI 数据中心不仅要“能开机”，还要成为电网可以理解、可以协调、可以信任的负载。储能系统和 BBU/CBU 不再只是断电保护，而是参与削峰、抑制训练功率振荡、支撑直流母线稳定性的部件。

同样重要的是部署方法。数字孪生从展示 Demo 变成了交付流程：先用数字模型验证供电、冷却和拓扑，再进入生产准备度检查。模块化数据中心也重新被讨论，但议题从“快速拼装”转向高密度负载、湿度策略、热排放系统和液冷调试。

## 二、800VDC 与 Rack & Power：供电开始“贴着 GPU”重新组织

如果说 AI Open Data Center 讨论的是“机房外面的电”，AI Open DC: Power Distribution 和 Rack & Power 讨论的就是“机架里面的电”。

今年最醒目的主线是 **800 VDC / LVDC 生态化**。议题已经从“能不能做 800V”推进到：

- 2 线还是 3 线；
- 如何做短路故障分析和保护协调；
- 固态断路器与 DC Fault Manager 如何工作；
- 800V sidecar 如何做到 600 kW 级；
- 直接到服务器的 800V 方案和 DC/DC 拓扑怎么取舍；
- 高温超导电配电是否可靠、可维护；
- 甚至要不要把低压直流范围继续推到 1500V。

Rack & Power 方向则把机架当成一个电力系统来设计：Open Rack Wide 继续推进，1 MW+ IT 机架的开放式 busbar 接口被摆上桌面，电源柜级联和聚合、高密度 BBU/CBU、AI 训练功率振荡滤波、MGX Power Rack 参考设计都成为具体工程问题。

一句话概括：以前电源是服务器的配件，现在电源路径是 AI 机架架构的主干。

## 三、冷却：Cold Plate、Immersion 和 Cooling Environments 三线并进

![800VDC 与液冷协同架构：从电网、储能、直流配电到冷板、CDU、热回收的完整链路](/images/ocp2026/ocp-2026-800vdc-cooling.png)

OCP 2026 的冷却议程终于摆脱了“液冷好不好”的初级问题，开始回答“百万千瓦级怎么稳定运行”。

Cold Plate 方向在啃硬细节：单相冷板的极限、两相冷板标准化、500 W 级 HSIO 直冷、热界面材料、铜冷板腐蚀、流体速度与侵蚀、免化学药剂的水质管理、生物膜管理和从传感器到洞察的流体监测。这里最像医疗系统：冷板不是装上就好，而是要持续监测水质、流速、材料寿命和微生物风险。

Immersion 方向则在补生态：流体资格认证、系统合规、浸没前清洗化学、材料兼容性、企业 SSD 在两相浸没中的长期可靠性、酯类冷却液差异化和 AI NeoCloud 中的原生浸没部署。它没有变成主角，但正在从特种工艺变成有测试、有数据、有供应链的工程路线。

Cooling Environments 负责把芯片外的世界接起来：CDU 设计选择对 2.3 MW 系统性能的影响、Door Heat Exchanger、废热回收生态、液冷大规模运维工具、PG25 混合阈值、Flush/Fill 车、机架级减重和可靠性验证。另一个现实问题是：普通计算什么时候仍然用风冷更划算。液冷赢了 AI 高密度，但不是所有场景都需要它。

## 四、AI Clusters：从单 Pod 走向 5000+ GPU 和 Scale-Across

AI Clusters 是今年最能看出系统边界外移的方向。

Scale-up 域已经从单机架扩展到 **5000+ GPU 的多机架域**。Meta 的 Tahan 展示了面向 MTIA 的开放 Ethernet scale-up switch；ESUN 生态、铜互连域测试、SUE-T、MRC、UET、SRv6 和多平面 AI scale-out fabric 都在解决同一个问题：当集群大到一定规模，网络不再只是搬数据，而是决定系统可不可用、可不可运维。

Scale-across 也开始成型。它讨论的是跨数据中心、跨区域 AI 集群的路由、传输和设备影响。换言之，AI 系统的边界正在从“一个 Pod”变成“一个园区”，再变成“多个区域”。

基准测试同样在补课：AI Cluster Benchmarks 试图为 Superpod 建立标准化评估体系，AI Tokenomics 则把基础设施讨论从“多少 FLOPS”推向“多少 Token、多少成本、多少中断”。可靠性方面，减少 workload interruption、裸金属 GPU 的上下文故障管理、集群级遥测和开放运维层成为新的战场。

## 五、Networking 与 Photonics：铜继续冲刺，光开始接管结构

Networking 和 Photonics 是今年最密集的一组。

铜的故事是 **448G**：AEC、400G electrical、co-packaged copper、信号 conditioner、 cartridge 级验证和 500 W-class HSIO。铜没有退场，而是在功耗、成本和可制造性上继续逼近物理边界。

光的故事则是分层推进：LPO、NPO、CPO、OCI、外部激光源、400G/lane 连接器、开放硅光参考设计、12.8T VCSEL、相干光在 AI 时代的角色，以及从 51.2T CPO 验证到 Meta 102.4T 的路线。光互连不再只是替代铜的“更远距离方案”，而是在高密度、高带宽、低功耗和多平面网络里变成结构性选择。

SONiC Workshop 也没有停在交换机软件本身：ML pipeline 的即插即用、亚毫秒级遥测、拥塞信号、先进光模块遥测、P2P transparent clock 和 FIPS 140-3 支持都在把开放网络软件栈变厚。这里的趋势很清楚：网络数据面、管理面和遥测面要同时开放，否则 AI fabric 只能当黑盒。

## 六、Server 与 Composable Memory：CPU、XPU 和内存一起换脑子

Server 方向的关键词是 **模块化和平台契约**。M-PNP 继续向产业 adoption 推进；OCP NIC Gen 7 开始面对更高速度、更高功率和液冷；MHS、DC-SCM 3.0、OAC、SBSA、低电感互连、VRM 电热协同仿真和 AI signal conditioner 标准一起出现。服务器正在从整机黑盒拆成一组可组合、可替换、可验证的模块。

Composable Memory Systems 则直指 AI 推理的内存墙。KV Cache 分层、CMS Memory Fabric Manager、CXL 在超大规模环境中的故障与调试、GPU 到内存扩展的 P2P 数据路径、动态池化与共享、PNM offload、XPU 级 smart memory fabric，以及通过 compute interconnect 提供字节寻址持久内存，都是围绕“HBM 不够用时系统怎么办”。

这不是简单给服务器加几根内存条，而是把内存变成一层可以调度、可以池化、可以按 Token 工作集移动的基础设施。

## 七、Open Chiplet Economy：芯片内部也开始讲接口契约

Open Chiplet Economy 的进展比“Chiplet 概念热”更实际：OCE 2.0 Transaction/Link Layer 规范、多厂商互操作系统级标准、Bunch of Wires Flexi 1.0、轻量级串行互连、die-to-die 光 chiplet、FCSA 安全架构、联邦式 chiplet 设计与仿真，以及汽车和物理 AI 中的互操作障碍。

HBM 也不再只是容量颗粒，而被当作系统共同设计对象：base die、D2D 供电、封装热和 AI 数据流要一起算。Chiplet 的价值不是“把大芯片拆小”，而是让不同厂商的 die 能在同一套电气、安全、验证和交付契约下组合。

## 八、Storage：重新变成 LLM Serving 的性能层

存储议程明显更偏 AI。QLC 在超大规模 fleet 中的角色、near-line SSD、高容量 QLC、块大小变化对数据放置经济学的影响、OCP NVMe SSD 规范合规测试、NVMe 路线图、IO 优先级、SSD 遥测、Hammerspace 和 Xsight Labs 的开放闪存平台，以及 **Storage as a Performance Tier for LLM Serving**，都在回答同一个问题：KV Cache、模型权重、数据集和 checkpoint 应该放在哪里。

特别是 KV$ 这类面向 LLM 基础设施的工作，把存储从“容量和持久化”推向“延迟、带宽、放置和成本共同优化的性能层”。大模型服务现在必须在 HBM、CXL、QLC、NVMe 和分布式存储之间做实时取舍。

## 九、Security：供应链、固件和后量子密码一起上桌

Security 方向的进展是“身份链补完”。Hitless upgrade 的 attestation、Integrated HSM、供应链 endorsement 语言、SPDM 设备身份、TPM 支持 SPDM 和 PQC、OCP L.O.C.K. 与 FIPS、S.A.F.E. 和 S.O.L.I.D. 升级、Caliptra 开源 DV、裸金属安全架构、运行时授权的无缝升级、SoC key distribution、scale-up pod 安全、confidential storage、BMC 介入的 composite attestation、Caliptra 2.2，以及 PQC masking，都在把信任从单点芯片扩展到整条供应链和整个生命周期。

AI 数据中心的安全问题不是“加一个加密模块”能解决的。服务器要能证明自己是谁、固件是否可信、更新是否授权、供应链是否可信、scale-up 网络是否安全、存储是否保密。身份和完整性必须像液冷管线一样贯穿全栈。

## 十、Open Platform Firmware 与 Systems Management：固件和遥测变成运维地基

OPF 方向继续推进 openSFI、UPCI、flash-less streaming boot、控制面固件解耦硅依赖、内存安全、可审计固件和 agentic 生命周期自动化。Intel FSP 也在向 openSFI 和 UPCI 对齐。固件不再只是开机代码，而是供应链安全、系统配置和远程维护的入口。

Systems Management 则在处理规模问题：硬件故障管理工作流、多机架层级 Device Manager、GPU 平台系统级可管理性、遥测聚合管道、Auxiliary Management Controller、DMTF PMCI、Runtime SBOM 与 Redfish 的连接、GPU/CPU 诊断和 RAS 标准化、OpenBMC 的 GraphQL 指标 API、安全统一 debug 框架、OBMF 规范，以及 OpenBMC 超大规模运行中的“什么能用、什么会坏、什么要改”。

这里最值得注意的趋势是：管理面开始向 GPU 系统和集群级层级扩展。一个机架不再是一个可管理单元，而是一组有层级、有策略、有遥测管道的设备集合。

## 十一、Test & Validation 与 AI Computing Continuum：验证跟着 AI 学会“看上下文”

Test & Validation 不再只是跑标准用例。AI-native 硬件验证开始引入本地上下文 agentic test/debug 平台；Microsoft 讨论把平台验证流程从 spec 推向 sign-off；GPU 加速器合规通过 CTAM 连接超大规模用户和供应商；高速线缆 cartridge、L10-L12 制造测试、风扇可靠性和诊断标准化也在补产业级验证链条。

AI Computing Continuum 则把讨论从云扩展到边缘和行业：统一 AI fabric、Edge MHS 1.0、开放 Adapter Card Light、AI-RU、AI PC 到 AI 基础设施的平台契约，以及信任、编排和开放标准。它想解决的是“AI 能力跨云、边、端流动时，接口和管理方式怎么统一”。

## 十二、Sustainability：从披露数字走向运营和供应链决策

Sustainability 方向并没有被 AI 热潮挤到角落，反而变得更工程化：水效率最佳实践、区域冷却架构权衡、机架级 embodied carbon、DCEM 指标案例、AI 优化可持续性看板、制造侧实时遥测和自动碳核算、废热回收、运营碳追踪、PCR/LCA、供应链水管理、结构钢再利用、电力效率设计，以及更严格的碳披露。

这里的重点是“可核查”。以前可持续性报告容易停在披露层面，现在开始要求供应商指标和运营方需求对齐，碳、水、废热、结构材料、电源效率都进入设计决策。

## 收尾：OCP 的中心议题从部件变成了契约

看完这些方向，最明显的共同点不是某个具体产品，而是 **系统契约**：电网要理解 AI 负载，直流配电要知道怎么保护，冷却水要知道化学状态，网络要知道亚毫秒级状态，内存要知道 KV Cache 位置，固件要知道自己是否可信，存储要知道自己服务的 Token 层。

OCP 2026 的角色也更清楚了：它不再只是把服务器规格开放出来，而是在给 AI Factory 造一套共同语言。开放的价值也从“避免锁定”变成了“让几十个层级的供应商能同时往前跑”。

AI 数据中心这台巨型机器还会继续变大，但今年真正的进展是：它开始有一套可讨论的神经、血管和免疫系统了。

## 参考资料

1. [2026 OCP Global Summit Recorded Content](https://www.opencompute.org/events/ocp-summit/2026-ocp-global-summit/recorded-content/)
2. [Explore 22 Tracks at the 2026 OCP Global Summit](https://www.opencompute.org/index.php/blog/explore-22-tracks-at-the-2026-ocp-global-summit)
3. [2026 OCP Global Summit](https://www.opencompute.org/summit/global-summit)
