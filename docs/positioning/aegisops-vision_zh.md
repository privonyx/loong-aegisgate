# AegisOps 愿景

> **Disclaimer / 免责声明：** 本文档将 AegisGate / AegisOps 的能力映射到法规框架与产品定位，
> 仅用于**规划与就绪度评估**目的。本文档不构成法律意见，也不声称已通过任何合规认证。
> §3 中引用的客户案例为期望目标——正式客户引用将随后补齐。需要正式合规背书的客户应聘请合格审计师
> （如针对 ISO 42001、SOC 2 Type II，或欧盟 2024/1689 下的 AI Act 一致性评估）。

**状态：** 愿景文档（三阶段重定位的阶段 1，TASK-20260520-01）
**最后更新：** 2026-05-20
**姊妹文档：** [合规对标](../compliance/README_zh.md) · [路线图 v4](../ROADMAP_v4_zh.md) · [English](aegisops-vision.md)

---

## 1. 定位（What is AegisOps）

### 1.1 一句话定位

**AegisOps 是开源 AI 治理与运营平台** —— 一个统一控制平面，让企业能以管理数据库、网络、云基础设施
的同等严谨度来运行 AI 技术栈：版本化、可审计、成本可归因、监管就绪。

### 1.2 与 AegisGate 的关系

AegisOps 构建于开源的 **AegisGate** 核心引擎之上。两个品牌形成有意为之的双层架构：

```
┌─────────────────────────────────────────────────────────┐
│  AegisOps  — 产品 / 平台品牌                            │
│  • 商业化 GTM / 愿景叙事 / 客户互动                     │
│  • 行业合规包（金融 / 医疗 / 政府）                     │
│  • SaaS 控制平面 / 多企业租户运营                       │
├─────────────────────────────────────────────────────────┤
│  AegisGate — 开源核心品牌                               │
│  • GitHub repo / npm 包 / 二进制 / DNS（保持不变）       │
│  • Apache 2.0 / 多模型网关引擎                          │
│  • 开发者社区 / 贡献者文档                              │
└─────────────────────────────────────────────────────────┘
```

**AegisGate 核心永久保持 Apache 2.0 开源。** AegisOps 在其上叠加产品/商业化层 ——
绝不把核心转为闭源。详见 §6 的开源承诺。

### 1.3 对标坐标系

| 维度 | AegisOps | Datadog（LLM 观测）| Cloudflare AI Gateway | Portkey | LiteLLM | Kong AI Gateway |
|---|---|---|---|---|---|---|
| **部署模式** | 自托管 + SaaS（规划中）| 仅 SaaS | 仅 SaaS | SaaS + 私有化（付费）| 自托管（Python）| 自托管 |
| **多模型中立性** | ✅ 7 厂商 + 自定义 | ⚠️ 仅观测 | ✅ 有限集 | ✅ 多家 | ✅ 多家 | ⚠️ 插件 |
| **审计链式哈希** | ✅ 内建（chain_hash）| ❌ | ❌ | ⚠️ 仅日志 | ❌ | ❌ |
| **配置变更双人审批** | ✅ 内建（W3 workflow）| ❌ | ❌ | ❌ | ❌ | ❌ |
| **灰度发布 + 自动回滚** | ✅ RolloutController | ⚠️ 仅观测 | ❌ | ⚠️ 手动 | ❌ | ⚠️ 手动 |
| **语义缓存含租户隔离** | ✅ sha256 + tenant_id | ⚠️ | ✅ | ✅ | ⚠️ | ❌ |
| **成本仪表盘（Savings）** | ✅ 内建 | ✅ | ⚠️ 基础 | ✅ | ⚠️ | ❌ |
| **内建 PII / 安全护栏** | ✅ 三层 + 外部 API shadow | ⚠️ 加购 | ⚠️ 基础 | ✅ | ❌ | ⚠️ 插件 |
| **许可证模式** | Apache 2.0 核心 + 企业增量 | 闭源 | 闭源 | 闭源 | MIT | Apache 2.0 + EE |
| **C++ 原生性能** | ✅（~47μs 管道）| N/A | N/A | N/A | ❌（Python GIL）| ⚠️（Lua/Go）|

**AegisOps 优势点：** 同时满足以下五项的唯一平台：(a) 开源核心 + (b) 链式哈希审计与双人审批 +
(c) 配置变更的灰度发布与自动回滚 + (d) 多模型中立性 + (e) 自托管部署。这正是受监管行业既需要、
而纯 SaaS（Datadog / Portkey / Cloudflare）和纯自托管库（LiteLLM）都给不了的精准交集。

## 2. 三大支柱

AegisOps 把三个当前被割裂在不同工具里的运营能力统一起来：

### 2.1 LLMOps — 多模型运营

