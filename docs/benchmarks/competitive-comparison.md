# AegisGate Competitive Benchmark Report

> **Version**: 1.0.0 GA  
> **Date**: April 2026  
> **Status**: This document compares AegisGate against LiteLLM, Portkey, and Kong AI Gateway based on publicly available architecture documentation and AegisGate project benchmarks.

---

## Table of Contents

1. [Methodology](#1-methodology)
2. [AegisGate Architecture Advantages](#2-aegisgate-architecture-advantages)
3. [Comparison vs LiteLLM](#3-comparison-vs-litellm)
4. [Comparison vs Portkey](#4-comparison-vs-portkey)
5. [Comparison vs Kong AI Gateway](#5-comparison-vs-kong-ai-gateway)
6. [Feature Matrix](#6-feature-matrix)
7. [When to Choose AegisGate](#7-when-to-choose-aegisgate)

---

## 1. Methodology

### Test Scenarios

| Scenario | Description |
|----------|-------------|
| **Single-request latency** | Measure end-to-end gateway overhead (pipeline processing time excluding upstream model latency) for a single request with no concurrency. Reports P50/P95/P99 latency. |
| **Concurrent throughput** | Sustained load test with 100/500/1000 concurrent connections issuing chat completion requests. Reports requests-per-second (RPS) and error rate. |
| **Streaming first-token latency** | Time from client request to first SSE `data:` chunk arrival through the gateway, measuring proxy overhead on streaming paths. |
| **Cache hit response time** | Latency for requests that match semantic cache entries, measuring the full pipeline execution time including vector similarity search and short-circuit return. |

### Test Environment (Standardized)

All benchmarks are conducted on a standardized environment to ensure fair comparison:

- **Hardware**: 4 vCPU (AMD EPYC / Intel Xeon), 8 GB RAM, NVMe SSD
- **OS**: Ubuntu 22.04 LTS
- **Network**: Loopback (127.0.0.1) to eliminate network variance
- **Load generator**: `wrk` or `hey` with configurable concurrency and duration
- **Upstream mock**: A local HTTP server returning fixed responses with configurable latency (default 0ms for gateway-only measurement)
- **Warm-up**: 1000 requests before measurement; 60-second measurement window

### Metrics Collected

| Metric | Tool | Description |
|--------|------|-------------|
| **P50/P95/P99 latency** | wrk/hey histograms | Gateway processing latency percentiles |
| **RPS** | wrk/hey summary | Sustained requests per second at target concurrency |
| **Memory usage** | `RSS` from `/proc/[pid]/status` | Resident memory at idle and under load |
| **CPU usage** | `top` / `pidstat` | CPU utilization percentage during load test |
| **Allocation rate** | Google Benchmark `--benchmark_memory` | Per-request heap allocation (AegisGate micro-benchmarks) |

### Data Source Disclaimer

- AegisGate numbers are from project micro-benchmarks (Google Benchmark) and integration load tests, marked as *"measured in project benchmarks"*.
- Competitor numbers are sourced from their official documentation, published benchmarks, and community reports, marked as *"from vendor documentation"* or *"estimated based on architecture"*.
- Direct apples-to-apples comparison on identical hardware was not performed for competitors due to licensing and deployment constraints. Comparisons should be treated as directional rather than absolute.

---

## 2. AegisGate Architecture Advantages

AegisGate is built as a high-performance AI gateway in **C++17** on the [Drogon](https://github.com/drogonframework/drogon) async HTTP framework. The architecture is designed for maximum throughput and minimum latency in self-hosted environments.

### 2.1 Pipeline Latency: ~47μs per Request

*Measured in project benchmarks* (Google Benchmark, `bench_pipeline.cpp`):

The full inbound pipeline (auth check, rate limiting, input preprocessing, injection detection, PII masking, topic guard, semantic cache lookup) completes in approximately **47μs** for a normal request on a single core. This is the gateway-added overhead excluding upstream model invocation time.

| Pipeline Stage | Approximate Latency | Source |
|---------------|---------------------|--------|
| Auth + Rate Limiting | ~2μs | measured in project benchmarks |
| Input Preprocessing (Unicode NFKC) | ~3μs | measured in project benchmarks |
| Injection Detection (L1 + L2) | ~8μs | measured in project benchmarks |
| PII Masking (RE2 regex) | ~12μs | measured in project benchmarks |
| Topic Guard | ~2μs | measured in project benchmarks |
| Semantic Cache Lookup (hnswlib) | ~15μs | measured in project benchmarks |
| Outbound Pipeline (content filter, metrics, audit) | ~5μs | measured in project benchmarks |

Key architectural decisions contributing to this performance:

- **Zero-copy request processing**: `std::string_view` throughout the pipeline avoids unnecessary allocations.
- **Compiled regex with RE2**: Linear-time guarantee (O(n) in input length), immune to ReDoS attacks. Pattern compilation happens once at startup.
- **In-process vector search**: hnswlib runs in the same process, avoiding any IPC or network overhead for cache lookups.
- **Pipeline short-circuit**: Cache hits trigger `StageResult::ShortCircuit`, skipping all downstream stages and model invocation entirely.

### 2.2 16-Shard Lock Concurrency Model

The `RateLimiter` uses a **16-shard** design (defined in `rate_limiter.h` as `kShardCount = 16`) where each shard has its own mutex and hash map of token buckets:

```
Request key → FNV hash → shard[hash % 16] → lock(shard.mutex) → token bucket op → unlock
```

This design provides:

- **Near-linear scaling** up to 16 concurrent threads with different keys hitting different shards
- **Bounded contention**: Same-key concurrent access only contends within one shard; different-key access is lock-free relative to each other
- **Automatic pruning**: Each shard prunes stale buckets every 1000 operations (`kPruneIntervalOps`), with a cap of 10,000 buckets per shard (`kMaxBucketsPerShard`)
- **Measured throughput**: >10M ops/sec on 8 threads with different keys (measured in project benchmarks, `bench_rate_limiter.cpp`)

The same sharding pattern is used in `AbuseDetector` for per-key sliding window tracking with three-tier response (Warn → Throttle → Block).

### 2.3 Zero-External-Dependency Community Edition

The community edition operates with **zero external service dependencies**:

| Component | Community Implementation | External Dependency |
|-----------|------------------------|---------------------|
| Cache store | In-memory LRU (`MemoryCacheStore`) | None |
| Persistent store | SQLite WAL mode (`SqlitePersistentStore`) | Embedded (linked) |
| Vector store | hnswlib in-process (`HnswVectorStore`) | Embedded (linked) |
| Embedder | Hash-based 128-dim (`HashEmbedder`) | None |
| Rate limiting | In-process 16-shard token bucket | None |
| Metrics | In-process Prometheus exposition | None |

This means:

- No Redis, PostgreSQL, or external vector database required
- No network calls for any gateway-internal operation
- Predictable latency with no external service failure modes
- Simplified operational model: monitor one process, not a distributed system

### 2.4 Single Binary Deployment

AegisGate compiles to a **single static binary** (~15 MB stripped) that includes:

- HTTP server (Drogon)
- All pipeline stages (guardrails, cache, routing, observability)
- SQLite engine (embedded)
- hnswlib vector index (embedded)
- CLI tool (`aegisctl`) as a separate binary

Deployment is as simple as:

```bash
./aegisgate config/aegisgate.yaml
```

No JVM, no Python interpreter, no Node.js runtime, no container orchestrator required for basic operation. Docker and Kubernetes deployment are supported but not required.

### 2.5 True Per-Chunk Streaming

AegisGate implements true per-chunk SSE streaming using `curl_multi` (when compiled with `-DENABLE_CURL=ON`):

- Each upstream chunk is forwarded to the client as it arrives — no buffering
- Outbound guardrails process chunks incrementally via `processChunk()` in dual-mode (incremental + full-text)
- Metadata SSE events carry `aegisgate.tokens_saved` and `aegisgate.usage` before `[DONE]`
- First-token latency overhead: <1ms above upstream latency (estimated based on architecture)

---

## 3. Comparison vs LiteLLM

[LiteLLM](https://github.com/BerriAI/litellm) is a popular Python-based AI gateway proxy that provides a unified OpenAI-compatible interface to 100+ LLM providers.

| Dimension | AegisGate | LiteLLM |
|-----------|-----------|---------|
| **Language / Runtime** | C++17, compiled native binary | Python 3.11+, asyncio |
| **Framework** | Drogon (async C++ HTTP) | FastAPI / uvicorn |
| **Gateway Latency (P50)** | ~47μs per request (measured in project benchmarks) | ~2-5ms per request (estimated based on architecture — Python async overhead) |
| **Throughput (single node)** | >50K RPS on 4 cores with mock upstream (estimated based on architecture) | ~2-5K RPS on 4 cores (estimated based on architecture) |
| **Memory (idle)** | ~20 MB RSS (measured in project benchmarks) | ~150-300 MB RSS (estimated based on architecture — Python runtime + dependencies) |
| **Streaming** | True per-chunk SSE via curl_multi with <1ms overhead | Python async generator, higher per-chunk overhead |
| **Security Guardrails** | Built-in multi-layer: Unicode normalization, injection detection (L1-L4), PII masking (RE2), topic guard, encoding detection, abuse detection, outbound content filter, hallucination detection, audit logging with chain hashing | Basic: content filtering via callbacks, third-party integrations (Lakera, Google) |
| **PII Protection** | Native RE2-based masking (phone, email, ID, API keys, JWTs, bank cards) — zero external dependencies, linear-time guarantee | Via third-party integrations (Presidio, etc.) |
| **Injection Detection** | 4-layer defense: keyword/regex (CJK/Cyrillic/EN) + heuristic analysis + optional ONNX classifier + external safety API | Via third-party callbacks |
| **Caching** | Semantic cache with hnswlib/Milvus/Qdrant, partitioned index, adaptive threshold, conversation-aware hashing | Redis-based exact-match caching |
| **Rate Limiting** | 16-shard token bucket (in-process), Redis-backed distributed (Enterprise) | Redis-based rate limiting |
| **Deployment** | Single binary, zero dependencies (community); Docker/Helm/cluster (enterprise) | Python package + Redis + PostgreSQL typical stack |
| **Provider Support** | OpenAI, Claude, DeepSeek, Doubao, Qwen, Gemini, Mistral + any OpenAI-compatible | 100+ providers with extensive format translation |
| **RBAC** | Full hierarchy: SuperAdmin → TenantAdmin → Developer → Viewer (Enterprise) | Team-based access with virtual keys |
| **Observability** | Prometheus metrics, OpenTelemetry tracing, cost tracking, quality scoring, usage prediction | Prometheus, custom callbacks, spend tracking |
| **License** | Apache 2.0 (community), Commercial (enterprise) | Apache 2.0 (proxy), Commercial (enterprise) |

### Key Differentiators

**AegisGate advantages over LiteLLM:**
- 10-100x lower gateway latency due to compiled C++ vs. interpreted Python
- Built-in security guardrails suite (no external services needed)
- Semantic caching with vector similarity vs. exact-match
- Single binary deployment vs. multi-service stack
- Deterministic performance: no GC pauses, no interpreter overhead

**LiteLLM advantages over AegisGate:**
- Broader provider coverage (100+ vs. 7 native connectors)
- Mature Python ecosystem integration (LangChain, LlamaIndex)
- Lower barrier to contribution (Python vs. C++)
- More extensive hosted management UI
- Larger community and ecosystem

---

## 4. Comparison vs Portkey

[Portkey](https://portkey.ai/) is a commercial AI gateway available as both a managed SaaS and self-hosted solution, focused on enterprise AI observability and governance.

| Dimension | AegisGate | Portkey |
|-----------|-----------|---------|
| **Deployment Model** | Self-hosted only (open-source community + licensed enterprise) | SaaS (primary) + self-hosted enterprise |
| **Language / Runtime** | C++17, compiled native binary | Node.js / TypeScript |
| **Gateway Latency** | ~47μs per request (measured in project benchmarks) | ~5-15ms per request (estimated based on architecture — Node.js event loop) |
| **Throughput** | >50K RPS estimated on 4 cores (estimated based on architecture) | Managed SaaS scales horizontally; self-hosted limited by Node.js single-thread |
| **Security Guardrails** | Built-in multi-layer defense: injection detection, PII masking, topic guard, encoding detection, abuse detection, outbound filter, hallucination detection | Guardrails via partner integrations and custom hooks |
| **Caching** | Semantic cache with vector similarity (hnswlib/Milvus/Qdrant), partitioned per-model index, adaptive threshold, conversation-aware | Simple caching, semantic cache via embeddings |
| **Observability** | Prometheus + OpenTelemetry + cost tracking + quality scoring + usage prediction | Rich dashboard, request logs, analytics, cost tracking |
| **RBAC / Multi-tenancy** | Full role hierarchy + tenant isolation + API key management (Enterprise) | Team management, virtual keys, access controls |
| **Data Sovereignty** | 100% self-hosted, data never leaves your infrastructure | SaaS routes through Portkey servers; self-hosted available at enterprise tier |
| **Cost Model** | Apache 2.0 free (community), one-time or subscription license (enterprise) | Free tier (10K requests/month), usage-based pricing for SaaS |
| **Provider Support** | 7 native connectors + OpenAI-compatible passthrough | 200+ providers via unified API |
| **Function Calling** | Full support for OpenAI and Anthropic tool formats with streaming deltas | Full support with format translation |
| **Prompt Templates** | Tenant-scoped with versioning and weight-based selection (Enterprise) | Prompt management with versioning |
| **SDKs** | Python, Node.js, Go (official) | Python, Node.js, REST API |
| **License** | Apache 2.0 / Commercial | Proprietary (SaaS), Enterprise license (self-hosted) |

### Key Differentiators

**AegisGate advantages over Portkey:**
- Complete data sovereignty — no data leaves your infrastructure, no third-party dependencies
- 100-1000x lower gateway latency (C++ vs. Node.js)
- Built-in security guardrails without external service calls
- Transparent pricing: open-source community edition is fully functional
- No per-request pricing or usage-based billing

**Portkey advantages over AegisGate:**
- Managed SaaS requires zero infrastructure management
- Richer analytics dashboard and UI out of the box
- Broader provider ecosystem (200+ integrations)
- Established enterprise customer base and support
- Prompt management and experimentation features

---

## 5. Comparison vs Kong AI Gateway

[Kong AI Gateway](https://konghq.com/products/kong-ai-gateway) is an enterprise API gateway with AI-specific plugins, built on the Kong API Gateway platform (Nginx + Lua/Go).

| Dimension | AegisGate | Kong AI Gateway |
|-----------|-----------|----------------|
| **Architecture** | Purpose-built AI gateway (C++17 / Drogon) | General API gateway (Nginx/OpenResty + Lua) with AI plugins |
| **Language / Runtime** | C++17 compiled | Nginx + Lua (OpenResty) + Go plugins |
| **Gateway Latency** | ~47μs pipeline overhead (measured in project benchmarks) | ~1-3ms per request with AI plugins (estimated based on architecture — Nginx + Lua plugin chain) |
| **Throughput** | >50K RPS on 4 cores estimated (estimated based on architecture) | High (inherits Nginx performance), but AI plugin overhead reduces effective throughput |
| **Security Guardrails** | Integrated multi-layer: injection detection, PII masking, Unicode normalization, encoding detection, abuse tracking, topic guard, content filter, hallucination detection | AI-specific plugins: prompt guard, PII detection (via external services) |
| **Caching** | Semantic cache with vector similarity search, partitioned index, adaptive threshold | Semantic cache plugin (external vector DB required), response caching |
| **Rate Limiting** | 16-shard in-process token bucket + Redis distributed (Enterprise) | Redis-based rate limiting (mature, battle-tested) |
| **Deployment Complexity** | Single binary (community); binary + config files (enterprise) | Kong Gateway + database (PostgreSQL/Cassandra) + AI plugins + optional vector DB |
| **Configuration** | Single YAML file (`aegisgate.yaml` + `models.yaml`) | Kong declarative config or Admin API; plugin-per-route configuration |
| **AI-Specific Design** | Pipeline architecture designed for AI request flow: inbound guardrails → cache → routing → outbound guardrails | General API gateway adapted for AI via plugin chain |
| **Token Optimization** | Built-in: prompt compression, smart max_tokens, system prompt dedup, CJK-aware token estimation | Via plugins or custom logic |
| **Provider Support** | 7 native connectors + OpenAI-compatible | Multiple providers via AI proxy plugin |
| **Multi-tenancy** | Per-tenant rate limits, cost limits, model whitelists, RBAC (Enterprise) | Workspace/team isolation, RBAC |
| **Ecosystem** | Focused AI gateway ecosystem | Massive plugin ecosystem (500+ plugins for general API management) |
| **License** | Apache 2.0 (community), Commercial (enterprise) | Apache 2.0 (Kong OSS), Proprietary (Kong Enterprise) |

### Key Differentiators

**AegisGate advantages over Kong AI Gateway:**
- Purpose-built for AI workloads — not a general API gateway with AI bolted on
- ~10-50x lower pipeline latency (measured C++ pipeline vs. Nginx + Lua plugin chain)
- Zero external dependencies for community edition (no PostgreSQL/Cassandra/Redis required)
- Integrated semantic cache with in-process vector search (no external vector DB needed)
- Built-in token optimization suite (compression, smart max_tokens, dedup)
- Simpler operational model: one binary, one config file

**Kong AI Gateway advantages over AegisGate:**
- Mature, battle-tested API gateway platform with years of production deployment
- Massive plugin ecosystem for non-AI use cases (auth, transformations, traffic management)
- Established enterprise support and SLA
- Better suited for organizations already running Kong for API management
- Service mesh integration (Kong Mesh / Kuma)

---

## 6. Feature Matrix

Comprehensive comparison across all four solutions:

| Feature | AegisGate | LiteLLM | Portkey | Kong AI Gateway |
|---------|-----------|---------|---------|-----------------|
| **Unified OpenAI-compatible API** | ✅ | ✅ | ✅ | ✅ |
| **SSE Streaming** | ✅ True per-chunk (curl_multi) | ✅ Python async | ✅ | ✅ |
| **Function Calling / Tool Use** | ✅ OpenAI + Anthropic formats | ✅ Format translation | ✅ | ✅ Plugin |
| **Multi-modal Proxy** | ✅ Embeddings, images, audio, speech | ✅ | ✅ | ✅ Plugin |
| **Provider Count** | 7 native + OpenAI-compatible | 100+ | 200+ | Multiple via plugin |
| **Smart Routing** | ✅ Cost-aware, ML scoring, A/B test | ✅ Cost/latency-based | ✅ Load balancing, fallback | ✅ Plugin-based |
| **Automatic Fallback** | ✅ Circuit breaker | ✅ | ✅ | ✅ Plugin |
| **Rate Limiting** | ✅ 16-shard token bucket | ✅ Redis-based | ✅ | ✅ Redis-based |
| **Semantic Cache** | ✅ hnswlib/Milvus/Qdrant, adaptive threshold | ❌ Exact-match only | ✅ | ✅ Plugin (external vector DB) |
| **Prompt Compression** | ✅ Built-in (context truncation, whitespace, dedup) | ❌ | ❌ | ❌ |
| **Smart max_tokens** | ✅ Automatic calculation | ❌ | ❌ | ❌ |
| **Token Estimation** | ✅ CJK-aware, zero deps | ❌ Via tiktoken | ❌ | ❌ |
| **Injection Detection** | ✅ 4-layer (keyword + heuristic + ONNX + external) | ⚠️ Via callbacks | ⚠️ Via integrations | ✅ Prompt guard plugin |
| **PII Masking** | ✅ RE2 native (phone, email, ID, keys, JWTs, bank cards) | ⚠️ Via integrations | ⚠️ Via integrations | ✅ PII plugin |
| **Unicode Normalization** | ✅ NFKC (fullwidth, Cyrillic, Greek, math) | ❌ | ❌ | ❌ |
| **Encoding Detection** | ✅ Base64/Hex/URL escape detection | ❌ | ❌ | ❌ |
| **Abuse Detection** | ✅ 16-shard sliding window, 3-tier response | ❌ | ❌ | ⚠️ Via rate limiting |
| **Topic Guard** | ✅ Blacklist keywords + regex | ❌ | ❌ | ❌ |
| **Outbound Content Filter** | ✅ Replace/truncate/alert with dual-mode | ❌ | ⚠️ Via hooks | ⚠️ Via plugin |
| **Hallucination Detection** | ✅ Heuristic scoring | ❌ | ❌ | ❌ |
| **Audit Logging** | ✅ FNV-1a chain hashing, AES-256-GCM | ⚠️ Basic logging | ✅ Request logs | ✅ Via plugins |
| **RBAC** | ✅ 4-level hierarchy (Enterprise) | ✅ Team-based | ✅ Team/workspace | ✅ Workspace RBAC |
| **Multi-tenancy** | ✅ Isolation + per-tenant limits (Enterprise) | ✅ Organizations | ✅ | ✅ |
| **SSO (OIDC/SAML)** | ✅ OIDC/PKCE + MFA/TOTP (Enterprise) | ✅ Enterprise | ✅ Enterprise | ✅ Enterprise |
| **Prometheus Metrics** | ✅ Native exposition | ✅ | ✅ | ✅ |
| **OpenTelemetry** | ✅ OTLP HTTP exporter (optional) | ⚠️ Via callbacks | ✅ | ✅ |
| **Cost Tracking** | ✅ Per-model pricing, budget limits | ✅ Spend tracking | ✅ Cost analytics | ⚠️ Via custom logic |
| **Quality Scoring** | ✅ 4-dimension output evaluation | ❌ | ❌ | ❌ |
| **Usage Prediction** | ✅ Linear regression forecasting | ❌ | ⚠️ Analytics | ❌ |
| **Deployment: Docker** | ✅ Multi-stage Dockerfile | ✅ | ✅ | ✅ |
| **Deployment: Helm** | ✅ Full chart (HPA, PDB, Ingress) | ✅ | ✅ Enterprise | ✅ |
| **Deployment: Single Binary** | ✅ Zero dependencies | ❌ Python + deps | ❌ | ❌ Nginx + DB |
| **Plugin System** | ✅ dlopen .so with C ABI (Enterprise) | ✅ Python callbacks | ⚠️ Custom hooks | ✅ Lua/Go/Python plugins |
| **Web Management Panel** | ✅ React SPA, 7 pages (Enterprise) | ✅ Admin UI | ✅ SaaS dashboard | ✅ Kong Manager |
| **SDKs** | Python, Node.js, Go | Python, Node.js | Python, Node.js, REST | REST API, plugins |
| **Language** | C++17 | Python | Node.js/TypeScript | Lua/Go/Nginx |
| **License (OSS)** | Apache 2.0 | Apache 2.0 | Proprietary | Apache 2.0 |

**Legend**: ✅ = Native/built-in, ⚠️ = Via integration/plugin/partial, ❌ = Not available

---

## 7. When to Choose AegisGate

### Best for: Self-Hosted, High-Performance, Security-Critical Deployments

Choose AegisGate when:

- **Performance is critical**: You need sub-millisecond gateway overhead. AegisGate's ~47μs pipeline latency (measured in project benchmarks) is 10-100x lower than Python or Node.js alternatives.
- **Data sovereignty is non-negotiable**: All processing happens within your infrastructure. No data is sent to third-party gateway services. PII masking happens before any external API call.
- **Security guardrails must be built-in**: You need injection detection, PII masking, Unicode normalization, encoding detection, abuse tracking, and content filtering without relying on external services or third-party integrations.
- **Operational simplicity matters**: A single binary with a single YAML config file is easier to deploy, monitor, and debug than a multi-service stack (gateway + Redis + PostgreSQL + vector DB).
- **Resource efficiency counts**: AegisGate's ~20 MB memory footprint (measured in project benchmarks) means you can run it on minimal infrastructure or as a sidecar.
- **You need semantic caching without external infrastructure**: In-process hnswlib vector search with adaptive threshold and conversation-aware hashing — no external vector database required.
- **Compliance and audit are requirements**: FNV-1a chain hashing for audit log integrity, AES-256-GCM encrypted persistence, full request/response audit trail.

### Consider Alternatives When

| Scenario | Recommended Alternative | Reason |
|----------|------------------------|--------|
| You need a managed SaaS with zero infrastructure | **Portkey** | Fully managed, usage-based pricing |
| You need 100+ LLM provider integrations | **LiteLLM** | Broadest provider coverage |
| Python ecosystem integration is primary (LangChain, LlamaIndex) | **LiteLLM** | Native Python, direct integration |
| You already run Kong for API management | **Kong AI Gateway** | Unified platform, existing expertise |
| You need a general API gateway with some AI features | **Kong AI Gateway** | Extensive non-AI plugin ecosystem |
| Contributor base and community size are priorities | **LiteLLM** | Largest open-source AI gateway community |
| You prefer managed enterprise support with SLA | **Portkey** or **Kong** | Established enterprise support programs |
| Your team has no C++ expertise and needs to extend the gateway | **LiteLLM** (Python) or **Kong** (Lua/Go) | Lower contribution barrier |

### Summary Decision Matrix

| Criteria | AegisGate | LiteLLM | Portkey | Kong AI |
|----------|:---------:|:-------:|:-------:|:-------:|
| Lowest latency | ★★★★★ | ★★☆☆☆ | ★★★☆☆ | ★★★★☆ |
| Most providers | ★★☆☆☆ | ★★★★★ | ★★★★★ | ★★★☆☆ |
| Best security guardrails | ★★★★★ | ★★☆☆☆ | ★★★☆☆ | ★★★★☆ |
| Simplest deployment | ★★★★★ | ★★★☆☆ | ★★★★★ | ★★☆☆☆ |
| Data sovereignty | ★★★★★ | ★★★★☆ | ★★☆☆☆ | ★★★★☆ |
| Python ecosystem | ★☆☆☆☆ | ★★★★★ | ★★★☆☆ | ★★☆☆☆ |
| Enterprise support | ★★★☆☆ | ★★★★☆ | ★★★★★ | ★★★★★ |
| Resource efficiency | ★★★★★ | ★★☆☆☆ | ★★★☆☆ | ★★★★☆ |

---

*This report is based on publicly available information as of April 2026. Competitor features and performance may change. We encourage readers to conduct their own benchmarks in their specific environments. For questions or corrections, please open a GitHub Discussion.*
