# AegisGate Roadmap v2.0

This document describes the v2.0 strategic direction and feature roadmap of
AegisGate. Phase 0-4 were all completed on 2026-03-26; this roadmap focuses on
the strategic shift from "feature building" to "production validation → platform
evolution → ecosystem expansion".

> Last updated: 2026-03-30
> Prior roadmap: the Phase 0-4 completion record is in the appendix at the end
> 中文版：[`ROADMAP_zh.md`](ROADMAP_zh.md)

---

## Strategic positioning

### Vision

**Become the de-facto standard for high-performance AI gateways** — in a
landscape where LiteLLM (Python) dominates ease of use and Portkey (SaaS) leads
the managed market, AegisGate claims the top spot for self-hosted
high-performance AI gateways with its differentiation triangle: C++ native
performance + built-in security guardrails + zero-external-dependency deployment.

### Core strategy: from building to validating, from single-node to platform

```
Phase 0-4 ✅            v2.0 new strategy
┌──────────────┐      ┌──────────────────────────────────────────────┐
│  Feature      │  →   │  Phase 5  Production validation & GA release   │
│  building     │      │  Phase 6  Platform evolution (multimodal +     │
│  (13 days)    │      │           external integrations)               │
│              │      │  Phase 7  Ecosystem & community                 │
│              │      │  Phase 8  Next-gen intelligence (RAG + Agent)    │
│              │      │  Phase 9  Global-scale deployment                │
└──────────────┘      └──────────────────────────────────────────────┘
```

### Differentiation triangle

| Dimension | AegisGate advantage | Competitor gap |
|------|---------------|---------|
| **Performance** | 47μs pipeline latency (C++ native), 16-shard lock concurrency | LiteLLM: Python GIL limits; Portkey: SaaS network overhead |
| **Security** | Three-layer guardrails built in (regex → rules → ONNX), Unicode normalization + encoding detection | LiteLLM: no built-in guardrails; Kong: plugin-level security |
| **Deployment** | Community single binary with zero external dependencies; Enterprise uses the same image via Feature Gate | Portkey: SaaS only; LiteLLM: requires a Python runtime |

---

## Edition model

AegisGate uses an **Open-Core** model:

- **Community** — fully open source, feature-complete, no artificial limits, ideal for developers and small teams
- **Enterprise** — adds scale operations, compliance management, multi-tenancy and other enterprise-grade capabilities on top of Community; requires License activation

A single binary controls feature availability at runtime through Feature Gate + License.

---

## Feature matrix

### Unified AI gateway

| Feature | Community | Enterprise |
|------|:------:|:------:|
| Unified API (OpenAI / Claude / DeepSeek / Doubao / Qwen / Gemini / Mistral) | ✅ | ✅ |
| Full-chain Function Calling / Tool Use support | ✅ | ✅ |
| ConnectorFactory zero-code new providers | ✅ | ✅ |
| Basic routing (pinned model / cost-aware) | ✅ | ✅ |
| Fallback degradation + load balancing | ✅ | ✅ |
| SSE streaming + double buffering | ✅ | ✅ |
| Token-bucket rate limiting (per-key) | ✅ | ✅ |
| Token optimization (compression + smart max_tokens) | ✅ | ✅ |
| Semantic cache (LRU + hnswlib) | ✅ | ✅ |
| Multimodal API proxy (embedding / image / audio) | ✅ ᴺᴱᵂ | ✅ ᴺᴱᵂ |
| ML-assisted advanced routing (cost/quality/latency optimization) | — | ✅ |
| External vector DB integration (Milvus / Qdrant) | — | ✅ ᴺᴱᵂ |

### Security guardrails

| Feature | Community | Enterprise |
|------|:------:|:------:|
| Prompt injection detection (regex + optional ONNX) | ✅ | ✅ |
| PII redaction (RE2) | ✅ | ✅ |
| Topic boundaries (allowlist / blocklist) | ✅ | ✅ |
| Outbound content filtering | ✅ | ✅ |
| Hallucination detection | ✅ | ✅ |
| Function Calling tool-call security audit | ✅ | ✅ |
| Audit log (file + chained hash) | ✅ | ✅ |
| External safety API integration (OpenAI Moderation / Perspective) | — | ✅ ᴺᴱᵂ |
| Custom rule engine (declarative definitions / version management) | — | ✅ |
| Compliance reports (audit export / retention policy) | — | ✅ |

### Management & operations