| 能力 | AegisGate 组件 | 价值 |
|---|---|---|
| 7+ 厂商统一 API | `ConnectorFactory`、`OpenAIConnector`、`ClaudeConnector` 等 | 接入一次，覆盖所有模型 |
| Provider Manifest + Conformance | `src/gateway/provider_spec/` | 厂商中立接口契约 |
| 智能路由（成本/质量/延迟）| `RoutingStrategy`、`MLScoringStrategy`、`ABTestStrategy` | 为每个请求选对模型 |
| 按区域/租户/百分比灰度 | `RolloutController`、`ScopeMatcher` | 渐进式安全发布 |
| 指标恶化自动回滚 | `RolloutMetricsProvider`、`RolloutTicker` | 秒级从坏配置中恢复 |
| 多模态代理（5 端点）| `ModalityRouter`、5 个 `OpenAI*Handler` | embeddings、图像、音频、moderation |
| Function calling / Tool use | OpenAI + Anthropic 双格式 | 现代 agent 工作流 |

### 2.2 LLM Security — 纵深防御

| 能力 | AegisGate 组件 | 价值 |
|---|---|---|
| PII 过滤（入站 & 出站）| `PIIFilter::mask`、RE2 正则包 | GDPR / HIPAA 数据主体就绪 |
| Prompt 注入检测 | `InjectionDetector`（L1+L2+可选 L3 ONNX）| 防御语义级攻击 |
| 外部安全 API（shadow + active）| `OpenAIModerationStage`、`PerspectiveAPIStage` | 多厂商最佳安全实践 |
| 链式哈希审计日志 | `AuditLogger`、`chain_hash` | 监管证据防篡改 |
| 配置变更双人审批 | `ConfigService` W3 workflow | 职责分离（SOC 2 CC6.x 风格）|
| 模态级限流 | `ModalityRateLimiter` + `ErrorCode::ModalityQuotaExceeded` | 模态粒度的 DoS / 滥用防护 |
| RBAC + 多租户隔离 | 4 角色 RBAC、tenant_id 全链传递 | 硬租户边界 |
| SSO（OIDC/PKCE）+ MFA（TOTP）+ SCIM 2.0 | `OIDCProvider`、`TOTPProvider`、`SCIMController` | 企业身份集成 |
| Ed25519 license 签名 | `FeatureGate::validateEd25519Signature` | 商业 license 防篡改 |
| 审计日志加密 | AES-256-GCM via `Encryption` | 审计踪迹的机密性 |

### 2.3 LLM FinOps — 成本可归因

| 能力 | AegisGate 组件 | 价值 |
|---|---|---|
| 语义缓存（跨请求）| `SemanticCache` + HNSW + ONNX BGE | 重复查询 30-60% 支出降低 |
| Prompt 压缩 | `PromptCompressor` | 不损质量的 token 削减 |
| 成本感知路由 | `CostAwareStrategy` | 选最便宜且可行的模型 |
| 租户级预算 | `CostTracker`、`effectiveTenantId` 过滤 | 预算不只是观测，是强制 |
| 实时省钱仪表盘 | `SavingsAggregator` + Admin `Savings.tsx` | 向 CFO 证明 ROI |
| 成本优化建议 | `CostOptimizer.getRecommendations()` | 持续改进闭环 |
| 对话缓存（多轮）| `ConversationIdResolver`、`SummarizerFactory` | 多轮对话也能受益 |
| 缓存迁移工具 | `CacheMigrator` CLI | 在向量 DB 间零停机迁移缓存 |

## 3. 5 年路线

> 详细里程碑在 [ROADMAP_v4_zh.md](../ROADMAP_v4_zh.md)，此处仅给出摘要：

| 年份 | 主题 | 主要交付 |
|---|---|---|
| **Year 1（2026）** | 品牌建立 + 首批付费客户 | 阶段 1 重定位（本任务）→ 阶段 2 金融合规包 → 3-5 个付费 PoC 客户（目标）|
| **Year 2（2027）** | PMF + 渠道合作 | SaaS 控制平面 MVP / ISO 42001 就绪度评估 / ISV 渠道（至少一个主流云市场上架）|
| **Year 3（2028）** | Agent 治理 | MCP / A2A 协议支持 / Agent RACI workflow / 100+ 客户（目标）|
| **Year 4（2029）** | 全球化 | 多区域 + 数据驻留（欧盟 / 中东 / 东南亚）/ SOC 2 Type II 就绪度 |
| **Year 5（2030）** | 平台化 | 第三方治理规则市场 / ARR 规模化 / "开源 AI 治理"细分类目领跑 |

> 客户案例待 Year 1 阶段 2（金融合规包上线）签下首批客户后补齐。

## 4. 抗 AI 冲击的 4 道护城河

一个自然的问题：未来 AI 越来越强，AegisOps 的竞品不就被 AI 自动生成出来了吗？
防御依赖四道护城河，每一道都会**随 AI 进步而增强**：

### 4.1 审计连续性 lock-in

每个采用 AegisOps 的客户从打开那一刻起就开始产出链式哈希审计证据。监管要求证据保存 7-10 年
（行业规则可能更长）。**AI 可以一周内重写软件，但无法重写 7 年的密码学链式审计历史。**
切换成本随时间单调累积。

### 4.2 多厂商中立性

