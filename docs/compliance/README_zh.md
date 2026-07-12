# AegisOps 合规对标

> **Disclaimer / 免责声明：** 本文档将 AegisGate / AegisOps 的能力映射到法规框架，仅用于
> **规划与就绪度评估**。本文档不构成法律意见，也不声称已通过任何合规认证。需要正式合规背书的客户
> 应聘请合格审计师（如针对 ISO 42001、SOC 2 Type II，或欧盟 2024/1689 下的 AI Act 一致性评估）。
> 下文的"就绪度"分级是**对公开法规文本的自评**，并非第三方背书。

**状态：** 合规对标骨架（阶段 1，TASK-20260520-01）。详细条款映射、控制证据收集、审计就绪报告
是阶段 2（TASK-W2）的交付物。

**最后更新：** 2026-05-20
**姊妹文档：** [愿景](../positioning/aegisops-vision_zh.md) · [路线图 v4](../ROADMAP_zh.md) · [English](README.md)

---

## 1. 概述

### 1.1 首批覆盖框架

本阶段 1 骨架覆盖当前与 AI 系统最相关的三个主要监管市场：

| 框架 | 司法管辖 | 状态 | 主要文本 |
|---|---|---|---|
| **欧盟 AI Act**（Regulation 2024/1689）| European Union | 2024-08-01 生效；高风险义务自 2026-08-02 起 | [eur-lex.europa.eu/eli/reg/2024/1689](https://eur-lex.europa.eu/eli/reg/2024/1689) |
| **ISO/IEC 42001:2023** AI 管理体系 | 国际标准 | 2023-12 发布 | [iso.org/standard/81230.html](https://www.iso.org/standard/81230.html) |
| **生成式人工智能服务管理暂行办法** | 中华人民共和国 | 2023-08-15 施行 | [cac.gov.cn/2023-07/13/c_1690898327029107.htm](http://www.cac.gov.cn/2023-07/13/c_1690898327029107.htm) |

NIST AI RMF 1.0、SOC 2 Type II、HIPAA、GDPR 第 22 条、以及行业法规
（PBOC、PCI-DSS）规划在阶段 2 扩展。详见 §5。

### 1.2 措辞规范：readiness 而非 compliant

本文档使用精确措辞以避免误导：

| 避免的措辞 | 原因 | 推荐措辞 |
|---|---|---|
| ~~"compliant / 合规"~~ | 暗示已通过第三方审计 | "对齐 / aligns with"、"支持 ... 要求 / supports requirements for" |
| ~~"certified / 已认证"~~ | 保留给第三方认证机构 | "就绪度评估可用 / readiness assessment available"、"证据收集到位" |
| ~~"production-grade / 生产级"~~ | 主观；无统一标尺 | "生产就绪架构，客户验证中" |
| ~~"满足全部要求"~~ | 少有系统真能做到 | "针对以下要求提供支持：…"（明确列出）|

下方"就绪度"列使用四级评分：

| 等级 | 含义 |
|---|---|
| 🟢 **直接映射** | AegisGate 组件直接满足该要求；证据收集自动化 |
| 🟡 **部分映射** | 组件满足部分要求；剩余由客户配置补齐 |
| 🔵 **架构就绪** | 架构原语存在；具体实施需要每客户工程化（阶段 2）|
| ⚪ **不在 AegisGate 核心范围** | 客户 / 运营方责任；AegisGate 提供证据通道但不提供控制本身 |

### 1.3 阶段 1 之后的路线

阶段 2（TASK-W2 / 金融合规包）将扩展此骨架：

- 详细条款映射（每个相关欧盟 AI Act 条款 / ISO 42001 第 4-10 章每个 clause / 国内办法每条）
- 每条款的证据收集 runbook（哪些 `metrics`、`audit_logs`、`config_versions` 记录满足每个 clause）
- 审计师就绪报告模板（PDF/HTML 生成挂钩）
- 行业专项叠加（金融用 PBOC AI 指引、医疗用 HIPAA Security Rule）

阶段 3（TASK-W3 / SaaS 控制平面）将增加：

- 多司法管辖租户路由（数据驻留强制）
- 持续合规监控仪表盘（实时每 clause 状态）

---

## 2. 欧盟 AI Act 就绪度映射（骨架）

> **骨架范围：** AegisOps 作为 AI 系统组件（AI 网关）的**提供方**，同时为下游客户 AI 系统的
> **部署方**提供支持层。下方映射聚焦于 AegisGate 原语最直接满足的义务。基于 AegisGate
> 构建的客户系统对于高风险用例仍需自行做 conformity assessment。

### 2.1 Title III 第 2 章 — 高风险 AI 系统要求

| 条款 | 要求（摘要）| AegisGate 组件 | 就绪度 |
|---|---|---|---|
| 第 8 条 | 符合本章要求 | 下列所有控制 | 🔵 架构就绪 |
| 第 9 条 | 风险管理体系 | `MetricsRegistry`、`FeedbackBus`、`RolloutMetricsProvider`（持续监控）| 🟡 部分 — 需每客户风险登记册 |
| 第 10 条 | 数据与数据治理 | `PIIFilter`、`AuditLogger`、`CacheMigrator`（数据 lineage）| 🟡 部分 — 训练数据治理客户自有 |
| 第 11 条 | 技术文档 | `docs/specs/`、`docs/guides/`、`docs/compliance/`（本文档）| 🔵 架构就绪 |
| 第 12 条 | 记录保留（日志记录）| `AuditLogger` 含 `chain_hash`、`PersistentStore` 保留 | 🟢 直接映射 |
| 第 13 条 | 透明度与对部署方的信息提供 | `docs/guides/`、OpenAPI 规范、Provider Manifest | 🟢 直接映射 |
| 第 14 条 | 人类监督 | `ConfigService` 双人审批（W3）、`RolloutController` 手动门 | 🟢 直接映射 |
| 第 15 条 | 准确性、稳健性与网络安全 | `InjectionDetector`、`ExternalSafetyStage`、`RolloutController` 自动回滚、限流 | 🟡 部分 — 模型级准确性客户自有 |

### 2.2 Title IV — 透明度义务

| 条款 | 要求（摘要）| AegisGate 组件 | 就绪度 |
|---|---|---|---|
| 第 50(1) 条 | AI 系统交互披露 | 客户在应用层实现 | ⚪ 网关范围外 |
| 第 50(2) 条 | 合成内容标识（AI 生成）| 客户实现；AegisGate 审计捕获生成事件 | ⚪ 网关范围外 |
| 第 50(4) 条 | 情感识别 / 生物特征分类提示 | 客户实现 | ⚪ 网关范围外 |

### 2.3 Title VIII — 上市后监测、信息共享、市场监督

| 条款 | 要求（摘要）| AegisGate 组件 | 就绪度 |
|---|---|---|---|
| 第 72 条 | 上市后监测系统 | `FeedbackBus`、`MetricsFeedbackSubscriber`、`RolloutMetricsProvider` | 🟢 直接映射 |
| 第 73 条 | 严重事件报告 | `AuditLogger`、告警通道（阶段 2 报告）| 🔵 架构就绪 |

> 详细 clause 映射（每个相关第 8-15 条 sub-clause）属阶段 2 范围。

---

## 3. ISO/IEC 42001:2023 就绪度映射（骨架）

### 3.1 第 4 章 — 组织的环境

| Clause | 要求（摘要）| AegisGate / 客户映射 | 就绪度 |
|---|---|---|---|
| 4.1 | 理解组织及其环境 | 客户自有；AegisGate 范围声明可用 | ⚪ 客户 |
| 4.2 | 相关方的需求和期望 | 客户自有 | ⚪ 客户 |
| 4.3 | AI 管理体系的范围 | AegisGate 范围声明在 [LICENSE](../../LICENSE) + 本文档 | 🟡 部分 — 客户扩展 |
| 4.4 | AI 管理体系 | 双人审批、审计链、metrics、rollouts（整套 AegisGate 栈）| 🟢 直接映射 |

### 3.2 第 5-7 章 — 领导作用 / 策划 / 支持

| Clause | 要求（摘要）| AegisGate / 客户映射 | 就绪度 |
|---|---|---|---|
| 5.x | AI 方针和角色 | RBAC 4 角色矩阵 + 客户方针文档 | 🟡 部分 |
| 6.x | AI 目标和策划 | 客户自有；AegisGate 提供测量原语 | 🟡 部分 |
| 7.1 | 资源 | 客户自有 | ⚪ 客户 |
| 7.2 | 能力 | 客户自有；AegisGate 文档支持入职 | ⚪ 客户 |
| 7.4 | 沟通 | AuditLogger + 告警 | 🟡 部分 |
| 7.5 | 形成文件的信息 | `docs/`、`memory-bank/`、`CHANGELOG.md` | 🟢 直接映射（AegisGate 范围内）|

### 3.3 第 8 章 — 运行

| Clause | 要求（摘要）| AegisGate / 客户映射 | 就绪度 |
|---|---|---|---|
| 8.1 | 运行的策划和控制 | `ConfigService` 版本化 + `RolloutController` | 🟢 直接映射 |
| 8.2 | AI 风险评价 | 客户自有；AegisGate metrics 反馈风险评审 | 🟡 部分 |
| 8.3 | AI 风险处置 | `RolloutController` 自动回滚、限流、护栏 | 🟡 部分 |

### 3.4 第 9-10 章 — 绩效评价 / 改进

| Clause | 要求（摘要）| AegisGate / 客户映射 | 就绪度 |
|---|---|---|---|
| 9.1 | 监视、测量、分析和评价 | `MetricsRegistry`（Prometheus）、`OpenTelemetry`、`Savings Dashboard` | 🟢 直接映射 |
| 9.2 | 内部审核 | `AuditLogger` chain_hash + `verifyChain()` | 🟢 直接映射 |
| 9.3 | 管理评审 | 客户自有；AegisGate 产出证据 | 🟡 部分 |
| 10.x | 持续改进 | `FeedbackBus` 事件流 + reflection workflow | 🟡 部分 |

### 3.5 PDCA 循环对接点

| PDCA 阶段 | AegisGate 原语 |
|---|---|
| **Plan** | `ConfigVersion` + `docs/specs/` + `docs/plans/`（本项目自己的 /plan 工作流就是该实践的范例）|
| **Do** | `RolloutController` 含 scope 匹配的灰度部署 |
| **Check** | `MetricsRegistry` + `RolloutMetricsProvider` + `Savings Dashboard` |
| **Act** | `RolloutController` 自动回滚 + `FeedbackBus` 持续改进闭环 |

---

## 4. 中国《生成式人工智能服务管理暂行办法》（骨架）

> 联合发布机关：网信办、发改委、教育部、科技部、工信部、公安部、广电总局（2023-07-13 发布；2023-08-15 施行）

| 条款 | 要求（摘要）| AegisGate / 客户映射 | 就绪度 |
|---|---|---|---|
| 第 4 条 | 坚持社会主义核心价值观；不歧视；尊重知识产权和个人权益 | 客户自有内容方针 + `InjectionDetector` + `PIIFilter` | 🟡 部分 |
| 第 7 条 | 训练数据合法、完整、准确 | 客户自有数据治理；AegisGate `AuditLogger` 记录数据 lineage 事件 | ⚪ 客户（网关不在训练环节）|
| 第 8 条 | 人工标注规则、标注人员培训、交叉校验 | 客户自有；AegisGate 不做标注 | ⚪ 客户 |
| 第 9 条 | 服务提供者责任；个人信息保护协议 | RBAC + 多租户隔离 + `PIIFilter` + 审计链 | 🟡 部分 |
| 第 10 条 | 服务提供者对用户输入的处理责任 | `PIIFilter`、`InjectionDetector`、审计 | 🟢 直接映射 |
| 第 11 条 | 服务提供者的内容审核义务 | `ExternalSafetyStage`（OpenAI Moderation、Perspective API）、`InjectionDetector` | 🟢 直接映射 |
| 第 12 条 | 生成内容的水印 / 标识 | 客户在应用层实现 | ⚪ 网关范围外 |
| 第 14 条 | 配合监管监督和检查 | `AuditLogger` chain_hash 导出、`aegisctl` 管理 CLI | 🟢 直接映射 |
| 第 15 条 | 服务提供者发现违法内容的处置责任 | `ExternalSafetyStage` + `RolloutController` 紧急暂停 | 🟡 部分 |
| 第 17 条 | 具有舆论属性或社会动员能力的服务需做安全评估 | 客户自有；AegisGate 证据通道可用 | 🟡 部分 |

---

## 5. 阶段 2 / 阶段 3 扩展路线

| 框架 | 阶段 | 范围 |
|---|---|---|
| **NIST AI RMF 1.0** | 阶段 2 | Govern / Map / Measure / Manage 4 function 映射 |
| **SOC 2 Type II** | 阶段 2 | TSC 100 control 映射（CC / A / C / PI / P 系列）|
| **GDPR 第 22 + 25 + 32 条** | 阶段 2 | 自动化决策、设计与默认数据保护、处理安全 |
| **HIPAA Security Rule（45 CFR §164.308–§164.314）** | 阶段 2（医疗包）| 管理 / 物理 / 技术保障 |
| **PBOC AI 应用指引** | 阶段 2（金融包）| 中国银行业 AI 指引叠加 |
| **PCI-DSS v4.0** | 阶段 2（金融包）| 支付行业叠加 |
| **等保 2.0** | 阶段 2（国内部署）| 信息系统安全等级保护 |

每个阶段 2 映射将产出：

- 详细 clause 表（每个 clause / control / 保障措施映射到具体 AegisGate 组件或客户责任行动）
- 证据收集 runbook（哪些审计日志查询 / 指标查询满足每个 control）
- 审计师就绪报告模板（PDF / HTML）

---

## 6. 如何使用本骨架

### 6.1 内部产品 / 销售对话

用就绪度列设置现实的客户预期。**绝不**夸大到"compliant"或"certified"——而是说"AegisGate
提供 X、Y、Z 条款的技术控制；你方团队负责管理体系条款（4.1 / 4.2 / 5.x / 7.1 等）"。

### 6.2 客户售前

尽早分享本文档。受监管行业的客户欣赏诚实的就绪度映射，胜过愿景式的营销说辞。

### 6.3 贡献者

如果你添加了新的 AegisGate 能力满足特定法规 clause，请：

1. 更新 §2 / §3 / §4 对应 clause 行
2. 引用源码路径（如 `src/server/audit_logger.cpp` 行号范围）
3. 提 PR 打标签 `compliance-mapping`
4. 若就绪度变化，更新等级（🔵 → 🟡 → 🟢）

### 6.4 审计师

本文档是审计入门**起点**，不是审计本身的替代。阶段 2 / TASK-W2 交付物将包括：

- 示例审计日志导出，演示 chain hash 完整性
- 示例 rollout 历史，演示自动回滚
- 示例双人审批 workflow 证据
- 每 clause 控制证据 checklist

---

## 另参见

- [AegisOps 愿景](../positioning/aegisops-vision_zh.md) — 为什么我们做这件事
- [路线图 v4](../ROADMAP_zh.md) — 阶段 2 / 阶段 3 合规包里程碑
- [架构指南](../guides/architecture_zh.md) — 整体架构含审计链
- [控制面指南](../guides/control-plane_zh.md) — 版本化与双人审批 workflow