| Feature | Community | Enterprise |
|------|:------:|:------:|
| CLI management tool (aegisctl) | ✅ | ✅ |
| Prometheus metrics endpoint | ✅ | ✅ |
| Cost tracking | ✅ | ✅ |
| CHANGELOG + semantic versioning | ✅ ᴺᴱᵂ | ✅ ᴺᴱᵂ |
| Web admin panel (dashboard / model management / cost analysis) | — | ✅ |
| Advanced alerting (Webhook / DingTalk / Feishu) | — | ✅ |

### Access control & deployment

| Feature | Community | Enterprise |
|------|:------:|:------:|
| API key authentication | ✅ | ✅ |
| SQLite storage | ✅ | ✅ |
| Single-node Docker deployment | ✅ | ✅ |
| RBAC (role-based access control) | — | ✅ |
| Multi-tenant isolation | — | ✅ |
| SSO (OIDC / OAuth2) | — | ✅ |
| PostgreSQL + Redis | — | ✅ |
| Cluster deployment + horizontal scaling | — | ✅ |

### Client SDKs

| SDK | Community | Enterprise |
|-----|:------:|:------:|
| Python (httpx) | ✅ | ✅ |
| Node.js (native fetch) | ✅ | ✅ |
| Go (stdlib) | ✅ | ✅ |
| Java / Kotlin | ᴺᴱᵂ | ᴺᴱᵂ |
| Rust | ᴺᴱᵂ | ᴺᴱᵂ |

---

## Phase 5: Production validation & GA release

> From "it works" to "it's trustworthy". Anchored on releasing v1.0 GA:
> establish API stability commitments, complete the CHANGELOG, and finish
> production-environment validation.

### 5.1 API stability & version governance

| Item | Content | Deliverable |
|------|------|--------|
| API versioning policy | Establish the `/v1/` prefix semantic-versioning commitment; breaking changes must keep two versions coexisting for one major cycle | Versioning policy doc |
| CHANGELOG backfill | Trace all feature changes from v0.1.0 → current; establish a Keep a Changelog process | Complete `CHANGELOG.md` |
| Deprecation policy | deprecated annotations + migration guides + at least two minor-version buffer | Deprecation policy doc |
| SDK backward compatibility | SDK versions aligned with API versions; a major SDK version pins an API version | SDK version matrix |

### 5.2 Production-environment validation

| Item | Content | Success criteria |
|------|------|---------|
| Canary deployment plan | Onboard 1-2 internal real AI application scenarios to AegisGate | 7 days of real traffic with no P0 |
| Long-running stress test | 72-hour continuous load: mixed read/write + streaming/non-streaming + fault injection | Zero memory leaks, zero crashes, P99 < 100ms |
| Chaos engineering | Upstream disconnect / Redis down / disk full / OOM boundary / high-concurrency key rotation | Graceful degradation in all scenarios, no dropped requests |
| Security penetration testing | Automated security scans (OWASP ZAP / Nuclei) + manual pen testing | Zero high/critical vulnerabilities |
| Memory-safety focus | ASAN + UBSAN + TSAN 24-hour runs + Valgrind memcheck | Zero ASAN/TSAN reports |

### 5.3 CI/CD completion

| Item | Content | Value |
|------|------|------|
| OTEL ON CI normalization | Add the OpenTelemetry conditional-compilation path to the CI matrix | Clear a P1 tech-debt item |
| Helm Chart integration tests | kind cluster + Helm install + smoke test in CI | Deployment reliability validation |
| Release automation | Tag-triggered multi-arch Docker image builds + GitHub Release | Release efficiency |
| Benchmark regression gate | Run core-path benchmarks in PR gates; block on >10% performance regression | Performance safety net |

### 5.4 v1.0 GA release checklist

| Item | Content |
|------|------|
| Semantic version | v1.0.0 official release |
| Complete CHANGELOG | All changes v0.1.0 → v1.0.0 |
| API stability statement | `/v1/` endpoint backward-compatibility commitment |
| Security advisory process | `SECURITY.md` + vulnerability reporting + response SLA |
| License terms | Community Apache 2.0 / Enterprise commercial License terms clarified |
| Official Docker image | `ghcr.io/privonyx/loong-aegisgate:1.0.0` |
| Upgrade guide | v0.x → v1.0 migration doc |

---

## Phase 6: Platform evolution ✅ Completed (2026-05-15, TASK-20260513-01)