OpenAI 不会做 Anthropic 的治理平面，反之亦然。云厂商（AWS Bedrock / Azure AI Foundry /
Google Vertex）只为自家第一方模型选型做治理。**中立性位置对模型厂商和云厂商本身在结构上不可得。**
随着多模型部署成为常态（所有数据都指向这点），中立治理愈加值钱。

### 4.3 法规护城河

欧盟 AI Act、ISO 42001、国内《生成式 AI 服务管理办法》、NIST AI RMF，加上十几个行业法规，
构成移动靶。**预建的映射、conformance 证据、审计就绪报告需要人力持续维护，客户切换需要重新投入。**
先行者被引用进合规咨询 playbook；这是低衰减的飞轮。

### 4.4 数据主权与自托管

许多受监管行业（金融、医疗、政府、国防）根本不能把审计日志或敏感 prompt 发给第三方 SaaS。
**开源 + 自托管不是 feature，是进入整片市场的入场券。** 纯 SaaS 竞品在这些市场段被结构性排除，
功能对等也没用。

## 5. 风险与对冲

| ID | 风险 | 对冲 |
|---|---|---|
| R1 | 主流云（AWS / Azure / GCP）做第一方治理 | 押多厂商中立性 + Apache 2.0 核心；与中型云（Oracle / IBM / 腾讯云 / 阿里云）合作，中立性在这里受欢迎 |
| R2 | 模型厂商（OpenAI / Anthropic）做原生治理 | 定位为"治理所有人"——厂商治理结构上只能是第一方 |
| R3 | AI 生成同类竞品 | 审计连续性（§4.1）+ 法规映射深度（§4.3）提供 AI 无法快速复制的切换成本护城河 |
| R4 | 经济下行砍 IT 预算 | 主打价值是**成本节省**（Savings Dashboard 显示 30-60% 支出下降）—— 即使紧缩周期也能作为"节流"叙事生存 |
| R5 | 开源贡献者白嫖企业 feature | 开源/商业边界清晰（`FeatureGate` + Ed25519 License）；Apache 2.0 核心由社区驱动 |
| R6 | 法规变动速度超过映射跟进 | 投资人级别的 `docs/compliance/` 映射仓库 / 法律专家贡献路径 / 版本化的法规 delta |
| R7 | 表面上看与 LiteLLM / Portkey 重叠 | 差异化在可审计性、双人审批、灰度发布、自托管——不在原始厂商数量 |

## 6. 治理与开源承诺

### 6.1 核心永久 Apache 2.0

AegisGate 开源核心（所有 `src/`、`include/`、`tests/` 中不需要 Ed25519 license 即可编译或运行的部分）
将**永久**保留 Apache 2.0 许可证。我们不会把核心改许可到 BSL、SSPL、ELv2 或任何 source-available
许可。此承诺通过 [LICENSE](../../LICENSE) 与 [GOVERNANCE.md](../../GOVERNANCE.md) 中的开源社区
章程进行固化。

### 6.2 双层品牌边界

| 关注点 | 归 AegisGate（开源）| 归 AegisOps（商业）|
|---|---|---|
| 多厂商网关引擎 | ✅ | — |
| 全部现有安全护栏（PII / 注入 / 审计）| ✅ | — |
| 语义缓存 + 成本优化 | ✅ | — |
| RBAC / 多租户 / SSO 核心 | ✅ | — |
| 控制面 gRPC API | ✅ | — |
| RolloutController + ScopeMatcher | ✅ | — |
| Provider Manifest + Conformance | ✅ | — |
| **行业合规包**（金融 / 医疗 / 政府）| — | ✅ |
| **SaaS 多企业控制平面** | — | ✅ |
| **托管 + on-call SLA** | — | ✅ |
| **第三方治理规则市场收入分成** | — | ✅ |
| **审计 / 合规背书报告**（每客户）| — | ✅ |

原则：**运营和技术原语保持开放；商业化包装、多租户 SaaS 经济、合规背书服务是 AegisOps 的收费点。**

### 6.3 商业化条款

定价细则、license 等级、商业化条款属于阶段 3 范围（见 [ROADMAP_v4_zh.md](../ROADMAP_v4_zh.md)
阶段 3）。当前承诺：

- 所有 API 端点、文件格式、协议在社区版和企业版之间保持向后兼容
- 自托管企业部署不回传遥测（无 opt-in 即无 telemetry）
- 客户审计日志和配置绝不被 AegisOps 员工访问，除非有文档化的事件响应授权

---

## 客户案例

_客户案例将随阶段 2（金融合规包上线）签下首批付费客户后补齐。此章节为意向占位。_

---

## 另参见

- [合规对标](../compliance/README_zh.md) — EU AI Act / ISO 42001 / 国内办法就绪度映射（骨架）
- [路线图 v4](../ROADMAP_v4_zh.md) — 详细 5 年里程碑
- [架构](../guides/architecture_zh.md) — AegisGate 技术架构
- [控制面指南](../guides/control-plane_zh.md) — 版本化 + 审批流程
- [省钱仪表盘指南](../guides/admin-savings_zh.md) — FinOps 实战
