# AegisOps Vision

> **Disclaimer:** This document maps AegisGate / AegisOps capabilities to
> regulatory frameworks and product positioning for **planning and readiness**
> purposes. It does **not** constitute legal advice or claim of compliance
> certification. Customer case studies referenced in §3 are aspirational
> targets — formal customer references will follow. Customers requiring formal
> compliance attestation should engage qualified auditors (e.g., for ISO 42001,
> SOC 2 Type II, or AI Act conformity assessment under EU 2024/1689).

**Status:** Vision document (Stage 1 of 3-stage repositioning, TASK-20260520-01)
**Last updated:** 2026-05-20
**Sister documents:** [Compliance Mapping](../compliance/README.md) · [Roadmap v4](../ROADMAP_v4.md) · [中文版](aegisops-vision_zh.md)

---

## 1. Positioning (What is AegisOps)

### 1.1 One-Sentence Positioning

**AegisOps is The Open AI Governance & Operations Platform** — a unified
control plane that helps enterprises run their AI stack with the same rigor
they apply to databases, networks, and cloud infrastructure: versioning,
auditability, cost accountability, and regulatory readiness.

### 1.2 Relationship to AegisGate

AegisOps is built on top of the open-source **AegisGate** core engine. The two
brands form a deliberate two-layer architecture:

```
┌─────────────────────────────────────────────────────────┐
│  AegisOps  — Product / Platform Brand                   │
│  • Commercial GTM, vision, customer engagement          │
│  • Industry compliance packs (finance, healthcare, gov) │
│  • SaaS control plane, multi-tenant operations          │
├─────────────────────────────────────────────────────────┤
│  AegisGate — Open-Source Core Brand                     │
│  • GitHub repo, npm package, binary, DNS (unchanged)    │
│  • Apache 2.0, multi-provider gateway engine            │
│  • Developer community, contributor docs                │
└─────────────────────────────────────────────────────────┘
```

**The AegisGate core stays Apache-2.0 open source forever.** AegisOps adds
product/commercial layers on top — without ever turning the core proprietary.
See §6 for the open-source commitment.

### 1.3 Comparison Coordinate System

| Dimension | AegisOps | Datadog (LLM Obs) | Cloudflare AI Gateway | Portkey | LiteLLM | Kong AI Gateway |
|---|---|---|---|---|---|---|
| **Deploy model** | Self-hosted + SaaS (planned) | SaaS-only | SaaS-only | SaaS + on-prem (paid) | Self-hosted (Python) | Self-hosted |
| **Multi-provider neutrality** | ✅ 7 providers + custom | ⚠️ Observe-only | ✅ Limited set | ✅ Many | ✅ Many | ⚠️ Plugin-based |
| **Audit chain w/ hash linkage** | ✅ Built-in (chain_hash) | ❌ | ❌ | ⚠️ Logs only | ❌ | ❌ |
| **Two-person config approval** | ✅ Built-in (W3 workflow) | ❌ | ❌ | ❌ | ❌ | ❌ |
| **Canary + auto-rollback** | ✅ RolloutController | ⚠️ Observation only | ❌ | ⚠️ Manual | ❌ | ⚠️ Manual |
| **Semantic cache w/ tenant isolation** | ✅ Sha256 + tenant_id mix | ⚠️ | ✅ | ✅ | ⚠️ | ❌ |
| **Cost dashboard (Savings)** | ✅ Built-in | ✅ | ⚠️ Basic | ✅ | ⚠️ | ❌ |
| **PII / safety guardrails built-in** | ✅ 3 layers + external API shadow | ⚠️ Add-on | ⚠️ Basic | ✅ | ❌ | ⚠️ Plugin |
| **License model** | Apache 2.0 core + enterprise add-ons | Proprietary | Proprietary | Proprietary | MIT | Apache 2.0 + EE |
| **C++ native performance** | ✅ (~47μs pipeline) | N/A | N/A | N/A | ❌ (Python GIL) | ⚠️ (Lua/Go) |

**Where AegisOps wins:** the only platform that combines (a) open-source core
with (b) chain-hashed audit and two-person approval, (c) canary +
auto-rollback for config changes, (d) multi-provider neutrality, and (e)
self-hosted deployment. This is the precise intersection regulated industries
need but neither pure SaaS (Datadog, Portkey, Cloudflare) nor pure
self-hosted libraries (LiteLLM) deliver.

## 2. Three Pillars

AegisOps unifies three operational disciplines that today are fragmented
across separate tools:

### 2.1 LLMOps — Multi-Model Operations

