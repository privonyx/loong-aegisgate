# AegisGate Architecture Guide

This document describes AegisGate’s system architecture, request processing flow, core component interactions, and key sequences in detail.

## System Overview

```
                         ┌──────────────────────────────────────────────────────────────┐
                         │                        AegisGate                             │
                         │                                                              │
   Client ──── HTTP ────►│  API Controller ─► GatewayRuntime ─► Pipeline Engine         │
   (SDK/curl)            │       │                  │                │                   │
                         │       │           ┌──────┴──────┐   ┌────┴────┐              │
                         │       │           │   Router    │   │ Inbound │              │
                         │       │           │ (ML/AB/Cost)│   │ Pipeline│              │
                         │       │           └──────┬──────┘   └────┬────┘              │
                         │       │                  │               │                   │
                         │       │           ┌──────┴──────┐   ┌────┴────┐              │
                         │       │           │  Fallback   │   │Outbound │              │
                         │       │           │  Manager    │   │Pipeline │              │
                         │       │           └──────┬──────┘   └────┬────┘              │
                         │       │                  │               │                   │
                         │  Admin Controller        │          Observability             │
                         │  (REST + WebSocket)      │   (Metrics/Cost/Quality/Predict)  │
                         │                          │                                   │
                         └──────────────────────────┼───────────────────────────────────┘
                                                    │
                                          ┌─────────┴─────────┐
                                          │   AI Providers    │
                                          │ OpenAI / Claude / │
                                          │ DeepSeek / Doubao │
                                          │ Qwen / Gemini ... │
                                          └───────────────────┘
```

## Request Processing Flow

### Full Request Pipeline

```
Client request
    │
    ▼
┌─────────────────┐
│ Auth & Rate     │  API Key verification → RBAC authorization → token-bucket rate limiting → abuse detection
│ Limiting        │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Inbound         │  Eight stages run in sequence
│ Pipeline        │
│  ① AuditLogger  │  Record request audit logs
│  ② Preprocessor │  Unicode NFKC normalization + encoding detection
│  ③ Injection    │  L1 keywords + L2 heuristic injection detection
│  ④ GuardModel   │  L3 ONNX classifier (optional)
│  ⑤ PIIFilter    │  RE2 regex redaction (phone / email / ID / secrets)
│  ⑥ TopicGuard   │  Topic allowlist / blocklist filtering
│  ⑦ RuleEngine   │  YAML declarative custom rules (Enterprise)
│  ⑧ SemanticCache│  Semantic similarity match → ShortCircuit on hit
└────────┬────────┘
         │
         ▼  (when cache misses)
┌─────────────────┐
│ Routing         │  BasicRouter / CostAwareRouter / MLRouter / ABTestRouter
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Tenant limit    │  Model allowlist → daily / monthly cost caps
│ checks          │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Model           │  FallbackManager auto-fallback → LoadBalancer rotates API keys
│ invocation      │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Outbound        │  Six stages run in sequence
│ Pipeline        │
│  ① ContentFilter│  Harmful content filtering
│  ② Hallucination│  Hallucination detection scoring
│  ③ QualityScorer│  Four-dimensional output quality scoring
│  ④ CostTracker  │  Token cost calculation and recording
│  ⑤ AlertManager │  Threshold alert checks
│  ⑥ RequestLogger│  Final status logging
└────────┬────────┘
         │
         ▼
    Return response to client
```

### Pipeline Stage State Machine

Each `PipelineStage` returns one of the following states:

```
           ┌──────────┐
           │ Continue │─────────► Pass to next stage
           └──────────┘
           ┌──────────────┐
           │ ShortCircuit │─────► Skip remaining stages (cache hit)
           └──────────────┘
           ┌──────────┐
           │  Reject  │─────────► Reject request (security violation, return 403)
           └──────────┘
           ┌──────────┐
           │  Error   │─────────► Internal error (return 500)
           └──────────┘
```

## Routing System Architecture

AegisGate supports four routing strategies, switchable via configuration:

