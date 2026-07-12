[English](feature-list.md) | [中文](feature-list_zh.md)

# AegisGate Feature List

> **Audience:** product selection, capability inventory, external/internal pitching, and RFP responses.
> **Baseline:** v1.0.0 GA (single binary; runtime Feature Gate decides Community vs. Enterprise capabilities).
> **Sources of truth:** `README.md` Features section, [Architecture guide](guides/architecture.md), `docs/guides/*.md`, [OpenAPI](openapi.yaml), [OpenAI compatibility matrix](openai-compat-matrix.md), [Roadmap](ROADMAP.md).

## Legend

| Symbol | Meaning |
|:------:|---------|
| 🟢 Community | Apache-2.0 open source; on by default in the single binary |
| 🔵 Enterprise | Requires a runtime Feature Gate license |
| ⚪ Both editions | Available in both editions (same implementation, no Feature Gate) |
| 🧪 Preview | Available but still being polished (preview, API may shift) |
| 📐 Planned | Roadmap item, not yet GA |

Key invariants: **single OpenAI-compatible endpoint** + **single binary** + **config-driven** (one YAML flips every capability).

---

## Table of contents

1. [Unified AI Gateway](#1-unified-ai-gateway)
2. [Token Optimization](#2-token-optimization)
3. [Security Guardrails](#3-security-guardrails)
4. [Semantic Cache](#4-semantic-cache)
5. [Observability](#5-observability)
6. [Storage Abstraction](#6-storage-abstraction)
7. [Multi-Tenancy · RBAC · AuthN](#7-multi-tenancy--rbac--authn-enterprise)
8. [Control Plane](#8-control-plane)
9. [Cost Autonomy & AegisOps](#9-cost-autonomy--aegisops)
10. [Deployment & Cluster](#10-deployment--cluster)
11. [Plugins & Ecosystem](#11-plugins--ecosystem)
12. [Client SDKs](#12-client-sdks)
13. [CLI Tooling](#13-cli-tooling)
14. [API Endpoint Index](#14-api-endpoint-index)
15. [Edition Capability Matrix](#15-edition-capability-matrix)

---

## 1. Unified AI Gateway

A single `/v1/chat/completions` endpoint that unifies OpenAI-compatible protocol access to 7 mainstream LLM vendors plus any OpenAI-compatible backend.

| Capability | Description | Edition | Endpoint / config | Guide |
|------------|-------------|:-------:|-------------------|-------|
| OpenAI-compatible Chat Completions | Single `POST /v1/chat/completions`; request/response fully aligned with OpenAI; `stream=true` for chunk-level SSE. | ⚪ | `/v1/chat/completions` · `providers.*` | [Architecture](guides/architecture.md) · [Usage examples](guides/usage-examples.md) |
| Built-in provider connectors | OpenAI / Anthropic Claude / DeepSeek / Doubao / Qwen / Google Gemini / Mistral shipped out of the box; Claude flows through OpenAI ↔ Anthropic bidirectional translation. | ⚪ | `api/providers/definitions/*.yaml` | [OpenAI compat matrix](openai-compat-matrix.md) · [Provider manifest](guides/provider-manifest.md) |
| Any OpenAI-compatible backend | Register third-party model endpoints (local inference, private deployments, proxy layers) via declarative Manifests. | ⚪ | `api/providers/definitions/<name>.yaml` | [Provider manifest](guides/provider-manifest.md) |
| Routing · BasicRouter | Explicit `model` → tag → default model; no ML dependency. | 🟢 | `routing.type: basic` | [Architecture · Routing](guides/architecture.md) |
| Routing · CostAwareRouter | Bucketed by prompt char count (economy / standard / premium); cost-aware. | ⚪ | `routing.type: cost_aware` | [Cost optimization](guides/cost-optimization.md) |
| Routing · MLRouter | 3-dimensional weighted scoring (cost × quality × latency), EMA-updated; weights configurable. | 🔵 | `routing.type: ml` | [Architecture · MLRouter](guides/architecture.md) · [Cost optimization](guides/cost-optimization.md) |
| A/B routing (ABTestRouter) | Decorator wrapping any Router; `hash(experiment + request_id) % weight` deterministic traffic split. | 🔵 | `routing.ab_tests[]` | [Architecture · ABTestRouter](guides/architecture.md) |
| Geo routing (GeoRouter) | Decorator routing by client geography to nearest regional backend. | 🔵 | `routing.geo.*` | [Multi-region](guides/multi-region.md) |
| API key load balancing | Multi-key pool rotation per model (round-robin / random); quotas isolated, failures ring-fenced. | ⚪ | `providers.<name>.api_keys[]` | [Architecture](guides/architecture.md) |
| Auto fallback + circuit breaker | FallbackManager primary/backup + CircuitBreaker (Closed → Open → HalfOpen); thresholds / timeouts configurable. | ⚪ | `providers.<name>.fallback` · `circuit_breaker.*` | [Architecture · Fallback](guides/architecture.md) |
| Token-bucket rate limiting | Per-API-key / per-tenant token-bucket quotas (`rate_limit.max_tokens` + `rate_limit.refill_rate`; callers may charge by request count or token cost). Redis-backed cluster quotas. | ⚪ | `rate_limit.*` | [Architecture](guides/architecture.md) · [Security best practices](guides/security-best-practices.md) |
| SSE streaming (true per-chunk) | Each chunk independently observable; compatible with outbound guardrails, cost accounting, and cache write-back; `stream: true`. | ⚪ | `/v1/chat/completions` `stream=true` | [Usage examples](guides/usage-examples.md) |
| Function Calling / Tool Use | Bidirectional translation between OpenAI `tools` / `tool_calls` and Anthropic `tool_use` / `tool_result`. | ⚪ | `tools[]` / `tool_choice` | [OpenAI compat matrix](openai-compat-matrix.md) |
| Multi-modal proxy · Embeddings | `POST /v1/embeddings` (text → vector). | ⚪ | `/v1/embeddings` | [Multi-modal](guides/multimodal.md) |
| Multi-modal proxy · Images | `POST /v1/images/generations`. | ⚪ | `/v1/images/generations` | [Multi-modal](guides/multimodal.md) |
| Multi-modal proxy · ASR | `POST /v1/audio/transcriptions` (speech-to-text). | ⚪ | `/v1/audio/transcriptions` | [Multi-modal](guides/multimodal.md) |
| Multi-modal proxy · Audio translation | `POST /v1/audio/translations`. | ⚪ | `/v1/audio/translations` | [Multi-modal](guides/multimodal.md) |
| Multi-modal proxy · TTS | `POST /v1/audio/speech` (text-to-speech). | ⚪ | `/v1/audio/speech` | [Multi-modal](guides/multimodal.md) |
| Models listing | `GET /v1/models` (OpenAI-compatible). | ⚪ | `/v1/models` | [OpenAPI](openapi.yaml) |

---

## 2. Token Optimization

Compress prompts and precise-size `max_tokens` without changing business semantics; surface the savings explicitly to the application.

| Capability | Description | Edition | Config / endpoint | Guide |
|------------|-------------|:-------:|-------------------|-------|
| Prompt compression · context truncation | Preserve system messages and the most recent window; safely drop older history. | ⚪ | `token_optimization.prompt_compression.max_context_messages` | [Cost optimization](guides/cost-optimization.md) |
| Prompt compression · whitespace normalize | Collapse redundant whitespace, newlines, and indentation. | ⚪ | `token_optimization.prompt_compression.compress_whitespace: true` | [Cost optimization](guides/cost-optimization.md) |
| Prompt compression · message dedup | Merge adjacent same-role duplicates. | ⚪ | `token_optimization.prompt_compression.dedup_system_prompts: true` | [Cost optimization](guides/cost-optimization.md) |
| Automatic `max_tokens` | Dynamically compute safe upper bound from model context minus input tokens; avoids upstream rejection or truncation. | ⚪ | `token_optimization.smart_max_tokens.enabled: true` | [Cost optimization](guides/cost-optimization.md) |
| CJK-aware token estimation | Approximate tokenizer optimized for Chinese/Japanese/Korean; avoids word-count underestimation. | ⚪ | Built-in (no config) | [Cost optimization](guides/cost-optimization.md) |
| Savings visibility · HTTP header | Each response includes `X-AegisGate-Tokens-Saved`. | ⚪ | Built-in | [Usage examples](guides/usage-examples.md) |
| Savings visibility · SSE metadata | Streaming mode attaches savings metadata in the final chunk. | ⚪ | Built-in | [Usage examples](guides/usage-examples.md) |
| Savings summary API | `GET /admin/api/savings/summary` aggregates savings stats. | 🔵 | `/admin/api/savings/*` | [Savings dashboard](guides/admin-savings.md) |
| Savings estimation CLI | `aegisctl estimate` estimates savings from historical logs post-migration. | ⚪ | `aegisctl estimate` | [Savings estimate](estimate.md) |

---

## 3. Security Guardrails

Multi-layer defense in depth: prompt injection, PII leakage, content compliance, hallucination detection, and tamper-evident auditing.

### 3.1 Inbound guardrails (Inbound Pipeline · 8 stages)

| Stage / capability | Description | Edition | Config | Guide |
|--------------------|-------------|:-------:|--------|-------|
| ① AuditLogger (inbound) | Records requests before persistence; tamper-evident hash chain (FNV-1a) + AES-256-GCM detail encryption. | ⚪ | `audit.*` | [Security best practices](guides/security-best-practices.md) |
| ② Preprocessor · NFKC normalize | Unicode NFKC + zero-width character (ZWSP/ZWNJ/ZWJ) stripping; blocks homoglyph / full-width bypass. | ⚪ | `security.unicode_normalization: true` | [Architecture · Guardrail layers](guides/architecture.md) |
| ③ Injection Detection · L1 keywords | CJK / Cyrillic / English multi-language keywords and heuristics (role switching, special-char density). | ⚪ | `security.encoding_detection: true` (L1 keywords built-in) | [Security best practices](guides/security-best-practices.md) |
| ③ Injection Detection · L2 encoding | Base64 / Hex / URL decode-and-rescan. | ⚪ | `security.encoding_detection: true` · `security.encoding_min_base64_length` | [Security best practices](guides/security-best-practices.md) |
| ④ GuardModel · L3 ONNX classifier | Optional neural safety classifier (~5ms); yields confidence + explanation. | 🔵 | `security.guard_model.model_path` | [Guard model](guides/guard-model.md) · [Adaptive guard](guides/adaptive-guard.md) |
| ⑤ PII Filter | RE2 linear-time regex (ReDoS-proof): phone / email / national ID / bank card / API key / private key; mask or reject configurable. | ⚪ | `— (inbound/outbound stage; no top-level YAML root; see guide)` | [Security best practices](guides/security-best-practices.md) |
| ⑥ TopicGuard | Topic allowlist / blocklist; consumes normalized text to prevent bypass. | ⚪ | `— (inbound/outbound stage; no top-level YAML root; see guide)` | [Architecture](guides/architecture.md) |
| ⑦ RuleEngine (custom rules) | YAML declarative rule engine; per-tenant rule sets; hot reload. | 🔵 | `config/rules` + `/admin/api/rules/*` | [Admin API](guides/admin-api.md) |
| ⑧ SemanticCache (final inbound stage) | See [Semantic Cache](#4-semantic-cache). | ⚪ | `cache.*` | [Conversation cache](guides/conversation-cache.md) |
| External safety APIs | Optional cascaded OpenAI Moderation / Perspective API; async or sync. | ⚪ | `security.external_safety.*` | [External safety](guides/external-safety.md) |
| Abuse detection | Frequency anomalies + content-similarity clustering to spot malicious flooding. | ⚪ | `security.abuse_detection.*` | [Security best practices](guides/security-best-practices.md) |

### 3.2 Outbound guardrails (Outbound Pipeline · 6 stages)

| Stage / capability | Description | Edition | Config | Guide |
|--------------------|-------------|:-------:|--------|-------|
| ① ContentFilter | Outbound harmful-content detection (violence / sexual / hate, etc.); policy-level configurable. | ⚪ | `— (inbound/outbound stage; no top-level YAML root; see guide)` | [Security best practices](guides/security-best-practices.md) |
| ② Hallucination Scorer | Hallucination likelihood score (0-1); optional block or annotate. | ⚪ | `— (inbound/outbound stage; no top-level YAML root; see guide)` | [Architecture](guides/architecture.md) |
| ③ Quality Scorer | 4-dimensional output quality (relevance / completeness / consistency / fluency); feeds MLRouter EMA. | ⚪ | `— (inbound/outbound stage; no top-level YAML root; see guide)` | [Architecture · Cost optimization](guides/architecture.md) |
| ④ CostTracker (outbound) | Token billing and cost recording; writes back to `PersistentStore`. | ⚪ | `cost.*` | [Cost optimization](guides/cost-optimization.md) |
| ⑤ AlertManager | Outbound threshold alerts (error rate / latency / hallucination rate); per-rule cooldown. | ⚪ | `alerting.rules[]` | [Troubleshooting](guides/troubleshooting.md) |
| ⑥ RequestLogger (outbound) | Full request-lifecycle log including final status and cost. | ⚪ | `logging.*` | — |

### 3.3 Adaptive Guard

| Capability | Description | Edition | Endpoint | Guide |
|------------|-------------|:-------:|----------|-------|
| Decision explanation query | `GET /admin/api/guard/explanation/{request_id}`: view single-block trigger_layer / trigger_rule / model_version / explanation_text. | 🔵 | `/admin/api/guard/explanation/{id}` | [Adaptive guard](guides/adaptive-guard.md) |
| False-positive / false-negative feedback | `POST /admin/api/guard/feedback`: 4 labels (false_positive / false_negative / confirmed_block / confirmed_allow) for human-in-the-loop annotation. | 🔵 | `/admin/api/guard/feedback` | [Adaptive guard](guides/adaptive-guard.md) |
| Guard model promotion | `POST /admin/api/guard/model/promote`: shadow → active ONNX version promotion, routed through the Autonomy approval loop. | 🔵 | `/admin/api/guard/model/promote` | [Adaptive guard](guides/adaptive-guard.md) · [Guard model](guides/guard-model.md) |

### 3.4 Audit & compliance

| Capability | Description | Edition | Endpoint / config | Guide |
|------------|-------------|:-------:|-------------------|-------|
| Tamper-evident audit chain | FNV-1a hash chain + AES-256-GCM detail encryption; chain-break detection. | ⚪ | `audit.encrypt: true` | [Security best practices](guides/security-best-practices.md) |
| Audit query with decrypt-on-read | `GET /admin/api/audits` (paged / filtered / decrypted); supports time window, tenant, event type. | 🔵 | `/admin/api/audits` | [Admin API](guides/admin-api.md) |
| Compliance report export | `GET /admin/api/export/audit` (CSV/JSON) for GDPR / DJCP / SOC audit. | 🔵 | `/admin/api/export/audit` | [Compliance](compliance/README.md) |
| Error code standard | Unified `AEGIS-xxxx` error codes covering authn / rate limit / guardrail / upstream / internal. | ⚪ | See guide | [Error codes](guides/error-codes.md) |

---

## 4. Semantic Cache

Cache hits short-circuit model calls (zero API cost); pluggable vector store + pluggable embedder.

| Capability | Description | Edition | Config | Guide |
|------------|-------------|:-------:|--------|-------|
| In-process vector store (hnswlib) | Default backend, HNSW index, pure in-memory; single-node deployments. | ⚪ | `vector_store.backend: hnswlib` | [Architecture](guides/architecture.md) |
| Milvus vector store | Distributed vector database integration. | 🔵 | `vector_store.backend: milvus` | [Cache migration](guides/cache-migration.md) |
| Qdrant vector store | Distributed vector database integration. | 🔵 | `vector_store.backend: qdrant` | [Cache migration](guides/cache-migration.md) |
| Hash embedder (default) | Zero-dependency hash-based features for quick start. | ⚪ | Built-in hash embedder (default) | [Architecture](guides/architecture.md) |
| ONNX BGE embedder | `BGE-small-zh-v1.5` 512-dim; Chinese-optimized. | ⚪ | `embedding.model_path` · `embedding.vocab_path` | [Architecture](guides/architecture.md) |
| Per-model partitioning | Separate namespace per model to avoid cross-contamination. | ⚪ | `cache.max_partitions` · `cache.context_aware` | [Cache migration](guides/cache-migration.md) |
| TTL + LRU eviction | Time + capacity dual eviction dimensions. | ⚪ | `cache.ttl_seconds` · `cache.max_entries` | [Architecture](guides/architecture.md) |
| Adaptive similarity threshold | Self-balancing hit rate vs. recall; per-model tuning. | ⚪ | `cache.adaptive_threshold.enabled: true` | [Architecture](guides/architecture.md) |
| Cache-hit short-circuit | ShortCircuit state skips routing + upstream call, returns cached response directly. | ⚪ | Built-in | [Architecture · Cache-hit timing](guides/architecture.md) |
| Conversation-level cache | Multi-turn conversation caching strategy. | ⚪ | `cache.conversation_cache.*` · `cache.conversation_hash.*` | [Conversation cache](guides/conversation-cache.md) |
| Cache import | `POST /admin/cache/import` for batch pre-warming. | ⚪ | `/admin/cache/import` | [OpenAPI](openapi.yaml) |
| Cache stats | Query hit rate, entry count, memory usage. | ⚪ | Internal metrics | [Architecture](guides/architecture.md) |
| Redis distributed cache | Cluster-mode shared cache + rate-limit state. | 🔵 | `storage.cache_backend: redis` | [Production deployment](guides/production-deployment.md) |

---

## 5. Observability

Prometheus + Grafana + OTEL trio out of the box; cost / quality / usage tri-axis monitoring.

| Capability | Description | Edition | Endpoint / config | Guide |
|------------|-------------|:-------:|-------------------|-------|
| Prometheus metrics | `GET /metrics` exposes full metric set (RPS / latency / cost / hit rate / guardrail triggers / breaker state, etc.) with label aggregation. | ⚪ | `/metrics` | [Architecture](guides/architecture.md) |
| Grafana dashboards | Prebuilt Dashboard JSON (`deploy/grafana/`) covering gateway health, cost, quality, guardrails. | ⚪ | `deploy/grafana/*.json` | [Production deployment](guides/production-deployment.md) |
| OpenTelemetry tracing | Distributed tracing export to Jaeger / Tempo / OTLP collector. | ⚪ | `telemetry.enabled` · `telemetry.otlp_endpoint` · `telemetry.service_name` · `telemetry.sample_ratio` | [OTEL verification](guides/otel-verification.md) · [OTEL offline deps](guides/otel-offline-deps.md) |
| Structured logs · key masking | JSON format; API keys / tokens / PII auto-masked. | ⚪ | `logging.*` | — |
| Realtime log stream | `GET /admin/logs/stream` (SSE) live log push. | 🔵 | `/admin/logs/stream` | [OpenAPI](openapi.yaml) |
| Cost tracking | Per-request token / cost records; aggregated by tenant / model / time. | ⚪ | `cost.*` · `/admin/api/costs` | [Cost optimization](guides/cost-optimization.md) |
| Cost budgets | Daily / monthly / per-request cost caps; over-limit downgrade quality tier or alert (see `budget_guard`). | ⚪ | `budget_guard.*` | [Cost optimization](guides/cost-optimization.md) |
| Usage prediction | `GET /admin/api/predict/usage` + `/predict/budget` trend prediction. | 🔵 | `/admin/api/predict/*` | [Admin API](guides/admin-api.md) |
| Alerting rule engine | Config-driven alerting rules (multi-metric, per-rule cooldown); triggers webhook / email. | ⚪ | `alerting.rules[]` | [Troubleshooting](guides/troubleshooting.md) |
| Output quality scoring | 4-dim quality score feeding MLRouter and dashboards. | ⚪ | `— (inbound/outbound stage; no top-level YAML root; see guide)` | [Architecture](guides/architecture.md) |
| Health probes | `GET /health` / `/health/live` / `/health/ready` (k8s liveness/readiness). | ⚪ | `/health` · `/health/live` · `/health/ready` | [Production deployment](guides/production-deployment.md) |
| Security event timeline | `GET /admin/api/security/events` surfaces guardrail blocks, authn failures, etc. | 🔵 | `/admin/api/security/events` | [Admin API](guides/admin-api.md) |

---

## 6. Storage Abstraction

`CacheStore` + `PersistentStore` dual interfaces × 4 backends × graceful degradation.

| Capability | Description | Edition | Config | Guide |
|------------|-------------|:-------:|--------|-------|
| `CacheStore` interface | KV + TTL abstraction; Memory / Redis backends. | ⚪ | `storage.cache_backend` | [Architecture · Storage](guides/architecture.md) |
| `PersistentStore` interface | Table-level abstraction (tenant / user / api_key / audit / cost / savings / mfa / sso / sessions / rulesets / templates / autonomy). | ⚪ | `storage.persistent_backend` | [Architecture · Storage](guides/architecture.md) |
| Memory backend | In-memory implementation covering the full interface; used for tests and single-node default. | ⚪ | `storage.persistent_backend: memory` | — |
| SQLite (WAL) backend | Single-node persistence default; WAL mode; three-backend parity (sort order / prune timestamp format aligned). | 🟢 | `storage.persistent_backend: sqlite` · `storage.sqlite.path` | [Architecture](guides/architecture.md) |
| PostgreSQL backend | Production / cluster mode; DB-level pushdown queries (cost export, audit filtering). | 🔵 | `storage.persistent_backend: postgres` · `AEGISGATE_PG_URL` | [Production deployment](guides/production-deployment.md) |
| Redis cache backend | Cluster-mode shared cache + rate-limit state; breaker state machine (Closed/Open/HalfOpen) persisted. | 🔵 | `storage.cache_backend: redis` · `storage.redis.host`/`port` | [Production deployment](guides/production-deployment.md) |
| Graceful degradation | Backend unavailable → degrade to Memory; SECURITY-level alert but requests not blocked. | ⚪ | Built-in | [Architecture](guides/architecture.md) |
| Lock ordering convention | Global lock acquisition order across multi-table transactions to prevent deadlocks. | ⚪ | See guide | [Lock ordering](LOCK_ORDERING.md) |

---

## 7. Multi-Tenancy · RBAC · AuthN (Enterprise)

| Capability | Description | Edition | Endpoint / config | Guide |
|------------|-------------|:-------:|-------------------|-------|
| 4-tier role hierarchy | `SuperAdmin > TenantAdmin > Developer > Viewer`; cross-tier grants gated by the `auth::canGrantRole` choke point. | 🔵 | `Feature::RBAC` (Enterprise license via FeatureGate); YAML `rbac.enabled` alone does not enable runtime RBAC | [Security best practices](guides/security-best-practices.md) |
| Tenant isolation | Per-tenant data isolation (audit / cost / rules / templates / apikeys); `effectiveTenantId` enforced filtering. | 🔵 | `tenants.*` · `/admin/api/tenants/*` | [Admin API](guides/admin-api.md) |
| User CRUD | `/admin/api/users/*` create / list / update / delete. | 🔵 | `/admin/api/users` | [Admin API](guides/admin-api.md) |
| API key lifecycle | Create / list / rotate / revoke; role-≤-self enforced. | 🔵 | `/admin/api/keys/*` | [Admin API](guides/admin-api.md) |
| SSO · OIDC / OAuth2 | OIDC + PKCE login flow; Provider CRUD API. | 🔵 | `/admin/auth/sso/*` · `/admin/api/sso/providers/*` | [Admin API](guides/admin-api.md) |
| MFA · TOTP | Bind / verify / disable / recover; `enforcement=required` gate rejects non-MFA sessions; failure lockout (`mfa_failures` table with three-backend parity). | 🔵 | `/admin/api/mfa/*` · `mfa.enforcement` · `mfa.lockout_max_failures` | [Security best practices](guides/security-best-practices.md) |
| MFA recovery codes | 80-bit entropy recovery codes (`RECOVERY_CODE_BYTES=10`), constant-time verified via `CRYPTO_memcmp`. | 🔵 | `/admin/api/mfa/recovery` | [Security best practices](guides/security-best-practices.md) |
| SCIM 2.0 | Full `/scim/v2/Users` + `/scim/v2/Groups` CRUD for Okta / Azure AD / OneLogin auto-provisioning. | 🔵 | `/scim/v2/*` | — |
| Web admin panel | React + TypeScript + Vite SPA at `web/admin/`. | 🔵 | `/admin/*` (static assets) | [Admin API](guides/admin-api.md) |
| Web admin · Dashboard | Requests / cost / cache hit rate overview. | 🔵 | `/admin/api/dashboard/summary` | [Savings dashboard](guides/admin-savings.md) |
| Web admin · Adaptive Guard page | Full three-endpoint UI loop (explanation / feedback / promote). | 🔵 | `/admin/api/guard/*` | [Adaptive guard](guides/adaptive-guard.md) |
| CORS choke point | `admin::decideCors` pure function: specific origin echoes + credentials + `Vary`; wildcard `*` never with credentials; misconfig warns once. | 🔵 | `admin.cors.*` | — |
| Trusted-proxy XFF | `admin.trusted_proxies` + `resolveClientIp` trusts XFF only when peer is trusted (rightmost non-trusted segment). | 🔵 | `admin.trusted_proxies` | [Security best practices](guides/security-best-practices.md) |
| IP allowlist (CIDR) | `isAdminIpAllowed` supports IPv4 CIDR; admin-endpoint granular control. | 🔵 | `admin.ip_allowlist[]` | [Security best practices](guides/security-best-practices.md) |
| Session cookie scope | `aegis_session` strictly `Path=/admin`, aligned with `/admin/api/*` namespace; data plane `/v1/*` unaffected by admin cookies. | 🔵 | Built-in | [Adaptive guard](guides/adaptive-guard.md) |

---

## 8. Control Plane

Runtime operability under `/admin/api/*`, driven by config.

| Capability | Description | Edition | Endpoint / config | Guide |
|------------|-------------|:-------:|-------------------|-------|
| Config hot reload | `POST /admin/reload` reloads YAML; GatewayRuntime rewires pipeline and rules. | ⚪ | `/admin/reload` | [Security best practices](guides/security-best-practices.md) |
| Rule set versioning | `/admin/api/rules/*` list / create / activate; global + per-tenant; `activateRuleSet` refreshes RuleEngine immediately. | 🔵 | `/admin/api/rules/*` | [Admin API](guides/admin-api.md) |
| Prompt template management | `/admin/api/templates/*` full CRUD; when Chat Completions has no `system` message, the tenant's active template (weight-selected) is prepended as system. Optional override: `X-AegisGate-Template: <name>`. Response: `X-AegisGate-Template-Applied`. | 🔵 | `/admin/api/templates/*` · `X-AegisGate-Template` | [Admin API](guides/admin-api.md) |
| Rollout · canary | Rollout controller: progressing → completed; onTick idempotent reconciliation (activate + updateRollout compensation). Stored via control-plane / env (not a top-level `rollout.*` YAML root in `aegisgate.yaml`). | 🔵 | Control plane rollout APIs · [Rollout](guides/rollout.md) | [Rollout](guides/rollout.md) |
| Provider manifest management | Declarative model / provider metadata; validated by `aegisctl conformance`. | ⚪ | `api/providers/definitions/*.yaml` | [Provider manifest](guides/provider-manifest.md) |
| Cluster control plane | Centralized config distribution and node coordination when `deployment.mode: cluster` (Redis-backed shared state). | 🔵 | `deployment.mode: cluster` | [Control plane](guides/control-plane.md) |
| Feedback Bus | Event / feedback pub-sub; guardrail feedback / autonomy proposal flow. | ⚪ | `feedback_bus.*` | [Feedback bus](guides/feedback-bus.md) |

---

## 9. Cost Autonomy & AegisOps

The `AegisOps` strategic layer: the gateway is not only a savings pipe but also a "self-optimizing operator".

| Capability | Description | Edition | Endpoint | Guide |
|------------|-------------|:-------:|----------|-------|
| Autonomy Proposals · list | `GET /admin/api/autonomy/proposals`: view pending / decided proposals. | 🔵 | `/admin/api/autonomy/proposals` | [Cost autonomy](guides/cost-autonomy.md) |
| Autonomy Proposals · approve | `POST /admin/api/autonomy/proposals/{id}/approve`. | 🔵 | `/admin/api/autonomy/proposals/{id}/approve` | [Cost autonomy](guides/cost-autonomy.md) |
| Autonomy Proposals · reject | `POST /admin/api/autonomy/proposals/{id}/reject`. | 🔵 | `/admin/api/autonomy/proposals/{id}/reject` | [Cost autonomy](guides/cost-autonomy.md) |
| Autonomy Proposals · rollback | `DELETE /admin/api/autonomy/proposals/{id}` (rollback). | 🔵 | `DELETE /admin/api/autonomy/proposals/{id}` | [Cost autonomy](guides/cost-autonomy.md) |
| Autonomy report | `GET /admin/api/autonomy/report`: savings-impact aggregation. | 🔵 | `/admin/api/autonomy/report` | [Cost autonomy](guides/cost-autonomy.md) |
| Savings event stream | `savings_events` table + `POST/GET /admin/api/savings/events`; three-backend parity (incl. Postgres). | 🔵 | `/admin/api/savings/events` | [Savings dashboard](guides/admin-savings.md) |
| Case Study Headline | `GET /admin/api/case-study/headline` generates a pitchable savings headline. | 🔵 | `/admin/api/case-study/headline` | [Admin API](guides/admin-api.md) |
| AegisOps strategic vision | Evolution from "AI gateway" to "AI governance platform" — capability matrix and roadmap. | 📐 | — | [AegisOps vision](positioning/aegisops-vision.md) · [Roadmap](ROADMAP.md) |

---

## 10. Deployment & Cluster

| Capability | Description | Edition | Path | Guide |
|------------|-------------|:-------:|------|-------|
| Docker single-node | Single container running AegisGate + SQLite; `Dockerfile` + `docker-compose.yaml`. | 🟢 | `Dockerfile` · `docker-compose.yaml` | [Production deployment](guides/production-deployment.md) |
| 5-min quickstart container | Auto-generated API key + self-provisioned SQLite volume + one-command start. | ⚪ | `scripts/quickstart-entrypoint.sh` | [5-minute quickstart](quickstart.md) |
| Helm Chart | Official `helm/aegisgate/` chart; full `values.yaml` overrides. | 🔵 | `helm/aegisgate/*` | [Production deployment](guides/production-deployment.md) |
| K8s deployment | Native Deployment / Service / ConfigMap examples. | 🔵 | `deploy/kubernetes/*` | [Production deployment](guides/production-deployment.md) |
| Redis cluster mode | Shared cache + rate limit + breaker state; nodes stateless. | 🔵 | `deployment.mode: cluster` | [Control plane](guides/control-plane.md) |
| Multi-region | Geo-distributed topology paired with GeoRouter. | 🔵 | `routing.geo.*` | [Multi-region](guides/multi-region.md) |
| Production-validation runbook | Pre / during / post go-live checklist. | ⚪ | — | [Production validation runbook](guides/production-validation-runbook.md) |
| Performance tuning | Throughput / latency / resource three-axis optimization. | ⚪ | — | [Performance tuning](guides/performance-tuning.md) |
| macOS development | Apple Silicon / Intel local dev guide. | ⚪ | — | [macOS development](guides/macos-development.md) |

---

## 11. Plugins & Ecosystem

| Capability | Description | Edition | Config / CLI | Guide |
|------------|-------------|:-------:|--------------|-------|
| C-ABI plugin system | `dlopen` dynamic loading, stable C ABI; enables third-party guardrail / router / storage extensions. | 🔵 | `plugins.*` | — |
| Local rule-pack management | `aegisctl rules list|install|remove|info|apply` manages locally installed rule packs (`RulePackManager`); not a remote marketplace client. | 🔵 | `aegisctl rules` | — |
| Prompt template library | Tenant prompt templates via `/admin/api/templates/*` CRUD; optional Chat Completions system-message injection (no shipped built-in template pack directory). | ⚪ | `/admin/api/templates/*` | [Admin API](guides/admin-api.md) |
| Showcase demo | LLM → AegisGate → application landing reference demo (AI drama flagship / e-commerce validation). | ⚪ | `apps/showcase/` | [Showcase](../apps/showcase/README.md) |

---

## 12. Client SDKs

| Language | Package | Highlights | Edition | Guide |
|----------|---------|------------|:-------:|-------|
| Python | `aegisgate` | Sync + async (httpx-based) | ⚪ | [SDK integration](guides/sdk-integration.md) · [Python SDK](../sdk/python/) |
| Node.js | `@aegisgate/sdk` | TypeScript, native fetch, ESM | ⚪ | [SDK integration](guides/sdk-integration.md) · [Node.js SDK](../sdk/nodejs/) |
| Go | `aegisgate-go` | Zero dependencies, stdlib only | ⚪ | [SDK integration](guides/sdk-integration.md) · [Go SDK](../sdk/go/) |
| Java / Kotlin | `dev.aegisgate` (Gradle) | JVM client | ⚪ | [Java/Kotlin SDK](../sdk/java/) |
| Rust | `aegisgate` (crate) | Async client | ⚪ | [Rust SDK](../sdk/rust/) |

Python / Node.js / Go SDKs provide chat completions (streaming + non-streaming), model listing, health check, `/metrics` retrieval, and config reload. Java/Kotlin and Rust cover the core client surface documented in each SDK README (capability parity with the first three is not claimed here).

---

## 13. CLI Tooling

`aegisctl` is the single CLI covering operations / estimation / validation / local rule packs.

| Subcommand | Description | Guide |
|------------|-------------|-------|
| `aegisctl estimate` | Historical logs → post-migration token/cost savings estimate. | [Savings estimate](estimate.md) |
| `aegisctl conformance check <manifest>` | Provider manifest conformance check. | [Provider manifest](guides/provider-manifest.md) |
| `aegisctl conformance check-all <dir>` | Directory-level batch check. | [Provider manifest](guides/provider-manifest.md) |
| `aegisctl rules list|install|remove|info|apply` | Local rule-pack manager (`RulePackManager`). | — |
| `aegisctl bench` | Benchmark load test (throughput / latency). | [Performance tuning](guides/performance-tuning.md) |

---

## 14. API Endpoint Index

### 14.1 Data plane (OpenAI-compatible · `/v1/*`)

| Endpoint | Method | Purpose |
|----------|:------:|---------|
| `/v1/chat/completions` | POST | Chat completions (streaming / non-streaming) |
| `/v1/embeddings` | POST | Text embeddings |
| `/v1/images/generations` | POST | Image generation |
| `/v1/audio/transcriptions` | POST | Speech-to-text |
| `/v1/audio/translations` | POST | Audio translation |
| `/v1/audio/speech` | POST | Text-to-speech |
| `/v1/models` | GET | Models listing |

### 14.2 Observability plane

| Endpoint | Method | Purpose |
|----------|:------:|---------|
| `/health` · `/health/live` · `/health/ready` | GET | Health / k8s liveness / readiness |
| `/metrics` | GET | Prometheus metrics |
| `/admin/reload` | POST | Config hot reload |
| `/admin/cache/import` | POST | Cache batch import |
| `/admin/logs/stream` | GET (SSE) | Realtime log stream |

### 14.3 Admin plane (`/admin/api/*` · Enterprise)

- **Auth / session:** `/admin/api/auth/login` · `/admin/api/auth/logout` · `/admin/api/me`
- **Tenants:** `/admin/api/tenants` · `/{id}`
- **Users:** `/admin/api/users` · `/{id}`
- **API keys:** `/admin/api/keys` · `/{id}/revoke` · `/{id}/rotate`
- **Audit & cost:** `/admin/api/audits` · `/admin/api/costs` · `/admin/api/dashboard/summary` · `/admin/api/savings/summary` · `/admin/api/security/events` · `/admin/api/case-study/headline`
- **SSO / MFA:** `/admin/auth/sso/*` · `/admin/api/sso/providers/*` · `/admin/api/mfa/*`
- **Prompt templates:** `/admin/api/templates/*`
- **Rule sets:** `/admin/api/rules` · `/admin/api/rules/active` · `POST /admin/api/rules/activate`
- **Prediction:** `/admin/api/predict/usage` · `/admin/api/predict/budget`
- **Report exports:** `/admin/api/export/audit` · `/admin/api/export/cost`
- **Autonomy proposals:** `/admin/api/autonomy/proposals` · `POST .../{id}/approve` · `POST .../{id}/reject` · `DELETE .../{id}` (rollback) · `/admin/api/autonomy/report`
- **Adaptive Guard:** `/admin/api/guard/{feedback,explanation/{id},model/promote}`

### 14.4 SCIM 2.0 · Enterprise

| Endpoint | Method | Purpose |
|----------|:------:|---------|
| `/scim/v2/Users` · `/{id}` | GET / POST / PUT / DELETE | User provisioning |
| `/scim/v2/Groups` · `/{id}` | GET / POST / PUT / DELETE | Group provisioning |

Full machine-readable definition: [OpenAPI specification](openapi.yaml).

---

## 15. Edition Capability Matrix

Aligned with the [`README.md` Editions section](../README.md#editions), expanded to sub-capability granularity:

| Capability domain | Community | Enterprise |
|-------------------|:---------:|:----------:|
| Unified API proxy (7 vendors + OpenAI-compatible) | ✅ | ✅ |
| Token optimization (compression / precise sizing / CJK) | ✅ | ✅ |
| Multi-modal proxy (Embeddings / Images / Audio) | ✅ | ✅ |
| Routing · BasicRouter | ✅ | ✅ |
| Routing · CostAwareRouter | ✅ | ✅ |
| Routing · MLRouter | — | ✅ |
| Routing · A/B Test · Geo | — | ✅ |
| Fallback / load balancing / rate limiting | ✅ | ✅ |
| Guardrails · basic (NFKC / L1-L2 injection / PII / topic) | ✅ | ✅ |
| Guardrails · L3 ONNX GuardModel | — | ✅ |
| Guardrails · YAML rule engine + per-tenant | — | ✅ |
| Guardrails · Adaptive Guard (explain / feedback / promote) | — | ✅ |
| Semantic cache · hnswlib in-process | ✅ | ✅ |
| Semantic cache · Milvus / Qdrant / Redis distributed | — | ✅ |
| Storage · SQLite | ✅ | ✅ |
| Storage · PostgreSQL | — | ✅ |
| Storage · Redis cluster shared | — | ✅ |
| Observability · Prometheus + Grafana | ✅ | ✅ |
| Observability · OTEL tracing | ✅ | ✅ |
| Observability · realtime log stream / security events | — | ✅ |
| Management · CLI (`aegisctl`) | ✅ | ✅ |
| Management · web panel (React SPA) | — | ✅ |
| Multi-tenancy · RBAC · SSO · MFA · SCIM | — | ✅ |
| Audit · tamper-evident chain + encryption | ✅ | ✅ |
| Audit · compliance report export / long-term archive | — | ✅ |
| Control plane · config hot reload | ✅ | ✅ |
| Control plane · rule set versioning / rollout / canary | — | ✅ |
| Cost autonomy · Autonomy proposal loop | — | ✅ |
| Deployment · Docker single-node | ✅ | ✅ |
| Deployment · Helm / K8s / cluster / multi-region | — | ✅ |
| Plugin system · C-ABI dlopen | — | ✅ |
| Local rule-pack management · `aegisctl rules` | — | ✅ |
| Feedback Bus | ✅ | ✅ |
| Provider manifest | ✅ | ✅ |
| Prompt template library (Admin CRUD) | — | ✅ |
| Showcase demo (`apps/showcase/`) | ✅ | ✅ |
| Client SDKs (Python / Node / Go / Java / Rust) | ✅ | ✅ |

Legend: ✅ = supported · — = not supported in this edition.

---

## Related documents

- [README](../README.md) — project overview and quick start
- [Architecture guide](guides/architecture.md) — system overview, request pipeline, timing diagrams
- [OpenAPI specification](openapi.yaml) · [OpenAI compat matrix](openai-compat-matrix.md) — machine-readable API definitions
- [Roadmap](ROADMAP.md) — AegisOps strategic roadmap
- [Documentation index](README.md) — all guides

---

*This list is updated whenever a capability lands or is deprecated. It is the single source of truth for external capability inventories. Report gaps or errors via [GitHub Discussions](https://github.com/privonyx/loong-aegisgate/discussions).*