| Capability | AegisGate component | Pillar value |
|---|---|---|
| Unified API across 7+ providers | `ConnectorFactory`, `OpenAIConnector`, `ClaudeConnector`, ... | One integration, every model |
| Provider Manifest + Conformance | `src/gateway/provider_spec/` | Vendor-neutral interface contract |
| Smart routing (cost/quality/latency) | `RoutingStrategy`, `MLScoringStrategy`, `ABTestStrategy` | The right model for the request |
| Canary rollout per region/tenant/% | `RolloutController`, `ScopeMatcher` | Safe progressive deployment |
| Auto-rollback on metric drift | `RolloutMetricsProvider`, `RolloutTicker` | Recover from bad configs in seconds |
| Multi-modal proxy (5 endpoints) | `ModalityRouter`, 5 `OpenAI*Handler` | Embeddings, images, audio, moderation |
| Function calling / Tool use | OpenAI + Anthropic format support | Modern agentic workflows |

### 2.2 LLM Security — Defense in Depth

| Capability | AegisGate component | Pillar value |
|---|---|---|
| PII filtering (in & out) | `PIIFilter::mask`, RE2 regex pack | GDPR / HIPAA data-subject readiness |
| Prompt injection detection | `InjectionDetector` (L1+L2+optional L3 ONNX) | Defense against semantic attacks |
| External safety APIs (shadow + active) | `OpenAIModerationStage`, `PerspectiveAPIStage` | Best-of-breed safety, multi-vendor |
| Audit log with chain hash | `AuditLogger`, `chain_hash` | Tamper-evident regulatory evidence |
| Two-person approval for config changes | `ConfigService` W3 workflow | Separation of duties (SOC 2 CC6.x style) |
| Per-modality rate limiting | `ModalityRateLimiter` + `ErrorCode::ModalityQuotaExceeded` | DoS / abuse prevention at modality granularity |
| RBAC + multi-tenant isolation | 4-role RBAC, tenant_id propagation | Hard tenant boundaries |
| SSO (OIDC/PKCE) + MFA (TOTP) + SCIM 2.0 | `OIDCProvider`, `TOTPProvider`, `SCIMController` | Enterprise identity integration |
| Ed25519 license signing | `FeatureGate::validateEd25519Signature` | Tamper-evident commercial licensing |
| Audit log encryption | AES-256-GCM via `Encryption` | Confidentiality of audit trail |

### 2.3 LLM FinOps — Cost Accountability

| Capability | AegisGate component | Pillar value |
|---|---|---|
| Semantic cache (cross-request) | `SemanticCache` + HNSW + ONNX BGE | 30–60% spend reduction on repeat queries |
| Prompt compression | `PromptCompressor` | Token reduction without quality loss |
| Cost-aware routing | `CostAwareStrategy` | Pick cheapest viable model per request |
| Per-tenant budgets | `CostTracker`, `effectiveTenantId` filtering | Budgets enforce, not just observe |
| Real-time savings dashboard | `SavingsAggregator` + Admin `Savings.tsx` | Show CFO the ROI |
| Cost optimization recommendations | `CostOptimizer.getRecommendations()` | Continuous improvement loop |
| Conversation cache (multi-turn) | `ConversationIdResolver`, `SummarizerFactory` | Even multi-turn chats benefit |
| Cache migration tool | `CacheMigrator` CLI | Move cache between vector DBs without downtime |

## 3. Five-Year Roadmap

> Detailed milestones live in [ROADMAP_v4.md](../ROADMAP_v4.md). Brief
> overview here:

| Year | Theme | Primary deliverables |
|---|---|---|
| **Year 1 (2026)** | Brand established + first paid customers | Stage 1 rebrand (this task) → Stage 2 finance compliance pack → 3–5 paid PoC customers (aspirational) |
| **Year 2 (2027)** | PMF + channel partners | SaaS control plane MVP, ISO 42001 readiness assessment, ISV channel (one major cloud marketplace listing) |
| **Year 3 (2028)** | Agent governance | MCP / A2A protocol support, Agent RACI workflow, 100+ customers (aspirational) |
| **Year 4 (2029)** | Global expansion | Multi-region + data residency (EU, MENA, SEA), SOC 2 Type II readiness |
| **Year 5 (2030)** | Platform play | Third-party governance rule marketplace, ARR scaling, category leadership in "open AI governance" |

> Customer case studies to follow — Year-1 targets are aspirational pending
> first signed customers in Stage 2 (finance compliance pack rollout).

## 4. Four Moats Against AI Disruption

A natural question: in a world where AI itself becomes ever more capable,
won't AI build AegisOps' competitors automatically? The defense rests on four
moats, each of which **strengthens** as AI advances:

### 4.1 Audit Continuity Lock-In

Every customer who adopts AegisOps starts producing chain-hashed audit
evidence the moment they turn it on. Regulatory regimes require evidence
retention for 7–10 years (or more under sectoral rules). **An AI can rebuild
the software in a weekend; it cannot rebuild seven years of cryptographically
linked audit history.** Switching costs compound monotonically over time.

### 4.2 Multi-Vendor Neutrality

OpenAI cannot ship a governance plane for Anthropic. Anthropic cannot ship
one for OpenAI. Cloud providers (AWS Bedrock, Azure AI Foundry, Google
Vertex) ship governance only for their own first-party model selection. **The
neutrality position is structurally unavailable to the model and cloud
providers themselves.** As multi-model deployments become the norm (which all
data suggests), neutral governance becomes more valuable.