```
                    ┌─────────────────────────┐
                    │   Router interface      │
                    │  selectModel(ctx, reg)  │
                    └────────────┬────────────┘
                                 │
            ┌────────────────────┼────────────────────┐
            │                    │                    │
   ┌────────┴─────────┐ ┌───────┴────────┐ ┌────────┴────────┐
   │   BasicRouter    │ │CostAwareRouter │ │    MLRouter     │
   │                  │ │                │ │                 │
   │ Explicit model   │ │ Character-count│ │ 3D weighted     │
   │ → tag → default  │ │ tiers          │ │ scoring         │
   │     model        │ │ economy/premium│ │ cost×quality×   │
   └──────────────────┘ └────────────────┘ │ latency         │
                                           │ EMA updates     │
                                           └────────┬────────┘
                                                    │
                                           ┌────────┴────────┐
                                           │  ABTestRouter   │
                                           │ (decorator      │
                                           │  pattern)       │
                                           │                 │
                                           │ Wraps any Router│
                                           │ Deterministic   │
                                           │ traffic split   │
                                           │ hash % weight   │
                                           └─────────────────┘
```

### MLRouter Scoring Formula

```
CombinedScore = w_cost × CostScore + w_quality × QualityScore + w_latency × LatencyScore

Where:
  CostScore    = 1.0 − (model_cost − min_cost) / (max_cost − min_cost)
  QualityScore = success_rate (EMA-updated)
  LatencyScore = 1.0 − (avg_latency − min_latency) / (max_latency − min_latency)

Default weights: cost=0.4, quality=0.35, latency=0.25
```

### ABTestRouter Allocation Algorithm

```
slot = hash(experiment_name + request_id) % total_weight

variants:  [  model-a (weight=70)  |  model-b (weight=30)  ]
           [  slot 0-69 → model-a  |  slot 70-99 → model-b ]

The same request_id always maps to the same variant (deterministic)
```

## Core Sequence Diagrams

### Non-Streaming Request — Full Sequence

```
Client            ApiController     GatewayRuntime      InboundPipeline    Router         FallbackMgr      OutboundPipeline
  │                    │                  │                   │               │                │                  │
  │── POST /v1/chat ──►│                  │                   │               │                │                  │
  │                    │── processReq ───►│                   │               │                │                  │
  │                    │                  │── Auth+RateLimit──│               │                │                  │
  │                    │                  │                   │               │                │                  │
  │                    │                  │── execute ────────►│               │                │                  │
  │                    │                  │                   │── Audit ──►   │                │                  │
  │                    │                  │                   │── Inject ──►  │                │                  │
  │                    │                  │                   │── PII ────►   │                │                  │
  │                    │                  │                   │── Cache ──►   │                │                  │
  │                    │                  │                   │◄── Continue ──│                │                  │
  │                    │                  │◄── Continue ──────│               │                │                  │
  │                    │                  │                   │               │                │                  │
  │                    │                  │── selectModel ────────────────────►│                │                  │
  │                    │                  │◄── "gpt-4o-mini" ────────────────│                │                  │
  │                    │                  │                   │               │                │                  │
  │                    │                  │── costLimitCheck──│               │                │                  │
  │                    │                  │                   │               │                │                  │
  │                    │                  │── executeWithFallback ────────────────────────────►│                  │
  │                    │                  │                   │               │  ┌── OpenAI ──►│                  │
  │                    │                  │                   │               │  │◄── resp ────│                  │
  │                    │                  │◄── ChatResponse ──────────────────────────────────│                  │
  │                    │                  │                   │               │                │                  │
  │                    │                  │── reportOutcome ──────────────────►│ (MLRouter)    │                  │
  │                    │                  │                   │               │                │                  │
  │                    │                  │── cacheStore ─────│               │                │                  │
  │                    │                  │                   │               │                │                  │
  │                    │                  │── execute ────────────────────────────────────────────────────────────►│
  │                    │                  │                   │               │                │  ── ContentFilter │
  │                    │                  │                   │               │                │  ── Hallucination │
  │                    │                  │                   │               │                │  ── QualityScorer │
  │                    │                  │                   │               │                │  ── CostTracker   │
  │                    │                  │                   │               │                │  ── AlertManager  │
  │                    │                  │                   │               │                │  ── RequestLogger │
  │                    │                  │◄── Continue ──────────────────────────────────────────────────────────│
  │                    │                  │                   │               │                │                  │
  │                    │◄── ProcessResult─│                   │               │                │                  │
  │◄── JSON Response ──│                  │                   │               │                │                  │
```

### Streaming Request Sequence