> Evolve from "LLM chat proxy" to "full-modality AI platform gateway". Covers
> embedding, image, audio and other multimodal endpoints, and integrates
> external vector databases and safety APIs.
>
> **Completion evidence:** ctest CP+ON 175/175 + vitest 32/32.

### 6.1 Multimodal API proxy ✅ Completed

| Endpoint | Content | Value |
|------|------|------|
| `/v1/embeddings` | Unified embedding API (OpenAI / Cohere / local ONNX), auto-routed to the optimal model | Covers RAG scenarios |
| `/v1/images/generations` | Unified image generation (DALL·E / Stability / Midjourney API) | Multimodal apps |
| `/v1/audio/transcriptions` | Unified transcription (Whisper / Azure Speech) | Voice scenarios |
| `/v1/audio/speech` | Unified speech synthesis (OpenAI TTS / Azure / ElevenLabs) | Voice scenarios |
| `/v1/moderations` | Content-moderation endpoint, unifying OpenAI Moderation / Google Perspective | Security & compliance |

**Architecture highlights:**
- Each modality implements an independent `ModalityHandler` interface (CR2 scheme A — thin Handler + thick Router)
- Reuses the existing pipeline (auth → rate limit → guardrails → routing → audit)
- Per-modality cost tracking and quota management (`CostTracker.modality` + `ModalityRateLimiter`)
- Input/output size limits (image/audio file-size thresholds)

### 6.2 External vector database integration ✅ Completed

| Item | Content | Value |
|------|------|------|
| VectorStore abstraction | `VectorStore::insert()/search()/delete()`, decoupled from hnswlib | Extensibility foundation |
| Milvus adapter | gRPC client with partition and index management | Million-scale vectors |
| Qdrant adapter | REST/gRPC client with payload filtering | Million-scale vectors |
| Dual-mode operation | Community hnswlib (in-process) / Enterprise Milvus/Qdrant | Scale on demand |
| Cache migration tool | hnswlib → Milvus/Qdrant **offline dump+restore CLI** (D5=B), with SHA-256 + tenant allowlist + API key triple security gate | Smooth upgrade |

### 6.3 External safety API integration ✅ Completed

| Item | Content | Value |
|------|------|------|
| L4 external safety layer | Adds a 4th security layer to the architecture: external API calls | Defense in depth |
| OpenAI Moderation | Integrates the `/v1/moderations` endpoint, async fire-and-forget or sync blocking | Semantic-level safety |
| Google Perspective | Perspective API toxicity/threat/insult scoring | Multi-dimensional safety |
| Async mode | Shadow analysis mode (non-blocking, async recording), configurable to switch to sync blocking — delivered in Epic 4, measured process() < 10ms with a 500ms provider | Latency/safety balance |
| Failure policy | fail-open + degrade to L1/L2/L3 when the external API is unavailable | Availability guarantee |
| Shadow audit + backpressure | **SR3** every shadow scan must write an audit entry; **SR6** `shadow_max_inflight` atomic counter prevents worker pile-up | Observability + prevents cloud-API jitter from amplifying into internal resource exhaustion |

### 6.4 Multi-turn conversation cache upgrade ✅ Completed

| Item | Content | Value |
|------|------|------|
| Conversation-summary cache key | `SHA-256(tenant_id) + SHA-256(conversation_id) + summary` partition_key V2 (**SR1 cross-tenant hard isolation**) | Avoids false hits across different contexts + multi-tenant safety |
| Lightweight summarization | `CompositeSummarizer`: ONNX primary + RuleBased fallback (CR1 scheme B decorator, auto-fallback on empty primary + `fallback_count_` reporting) | Zero external deps + runtime fault tolerance |
| Conversation ID association | Supports `metadata.conversation_id` (D2=C client priority + server-side SHA-256 history derivation + request_id fallback) | Multi-turn scenarios |
| Cache eviction policy | `ConversationCacheEvictor` 4-factor scoring (frequency / recency / size / TTL) | Cache quality |

---

## Phase 7: Ecosystem & community

> From "technical product" to "developer ecosystem". Lower the contribution
> barrier, build community infrastructure, and improve docs and SDKs.

### 7.1 Community infrastructure