### 4.3 Regulatory Moat

EU AI Act, ISO 42001, China's Generative AI Services Management measures,
NIST AI RMF, and a dozen sectoral regulations create a moving target.
**Pre-built mappings, conformance evidence, and audit-ready reports take
human effort to maintain and customer effort to switch off.** First-movers
get cited in compliance consulting playbooks; this is a low-decay flywheel.

### 4.4 Data Sovereignty and Self-Hosting

Many regulated industries (finance, healthcare, government, defense) cannot
ship audit logs or sensitive prompts to third-party SaaS, period. **Open
source + self-hosted deployment is not a feature; it is a license to
participate in entire market segments.** Pure-SaaS competitors are
structurally excluded from these segments regardless of feature parity.

## 5. Risks and Mitigations

| ID | Risk | Mitigation |
|---|---|---|
| R1 | Major cloud (AWS / Azure / GCP) ships first-party governance | Lean into multi-vendor neutrality + Apache 2.0 core; partner with mid-tier clouds (Oracle, IBM, Tencent, Alibaba) where neutrality is welcomed |
| R2 | Model vendor (OpenAI, Anthropic) ships native governance | Position as "the one that governs all of them" — vendor governance is structurally first-party-only |
| R3 | AI-generated competitor product | Audit continuity (§4.1) + regulatory mapping depth (§4.3) provide switching-cost moats AI cannot replicate quickly |
| R4 | Economic downturn cuts IT budgets | Position primary value as **cost savings** (Savings Dashboard shows 30–60% spend reduction) — survives as a cost-out story even in lean cycles |
| R5 | Open-source contributors free-ride enterprise features | Open-core boundary clearly defined (`FeatureGate` + Ed25519 License); Apache 2.0 core remains community-driven |
| R6 | Regulatory frameworks shift faster than mapping can keep up | Investor-grade `docs/compliance/` mapping repo, community contribution path for legal experts, version-controlled regulation deltas |
| R7 | Apparent overlap with LiteLLM / Portkey at surface level | Differentiation on auditability, two-person approval, canary rollout, self-hosted — not on raw provider count |

## 6. Governance and Open-Source Commitment

### 6.1 Apache 2.0 Forever for the Core

The AegisGate open-source core (everything in `src/`, `include/`, `tests/`
that does not require an Ed25519 license to compile or run) will remain
under Apache 2.0 license **permanently**. We will not relicense the core to
BSL, SSPL, ELv2, or any source-available license. This commitment is
codified in [LICENSE](../../LICENSE) and the open-source community charter
in [GOVERNANCE.md](../../GOVERNANCE.md).

### 6.2 Two-Layer Brand Boundary

| Concern | Belongs to AegisGate (open) | Belongs to AegisOps (commercial) |
|---|---|---|
| Multi-provider gateway engine | ✅ | — |
| All current security guardrails (PII, injection, audit) | ✅ | — |
| Semantic cache + cost optimization | ✅ | — |
| RBAC / multi-tenant / SSO core | ✅ | — |
| Control plane gRPC API | ✅ | — |
| RolloutController + ScopeMatcher | ✅ | — |
| Provider Manifest + Conformance | ✅ | — |
| **Industry-specific compliance packs** (finance, healthcare, gov) | — | ✅ |
| **SaaS multi-tenant control plane** | — | ✅ |
| **Managed hosting and on-call SLA** | — | ✅ |
| **Third-party governance rule marketplace revenue share** | — | ✅ |
| **Audit / compliance attestation reports** (per customer) | — | ✅ |

The principle: **operational and technical primitives stay open; commercial
packaging, multi-tenant SaaS economics, and compliance attestation services
are where AegisOps charges.**

### 6.3 Commercial Terms

Detailed pricing, license tiers, and commercial terms are stage-3 scope
(see [ROADMAP_v4.md](../ROADMAP_v4.md) Stage 3). Current commitment:

- All API endpoints, file formats, and on-the-wire protocols stay backward
  compatible across community and enterprise editions
- Self-hosted enterprise deployments do not phone home (no telemetry without
  opt-in)
- Customer audit logs and configurations are never accessed by AegisOps
  staff without a documented incident response engagement

---

## Customer Case Studies

_Customer case studies will be added as Stage 2 (finance compliance pack
rollout) onboards first paid customers. This section is intentionally a
placeholder._

---

## See Also

- [Compliance Mapping](../compliance/README.md) — EU AI Act / ISO 42001 /
  China measures readiness mapping (skeleton)
- [Roadmap v4](../ROADMAP_v4.md) — Detailed five-year milestones
- [Architecture](../guides/architecture.md) — AegisGate technical
  architecture
- [Control Plane Guide](../guides/control-plane.md) — Versioning + approval
  workflow
- [Savings Dashboard Guide](../guides/admin-savings.md) — FinOps in practice