```
Client            ApiController     GatewayRuntime      FallbackMgr       Provider
  │                    │                  │                   │               │
  │── POST (stream) ──►│                  │                   │               │
  │                    │── processStream─►│                   │               │
  │                    │                  │── Auth+Inbound ──►│               │
  │                    │                  │── selectModel ────│               │
  │                    │                  │                   │               │
  │                    │                  │── streamWithFallback ─────────────►│
  │                    │                  │                   │── SSE req ────►│
  │                    │                  │                   │               │
  │◄── SSE: chunk 1 ──│◄─── onChunk ────│◄── chunk ────────│◄── chunk ──────│
  │◄── SSE: chunk 2 ──│◄─── onChunk ────│◄── chunk ────────│◄── chunk ──────│
  │◄── SSE: chunk N ──│◄─── onChunk ────│◄── chunk ────────│◄── chunk ──────│
  │                    │                  │                   │               │
  │                    │                  │◄── onDone ───────│◄── [DONE] ────│
  │                    │                  │── outbound.exec──│               │
  │                    │                  │── cacheStore ────│               │
  │                    │                  │── reportOutcome ─│               │
  │◄── SSE: [DONE] ───│◄─── onDone ─────│                   │               │
```

### Cache Hit Sequence (Short-Circuit)

```
Client            GatewayRuntime      SemanticCache       Embedder
  │                    │                   │                  │
  │── request ────────►│                   │                  │
  │                    │── inbound.exec ──►│                  │
  │                    │                   │── embed(prompt) ─►│
  │                    │                   │◄── vector ───────│
  │                    │                   │── hnswlib search ─│
  │                    │                   │── similarity > θ ─│
  │                    │                   │                  │
  │                    │◄── ShortCircuit ──│                  │
  │                    │   ctx.cache_hit   │                  │
  │                    │   ctx.cached_resp │                  │
  │                    │                   │                  │
  │◄── cached resp ───│   (skips model call, zero API cost)   │
```

### Fallback and Fault-Tolerance Sequence

```
Client            GatewayRuntime      FallbackMgr       Primary          Fallback
  │                    │                  │                │                │
  │── request ────────►│                  │                │                │
  │                    │── execute ───────►│                │                │
  │                    │                  │── call ────────►│                │
  │                    │                  │◄── timeout ────│                │
  │                    │                  │                │                │
  │                    │                  │── call ─────────────────────────►│
  │                    │                  │◄── response ────────────────────│
  │                    │                  │                │                │
  │                    │◄── response ─────│                │                │
  │◄── response ──────│                  │                │                │
```

## Storage Architecture

```
┌──────────────────────────────────────────────────────────┐
│                 Application layer                        │
│  AdminController / CostTracker / AuditLogger / AuthService│
└─────────────────────────┬────────────────────────────────┘
                          │
            ┌─────────────┴─────────────┐
            │                           │
   ┌────────┴────────┐       ┌─────────┴─────────┐
   │   CacheStore    │       │  PersistentStore  │
   │   (KV + TTL)    │       │ (table-level ops) │
   └────────┬────────┘       └─────────┬─────────┘
            │                          │
      ┌─────┴─────┐            ┌──────┼──────┐
      │           │            │      │      │
   Memory     Redis         Memory  SQLite  PG
 (Community) (Enterprise)  (testing)(Community)(Enterprise)
```

## Security Guardrail Layers

```
       Request input
          │
          ▼
   ┌──────────────┐
   │ L1: Keywords │  Regex pattern matching (fastest, ~1μs)
   │  + heuristics│  Special-character density, role-switch detection
   └──────┬───────┘
          │ Pass
          ▼
   ┌──────────────┐
   │ L2: Encoding │  Secondary check after Base64/Hex/URL decode
   │  + Unicode   │  NFKC normalization against confusion attacks
   └──────┬───────┘
          │ Pass
          ▼
   ┌──────────────┐
   │ L3: ONNX     │  Neural network safety classification (optional, ~5ms)
   │    classifier│
   └──────┬───────┘
          │ Pass
          ▼
   ┌──────────────┐
   │ L4: PII      │  RE2 linear-time regex (ReDoS-safe)
   │    redaction │
   └──────┬───────┘
          │ Pass
          ▼
   ┌──────────────┐
   │ L5: Topic    │  Allowlist / blocklist
   │    boundary  │
   └──────┬───────┘
          │ Pass
          ▼
   ┌──────────────┐
   │ L6: Custom   │  YAML declarative rule engine (Enterprise)
   │    rules     │
   └──────┬───────┘
          │ Pass
          ▼
      Continue processing
```

## Cost Optimization Architecture