| Item | Content | Value |
|------|------|------|
| GitHub Discussions | Enable Q&A / Ideas / Show and Tell sections | Community exchange |
| Discord channels | Real-time developer chat + English/Chinese channels | Community stickiness |
| Good First Issues | Tag 20+ beginner-friendly issues (docs/tests/small features) | Lower the contribution barrier |
| CONTRIBUTING enhancements | One-click dev-environment script + architecture tour + common-task cookbook | Contributor-friendly |
| Contributor agreements | Contributor License Agreement (CLA) + Code of Conduct | Open-source governance |
| Community governance | Tiered maintainer permissions + PR review process + release approval | Sustainable operation |

### 7.2 Documentation internationalization

| Item | Content | Value |
|------|------|------|
| Core docs in English | Quick start, architecture overview, API reference, deployment guide | International community |
| Auto-generated API reference | Doxygen/Sphinx generate API docs from code comments | Doc consistency |
| Tutorial series | 5-minute quick start / security best practices / performance tuning / production deployment / SDK integration | Adoption |
| Interactive playground | Online demo environment, try it without local installation | Lower the trial barrier |

### 7.3 SDK productionization

| SDK | Enhancements | Value |
|-----|---------|------|
| **Common** | Exponential-backoff retries + configurable timeouts + connection-pool reuse + OpenTelemetry trace injection | Production reliability |
| **Python** | Native async/await + Pydantic type models + streaming iterator | Python ecosystem fit |
| **Node.js** | TypeScript type safety + streaming ReadableStream + auto-reconnect | TS ecosystem fit |
| **Go** | context.Context propagation + structured errors + streaming io.Reader | Go ecosystem fit |
| **Java/Kotlin** ᴺᴱᵂ | OkHttp/Ktor client + Kotlin coroutines + Spring Boot starter | JVM ecosystem coverage |
| **Rust** ᴺᴱᵂ | reqwest + tokio async + serde serialization + tracing integration | Rust ecosystem coverage |

### 7.4 Competitive benchmarking

| Item | Content | Value |
|------|------|------|
| Standardized benchmark suite | Unified test scenarios (single-request latency / concurrent throughput / streaming first token / cache hit) | Fair comparison |
| vs LiteLLM | Single-node throughput, latency percentiles, memory footprint comparison | Performance differentiation |
| vs Portkey | Feature coverage + security capabilities + self-hosting flexibility comparison | Scenario differentiation |
| Comparison report | Published in official docs and blog | Market awareness |

---

## Phase 8: Next-generation intelligence

> From "AI proxy gateway" to "AI application infrastructure". Support agent
> orchestration, RAG pipeline integration, and intelligent caching, providing
> infrastructure-level support for complex AI applications.

### 8.1 Agent orchestration foundation

| Item | Content | Value |
|------|------|------|
| Tool Registry | Register and manage the tool set callable by the AI, with versioning | Agent foundation |
| Tool execution sandbox | Secure isolated execution environment for tool calls (syscall filtering + timeout + resource limits) | Agent safety |
| Multi-step orchestration | Multi-step tool-call chains supporting ReAct / Plan-and-Execute patterns | Complex agents |
| Tool-call audit | Full record of input/output/duration/cost for each tool call | Traceability |
| Per-tool rate limiting | per-tool rate limits and concurrency control | Safety protection |

### 8.2 RAG pipeline integration

| Item | Content | Value |
|------|------|------|
| Knowledge base management | Document upload → chunking → embedding → vector-store management API | RAG foundation |
| Retrieval-augmentation stage | New `RetrievalStage`: retrieve relevant knowledge on request arrival and inject into context | RAG pipeline |
| Enhanced hallucination detection | Use retrieved facts as ground truth against LLM output | Higher trustworthiness |
| Citation tracking | Annotate output with information sources (which chunk of which document) | Verifiability |

### 8.3 Intelligent caching 2.0

| Item | Content | Value |
|------|------|------|
| Semantic-compression cache | Semantically compress long responses for storage, expand on demand at hit time | Cache capacity |
| Predictive cache warming | Predict popular queries from historical patterns and pre-fill the cache | Hit rate |
| Cross-tenant anonymous cache | A general-knowledge cache layer with tenant identity removed, shared where the security policy allows | Global efficiency |
| Cache quality feedback | User satisfaction feedback on cache hits, dynamically adjusting thresholds | Adaptivity |

### 8.4 Advanced observability

| Item | Content | Value |
|------|------|------|
| AI-call cost attribution | Cost attribution analysis by app / feature module / user granularity | Cost optimization |
| Anomaly detection | Statistical-model-based anomalous request-pattern detection (spike/drop/abnormal distribution) | Proactive ops |
| Quality trend monitoring | Trend tracking and alerting on model output quality over time | Model-degradation early warning |
| Cost-optimization recommendations | Auto-recommend optimal routing strategies and model combinations from usage patterns | Intelligent ops |

