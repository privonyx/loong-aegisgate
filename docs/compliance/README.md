# AegisOps Compliance Mapping

> **Disclaimer:** This document maps AegisGate / AegisOps capabilities to
> regulatory frameworks for **planning and readiness** purposes. It does
> **not** constitute legal advice or claim of compliance certification.
> Customers requiring formal compliance attestation should engage qualified
> auditors (e.g., for ISO 42001, SOC 2 Type II, or AI Act conformity
> assessment under EU Regulation 2024/1689). Readiness levels below are
> **self-assessed against publicly available framework text**; they are not
> third-party attestations.

**Status:** Compliance mapping skeleton (Stage 1, TASK-20260520-01).
Detailed clause-by-clause mappings, control evidence, and audit-ready
reports are Stage 2 (TASK-W2) deliverables.

**Last updated:** 2026-05-20
**Sister documents:** [Vision](../positioning/aegisops-vision.md) · [Roadmap v4](../ROADMAP.md) · [中文版](README_zh.md)

---

## 1. Overview

### 1.1 First-Wave Frameworks

This stage 1 skeleton covers the three largest regulatory markets relevant
to AI systems today:

| Framework | Jurisdiction | Status | Primary text |
|---|---|---|---|
| **EU AI Act** (Regulation 2024/1689) | European Union | Force from 2024-08-01; high-risk obligations from 2026-08-02 | [eur-lex.europa.eu/eli/reg/2024/1689](https://eur-lex.europa.eu/eli/reg/2024/1689) |
| **ISO/IEC 42001:2023** AI management system | International standard | Published 2023-12 | [iso.org/standard/81230.html](https://www.iso.org/standard/81230.html) |
| **生成式人工智能服务管理暂行办法** (China Interim Measures for the Management of Generative AI Services) | People's Republic of China | Effective 2023-08-15 | [cac.gov.cn/2023-07/13/c_1690898327029107.htm](http://www.cac.gov.cn/2023-07/13/c_1690898327029107.htm) |

NIST AI RMF 1.0, SOC 2 Type II, HIPAA, GDPR Article 22, and sector-specific
regulations (PBOC, PCI-DSS) are roadmapped for Stage 2 expansion. See §5.

### 1.2 Wording Conventions: Readiness vs Compliance

This document uses precise language to avoid misrepresentation:

| Avoided wording | Why | Preferred wording |
|---|---|---|
| ~~"compliant"~~ | Implies passed third-party audit | "aligns with" / "supports requirements for" |
| ~~"certified"~~ | Reserved for third-party certification body | "readiness assessment available" / "evidence collection in place" |
| ~~"production-grade"~~ | Subjective; no agreed bar | "production-ready architecture; customer validation in progress" |
| ~~"meets all requirements"~~ | Few systems do; specifics matter | "addresses the following requirements: ..." (enumerate) |

The columns labeled **Readiness** below use a four-level scale:

| Level | Meaning |
|---|---|
| 🟢 **Direct mapping** | AegisGate component directly addresses the requirement; evidence collection automated |
| 🟡 **Partial mapping** | Component addresses part of the requirement; customer configuration completes the picture |
| 🔵 **Architecture-ready** | Architectural primitives present; concrete implementation requires per-customer engagement (Stage 2) |
| ⚪ **Out of scope for AegisGate core** | Customer / operator responsibility; AegisGate provides evidence channels but not the controls themselves |

### 1.3 Roadmap Beyond Stage 1

Stage 2 (TASK-W2 / finance compliance pack) will expand this skeleton with:

- Detailed clause-by-clause mappings (every relevant EU AI Act Article, every
  ISO 42001 Clause 4–10, every Chinese measure article)
- Per-clause evidence collection runbooks (which `metrics`, `audit_logs`,
  `config_versions` records satisfy each clause)
- Auditor-ready report templates (PDF/HTML generation hooks)
- Industry-specific overlays (PBOC AI guidance for finance,
  HIPAA Security Rule for healthcare)

Stage 3 (TASK-W3 / SaaS control plane) will add:

- Multi-jurisdiction tenant routing (data residency enforcement)
- Continuous compliance monitoring dashboard (real-time per-clause status)

---

## 2. EU AI Act Readiness Mapping (Skeleton)

> **Scope of skeleton:** AegisOps acts as a **provider** of an AI system
> component (an AI gateway) and as a **deployer** support layer for
> downstream customer AI systems. The mapping below focuses on the
> obligations most directly addressed by AegisGate primitives. Customer
> systems built on top of AegisGate still need their own conformity
> assessment for high-risk use cases.

### 2.1 Title III, Chapter 2 — High-Risk AI Systems Requirements

| Article | Requirement (summary) | AegisGate component | Readiness |
|---|---|---|---|
| Art. 8 | Compliance with the requirements of this Chapter | All controls below | 🔵 Architecture-ready |
| Art. 9 | Risk management system | `MetricsRegistry`, `FeedbackBus`, `RolloutMetricsProvider` (continuous monitoring) | 🟡 Partial — needs per-customer risk register |
| Art. 10 | Data and data governance | `PIIFilter`, `AuditLogger`, `CacheMigrator` (data lineage) | 🟡 Partial — training data governance customer-owned |
| Art. 11 | Technical documentation | `docs/specs/`, `docs/guides/`, `docs/compliance/` (this doc) | 🔵 Architecture-ready |
| Art. 12 | Record-keeping (logging) | `AuditLogger` with `chain_hash`, `PersistentStore` retention | 🟢 Direct mapping |
| Art. 13 | Transparency and provision of information to deployers | `docs/guides/`, OpenAPI spec, Provider Manifest | 🟢 Direct mapping |
| Art. 14 | Human oversight | `ConfigService` two-person approval (W3), `RolloutController` manual gates | 🟢 Direct mapping |
| Art. 15 | Accuracy, robustness and cybersecurity | `InjectionDetector`, `ExternalSafetyStage`, `RolloutController` auto-rollback, rate limits | 🟡 Partial — model-level accuracy customer-owned |

### 2.2 Title IV — Transparency Obligations

| Article | Requirement (summary) | AegisGate component | Readiness |
|---|---|---|---|
| Art. 50(1) | AI system interaction disclosure | Customer-implemented at application layer | ⚪ Out of scope for gateway |
| Art. 50(2) | Synthetic content marking (AI-generated) | Customer-implemented; AegisGate audit captures generation events | ⚪ Out of scope for gateway |
| Art. 50(4) | Emotion-recognition / biometric categorization notice | Customer-implemented | ⚪ Out of scope for gateway |

### 2.3 Title VIII — Post-Market Monitoring, Information Sharing, Market Surveillance

| Article | Requirement (summary) | AegisGate component | Readiness |
|---|---|---|---|
| Art. 72 | Post-market monitoring system | `FeedbackBus`, `MetricsFeedbackSubscriber`, `RolloutMetricsProvider` | 🟢 Direct mapping |
| Art. 73 | Reporting of serious incidents | `AuditLogger`, alerting channels (Stage 2 reports) | 🔵 Architecture-ready |

> Detailed clause-by-clause mapping (every relevant Article 8–15 sub-clause)
> is Stage 2 scope.

---

## 3. ISO/IEC 42001:2023 Readiness Mapping (Skeleton)

### 3.1 Clause 4 — Context of the Organization

| Clause | Requirement (summary) | AegisGate / customer mapping | Readiness |
|---|---|---|---|
| 4.1 | Understanding the organization and its context | Customer-owned; AegisGate scope statement available | ⚪ Customer |
| 4.2 | Needs and expectations of interested parties | Customer-owned | ⚪ Customer |
| 4.3 | Scope of the AI management system | AegisGate scope statement in [LICENSE](../../LICENSE) + this doc | 🟡 Partial — customer extends |
| 4.4 | AI management system | Two-person approval, audit chain, metrics, rollouts (entire AegisGate stack) | 🟢 Direct mapping |

### 3.2 Clause 5–7 — Leadership / Planning / Support

| Clause | Requirement (summary) | AegisGate / customer mapping | Readiness |
|---|---|---|---|
| 5.x | AI policy and roles | RBAC 4-role matrix + customer policy doc | 🟡 Partial |
| 6.x | AI objectives and planning | Customer-owned; AegisGate supplies measurement primitives | 🟡 Partial |
| 7.1 | Resources | Customer-owned | ⚪ Customer |
| 7.2 | Competence | Customer-owned; AegisGate documentation supports onboarding | ⚪ Customer |
| 7.4 | Communication | AuditLogger + alerting | 🟡 Partial |
| 7.5 | Documented information | `docs/`, `memory-bank/`, `CHANGELOG.md` | 🟢 Direct mapping (for AegisGate scope) |

### 3.3 Clause 8 — Operation

| Clause | Requirement (summary) | AegisGate / customer mapping | Readiness |
|---|---|---|---|
| 8.1 | Operational planning and control | `ConfigService` versioning + `RolloutController` | 🟢 Direct mapping |
| 8.2 | AI risk assessment | Customer-owned; AegisGate metrics feed risk review | 🟡 Partial |
| 8.3 | AI risk treatment | `RolloutController` auto-rollback, rate limits, guardrails | 🟡 Partial |

### 3.4 Clause 9–10 — Performance Evaluation / Improvement

| Clause | Requirement (summary) | AegisGate / customer mapping | Readiness |
|---|---|---|---|
| 9.1 | Monitoring, measurement, analysis, evaluation | `MetricsRegistry` (Prometheus), `OpenTelemetry`, `Savings Dashboard` | 🟢 Direct mapping |
| 9.2 | Internal audit | `AuditLogger` chain_hash + `verifyChain()` | 🟢 Direct mapping |
| 9.3 | Management review | Customer-owned; AegisGate produces evidence | 🟡 Partial |
| 10.x | Continual improvement | `FeedbackBus` event stream + reflection workflow | 🟡 Partial |

### 3.5 PDCA Cycle Touchpoints

| PDCA stage | AegisGate primitives |
|---|---|
| **Plan** | `ConfigVersion` + `docs/specs/` + `docs/plans/` (this project's own /plan workflow models the practice) |
| **Do** | `RolloutController` canary deployment with scope matching |
| **Check** | `MetricsRegistry` + `RolloutMetricsProvider` + `Savings Dashboard` |
| **Act** | `RolloutController` auto-rollback + `FeedbackBus` continuous improvement loop |

---

## 4. China Generative AI Services Management Measures (Skeleton)

> 中文标题：《生成式人工智能服务管理暂行办法》(网信办、发改委、教育部、科技部、工信部、公安部、广电总局联合发布，2023-07-13；2023-08-15 施行)

| Article | Requirement (summary) | AegisGate / customer mapping | Readiness |
|---|---|---|---|
| Art. 4 | Adherence to core socialist values; non-discrimination; respect for IP and personal rights | Customer-owned content policy + `InjectionDetector` + `PIIFilter` | 🟡 Partial |
| Art. 7 | Training data legality, completeness, accuracy | Customer-owned data governance; AegisGate `AuditLogger` records data lineage events | ⚪ Customer (gateway out of training loop) |
| Art. 8 | Manual labeling rules; labeler training; cross-validation | Customer-owned; AegisGate does not perform labeling | ⚪ Customer |
| Art. 9 | Provider responsibilities; personal information protection agreement | RBAC + multi-tenant isolation + `PIIFilter` + audit chain | 🟡 Partial |
| Art. 10 | Service provider's responsibility for user input handling | `PIIFilter`, `InjectionDetector`, audit | 🟢 Direct mapping |
| Art. 11 | Service provider's content moderation obligations | `ExternalSafetyStage` (OpenAI Moderation, Perspective API), `InjectionDetector` | 🟢 Direct mapping |
| Art. 12 | Watermarking / labeling of generated content | Customer-implemented at application layer | ⚪ Out of scope for gateway |
| Art. 14 | Cooperation with regulatory supervision and inspection | `AuditLogger` chain_hash export, `aegisctl` admin CLI | 🟢 Direct mapping |
| Art. 15 | Service provider's responsibility for illegal content discovery and disposal | `ExternalSafetyStage` + `RolloutController` emergency pause | 🟡 Partial |
| Art. 17 | Security assessment for services with public-opinion or social-mobilization attributes | Customer-owned; AegisGate evidence channels available | 🟡 Partial |

---

## 5. Roadmap for Stage 2 / Stage 3 Expansion

| Framework | Stage | Scope |
|---|---|---|
| **NIST AI RMF 1.0** | Stage 2 | Govern / Map / Measure / Manage function mappings |
| **SOC 2 Type II** | Stage 2 | TSC 100 control mapping (CC, A, C, PI, P series) |
| **GDPR Articles 22 + 25 + 32** | Stage 2 | Automated decision-making, data protection by design, security of processing |
| **HIPAA Security Rule (45 CFR §164.308–§164.314)** | Stage 2 (healthcare pack) | Administrative / Physical / Technical safeguards |
| **PBOC AI Applications Guidance** | Stage 2 (finance pack) | China banking-sector AI guidance overlay |
| **PCI-DSS v4.0** | Stage 2 (finance pack) | Payment industry overlay |
| **等保 2.0 (China MLPS 2.0)** | Stage 2 (China deployments) | Information system protection grading |

Each Stage 2 mapping will produce:

- Detailed clause table (every clause / control / safeguard mapped to a
  specific AegisGate component or customer-owned action)
- Evidence collection runbook (which audit log queries / metric queries
  satisfy each control)
- Auditor-ready report template (PDF/HTML)

---

## 6. How to Use This Skeleton

### 6.1 For Internal Product / Sales Conversations

Use the readiness columns to set realistic customer expectations. **Never
overstate** to "compliant" or "certified" — instead say "AegisGate provides
the technical controls for clauses X, Y, Z; your team is responsible for the
management-system clauses (4.1, 4.2, 5.x, 7.1, etc.)".

### 6.2 For Customer Pre-Sales

Share this document early. Customers in regulated industries appreciate
honest readiness mapping more than aspirational marketing claims.

### 6.3 For Contributors

If you add a new AegisGate capability that addresses a specific regulatory
clause, please:

1. Update the corresponding clause row in §2 / §3 / §4 above
2. Cite the source file path (e.g., `src/server/audit_logger.cpp` line range)
3. Open a PR labeled `compliance-mapping`
4. Update the readiness level if it changes (🔵 → 🟡 → 🟢)

### 6.4 For Auditors

This document is a **starting point** for an audit engagement, not a
substitute for one. Stage 2 / TASK-W2 deliverables will include:

- Sample audit log exports demonstrating chain hash integrity
- Sample rollout history demonstrating auto-rollback
- Sample two-person approval workflow evidence
- Per-clause control evidence checklists

---

## See Also

- [AegisOps Vision](../positioning/aegisops-vision.md) — Why we built this
- [Roadmap v4](../ROADMAP.md) — Stage 2 / Stage 3 compliance pack
  milestones
- [Architecture guide](../guides/architecture.md) — overall architecture
  including audit chain
- [Control Plane guide](../guides/control-plane.md) — versioning and
  two-person approval workflow