```
                    ┌──────────────────┐
                    │ Request ingress  │
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │ Semantic cache   │ ──── hit ──► Zero-cost response
                    │ interception     │
                    │ (skips duplicate │
                    │  API calls)      │
                    └────────┬─────────┘
                             │ miss
                    ┌────────▼─────────┐
                    │ Intelligent      │
                    │ routing          │
                    │                  │
                    │  ┌─ MLRouter ──┐ │  Pick optimal model dynamically
                    │  │ 3D scoring  │ │  from cost / quality / latency
                    │  └─────────────┘ │
                    │  ┌─ ABTest ────┐ │  A/B comparison for cost-
                    │  │ Traffic     │ │  performance; optimal model mix
                    │  │ split       │ │
                    │  └─────────────┘ │
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │ Tenant cost      │ ──── over limit ──► 429 reject
                    │ limits           │
                    │ (daily/monthly   │
                    │  caps)           │
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │ Model invocation │
                    └────────┬─────────┘
                             │
              ┌──────────────┼──────────────┐
              │              │              │
     ┌────────▼────┐  ┌─────▼─────┐  ┌────▼──────┐
     │ QualityScore│  │CostTracker│  │ Usage     │
     │ Quality     │  │ Cost      │  │ Predictor │
     │ feedback    │  │ records   │  │           │
     │ → MLRouter  │  │ → dashboard │ │ → trends  │
     └─────────────┘  └───────────┘  └───────────┘
```

For a detailed cost optimization guide, see the [Cost Optimization Guide](./cost-optimization.md).

## Enterprise Extension Architecture

```
   ┌────────────────────────────────────────────────┐
   │         Enterprise add-on components           │
   │                                                │
   │  ┌──────────┐  ┌──────────┐  ┌──────────────┐ │
   │  │   RBAC   │  │   SSO    │  │ Web admin    │ │
   │  │ Roles &  │  │OIDC/OAuth│  │ console      │ │
   │  │ perms    │  │          │  │ React SPA    │ │
   │  └──────────┘  └──────────┘  └──────────────┘ │
   │                                                │
   │  ┌──────────┐  ┌──────────┐  ┌──────────────┐ │
   │  │ Multi-   │  │   MFA    │  │ Rule engine  │ │
   │  │ tenant   │  │  TOTP    │  │ Hot reload   │ │
   │  │ isolation│  │          │  │              │ │
   │  └──────────┘  └──────────┘  └──────────────┘ │
   │                                                │
   │  ┌──────────┐  ┌──────────┐  ┌──────────────┐ │
   │  │PostgreSQL│  │  Redis   │  │ Compliance   │ │
   │  │ Persist- │  │  Cache   │  │ reports      │ │
   │  │ ence     │  │          │  │ CSV export   │ │
   │  └──────────┘  └──────────┘  └──────────────┘ │
   └────────────────────────────────────────────────┘
```

## Deployment Architecture

### Single-Node Deployment (Community)

```
   ┌─────────────────────────────┐
   │      Docker Container       │
   │                             │
   │  aegisgate binary           │
   │       │                     │
   │       ├── SQLite DB         │
   │       ├── ONNX models       │
   │       │   (optional)        │
   │       └── config/*.yaml     │
   │                             │
   └─────────────┬───────────────┘
                 │
          ┌──────┴──────┐
          │  Prometheus  │
          │  + Grafana   │
          └─────────────┘
```

### Cluster Deployment (Enterprise, planned)

```
                    ┌──────────┐
                    │   LB     │
                    └────┬─────┘
               ┌─────────┼─────────┐
               │         │         │
         ┌─────┴───┐ ┌───┴───┐ ┌──┴──────┐
         │AegisGate│ │AegisGa│ │AegisGate│
         │ Node 1  │ │ Node 2│ │ Node 3  │
         └────┬────┘ └───┬───┘ └────┬────┘
              │          │          │
         ┌────┴──────────┴──────────┴────┐
         │        Shared state          │
         │ Redis (cache + rate-limit    │
         │       state)                 │
         │ PostgreSQL (persistent data) │
         └──────────────────────────────┘
```

## Related Documents

- [Quick Start](./quick-start.md) — Build, configure, and first API call
- [Cost Optimization Guide](./cost-optimization.md) — Cost-saving strategies in detail
- [Admin API Reference](./admin-api.md) — Full Admin REST API reference
- [Error Codes](./error-codes.md) — AEGIS-xxxx error codes
- [Performance Tuning](./performance-tuning.md) — Cache, rate limiting, and threading
- [Security Best Practices](./security-best-practices.md) — Keys, TLS, and guardrail rules