---

## Phase 9: Global-scale deployment

> From "single cluster" to "globally distributed". Support multi-region
> deployment, edge caching, and compliant data residency.

### 9.1 Multi-region deployment

| Item | Content | Value |
|------|------|------|
| Region-aware routing | Route requests to the lowest-latency regional AI provider endpoint | Latency optimization |
| Cross-region cache sync | Async replication of the semantic cache across regions; hot data available globally | Global hit rate |
| Data residency policy | Configure per-tenant data-residency regions (audit logs, cache data stay in-region) | Compliance requirements |
| Multi-cluster federation | Central control plane + regional data plane architecture | Global scale |

### 9.2 Edge deployment

| Item | Content | Value |
|------|------|------|
| Lightweight edge nodes | Slimmed AegisGate binary (auth + rate limit + cache + routing only) | Edge latency |
| Edge caching | Semantic cache hits at the CDN layer | Ultra-low latency |
| Edge security | Run basic security checks at the edge, send suspicious requests back to origin for deep inspection | Security + performance |

### 9.3 Advanced compliance

| Item | Content | Value |
|------|------|------|
| SOC 2 compliance framework | Security-control mapping + evidence-collection automation | Enterprise compliance |
| GDPR support | Data-deletion API + right-of-access requests + consent management | European market |
| ISO 27001 alignment | Information-security management-system control mapping | International certification |
| Automated audit reports | Scheduled compliance-report generation + automatic anomaly notification | Compliance efficiency |

---

## Phase overview

```
Phase 0-4 ✅ Completed    Feature-building foundation
   │
Phase 5  Production + GA   API stability + production validation + CI/CD + v1.0 release
   │
Phase 6  Platform evolution   Multimodal + external vector DB + safety API + cache upgrade
   │
Phase 7  Ecosystem & community   Community infra + doc i18n + SDK enhancements + benchmarking
   │
Phase 8  Next-gen intelligence   Agent orchestration + RAG integration + smart cache + advanced observability
   │
Phase 9  Global scale       Multi-region + edge deployment + advanced compliance
```

### Complexity & tech-stack requirements

| Phase | Core skills | Invasiveness |
|-------|-------------|--------|
| Phase 5 | CI/CD engineering, stress testing, security auditing | Low (mostly docs and config) |
| Phase 6 | gRPC clients, multimodal APIs, vector databases | Medium (new modules, reuses pipeline) |
| Phase 7 | Technical writing, multi-language SDKs, community ops | Low (mostly docs and SDKs) |
| Phase 8 | Agent orchestration, RAG architecture, ML engineering | High (core pipeline extension) |
| Phase 9 | Distributed systems, global networking, compliance engineering | High (architecture-level changes) |

---

## Key milestones

| Milestone | Status | Signature achievement |
|--------|------|----------|
| v0.1.0 — First release | ✅ Done | Unified AI gateway + security guardrails + semantic cache + SDK |
| v0.2.0 — Multi-provider | ✅ Done | DeepSeek/Doubao + ConnectorFactory refactor |
| v0.3.0 — Production ready | ✅ Done | Tech debt cleared + Docker + graceful shutdown + chaos tests |
| v0.4.0 — Community evolution | ✅ Done | OpenTelemetry + security upgrades + cache evolution + developer experience |
| v0.5.0 — Enterprise foundation | ✅ Done | RBAC + multi-tenancy + web admin panel |
| v0.6.0 — Enterprise advanced | ✅ Done | SSO + rule engine + ML routing + A/B testing + quality scoring |
| v0.7.0 — Scale | ✅ Done | Cluster deployment + plugin system + rule marketplace |
| v0.8.0 — Security hardening | ✅ Done | Ed25519 License + audit encryption + IP allowlist |
| v0.9.0 — AI enhancements | ✅ Done | Function Calling + token optimization system |
| **v1.0.0 GA — Official release** | **✅ Done** | **API stability commitment + release automation + agent orchestration + RAG + intelligent cache 2.0 + advanced observability** |
| **v1.3 — Community enhancements** | **✅ Done** | **5-language SDKs (+Java +Rust) + competitive benchmark report + community governance** |
| v2.1 — Global deployment | 🔜 Planned | Multi-region routing + cross-region cache sync + compliance framework |

> For detailed version change records see [CHANGELOG.md](../CHANGELOG.md); for the API stability policy see [VERSIONING.md](../VERSIONING.md).

## Resource estimates

| Phase | Estimated effort | Core skills |
|-------|---------|-------------|
| Phase 0 — Foundation | 4-5 weeks | C++ concurrent programming, containerization |
| Phase 1 — Community evolution | 8-10 weeks | OpenTelemetry, ML model inference, security engineering |
| Phase 2 — Enterprise foundation | 9-12 weeks | C++ backend + React/TypeScript frontend |
| Phase 3 — Enterprise advanced | 6-8 weeks | OAuth2/OIDC protocols, data analytics |
| Phase 4 — Scale & ecosystem | 10-14 weeks | Distributed systems, K8s, plugin architecture |

---

## Dependencies

```
Phase 5.1 API stability ──────→ Phase 5.4 v1.0 GA
Phase 5.2 Production validation ──────→ Phase 5.4 v1.0 GA
Phase 5.3 CI/CD completion ──────→ Phase 5.4 v1.0 GA
                                    │
              ┌─────────────────────┼─────────────────────┐
              ↓                     ↓                     ↓
Phase 6.1 Multimodal API    Phase 6.2 External vector DB   Phase 6.3 External safety API
Phase 6.4 Conversation cache upgrade │
              │                     │
              ↓                     ↓
       Phase 7.1-7.4          Phase 8.1 Agent orchestration
       Ecosystem & community  Phase 8.2 RAG integration
                               Phase 8.3 Intelligent cache 2.0
                                    │
                                    ↓
                              Phase 9.1-9.3
                              Global-scale deployment
```

---

## Technical risks & mitigations

| Risk | Impact | Mitigation |
|------|------|---------|
| High C++ contribution barrier | Limited ecosystem growth | Accept contributions in other languages for SDK/docs/CLI; recruit core C++ maintainers |
| Fast-changing multimodal APIs | Maintenance cost | Abstract the ModalityHandler interface so provider changes don't affect the core |
| Fragmented vector-DB ecosystem | Adaptation cost | VectorStore abstraction; adapt the Top 2 first by user demand |
| Agent security risk | Tool-call abuse | Triple protection: sandbox isolation + approval mechanism + resource limits |
| Global-deployment network complexity | Operational difficulty | Validate single-region cluster first, then progressive multi-region |

---

## Appendix: Phase 0-4 completion record

| Phase | Completion date | Core deliverables |
|-------|---------|---------|
| Phase 0 Foundation | 2026-03-21 | Tech debt cleared + Docker + chaos tests |
| Phase 1 Community evolution | 2026-03-22 | OTEL tracing + security upgrades + cache evolution + developer experience |
| Phase 2 Enterprise foundation | 2026-03-22 | RBAC multi-tenancy + web admin panel |
| Phase 3 Enterprise advanced | 2026-03-26 | SSO + rule engine + ML routing + A/B testing + quality scoring + usage forecasting |
| Phase 4.1 Cluster deployment | 2026-03-26 | RedisStateStore + Helm Chart + docker-compose cluster |
| Phase 4.2 Ecosystem | 2026-03-26 | Plugin system (dlopen .so) + rule marketplace (aegisctl rules) |
| Add-on: Token optimization | 2026-03-28 | PromptCompressor + SmartMaxTokens + TokenEstimator |
| Add-on: Security guarantees | 2026-03-29 | Ed25519 License + audit chain hash + audit encryption + IP allowlist |
| Add-on: Function Calling | 2026-03-30 | Full-chain Tool Use support (type system + connectors + Runtime + guardrails + cache) |

**96/96 tests passing.** Source: 171 files ~29,000 lines of C++; tests: 100 files ~16,000 lines.

---

## Contributing

Community contributions are welcome! See [CONTRIBUTING.md](../CONTRIBUTING.md) for the development process.

Community-related issues and PRs are prioritized. Design discussions for Enterprise features are also welcome.

---

## What's next

This document records the strategy and delivery of Phase 5-8. The subsequent roadmap splits into two parallel tracks:

- **Open-source core engineering track**: globalization × platformization × intelligence (Phase 9-14)
- **Product / commercialization track**: brand upgrade → industry compliance packs → SaaS control plane

Both tracks coexist: engineering and the commercialization layer do not replace each other.
