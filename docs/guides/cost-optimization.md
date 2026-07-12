# AegisGate Cost Optimization Guide

This document describes AegisGate’s cost-optimization stack and how teams can reduce AI API spend by roughly 30–60%.

## Cost optimization overview

AegisGate saves cost through five coordinated layers:

```
┌─────────────────────────────────────────────────────────────────┐
│              AegisGate cost-saving capability stack             │
│                                                                 │
│  Layer 1 — Semantic cache     Reuse identical/similar requests  │
│                               at zero marginal cost             │
│  Layer 2 — Guardrails         Malicious requests consume        │
│                               zero upstream tokens              │
│  Layer 3 — Smart routing      Optimize cost / quality / latency │
│  Layer 4 — Tenant cost caps   Hard daily/monthly limits;        │
│                               reject when exceeded              │
│  Layer 5 — Usage forecasting  Trend estimates + budget-runout    │
│                               signals                           │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## Layer 1: Semantic cache

### How it works

```
Request "What programming language is Python?"
        │
        ▼
┌──────────────────┐
│ Embedder         │  Turn text into vector representation
│ (Hash / ONNX)    │
└────────┬─────────┘
         │
         ▼
┌──────────────────┐
│ hnswlib ANN      │  Nearest-neighbor search in the vector index
│ search           │
└────────┬─────────┘
         │
    Similarity ≥ threshold?
    ┌────┴────┐
   YES       NO
    │         │
    ▼         ▼
 Return     Call model
 cache      (~2–5s)
 (~1ms)     Incur token cost
 Zero cost
```

### Configuration recommendations

```yaml
cache:
  threshold: 0.90          # similarity threshold (lower = higher hit rate but less precise)
  ttl_seconds: 3600        # cache entry TTL
  max_entries: 10000       # max cached entries
  context_aware: true      # multi-turn conversation awareness

  # adaptive threshold (auto-adjusted based on hit feedback)
  adaptive:
    enabled: true
    min_threshold: 0.85
    max_threshold: 0.98
    adjustment_rate: 0.01

  # selective caching (skip requests that are not suitable for caching)
  policy:
    enabled: true
    skip_models: ["dall-e-3"]       # generative models — do not cache
    max_temperature: 0.8            # high-randomness requests — do not cache
    skip_streaming: false
```

### Expected impact

- Repeated-query workloads (support / FAQ): cache hit rate 40–70%, saving 40–70% of invocation cost
- General chat: cache hit rate 10–30%
- ONNX embeddings are about 3–5× more accurate than Hash embeddings for semantic matching

## Layer 2: Guardrail interception

Requests blocked by guardrails **never reach upstream AI services**, so they consume zero tokens:

```
Malicious injection request ──► InjectionDetector ──► 403 Rejected (zero cost)
```

Typical cases:

- Prompt injection
- Encoding evasion attempts
- Out-of-scope topic requests
- Abuse (keys with many short-interval blocks are auto-throttled)

## Layer 3: Smart routing

### Routing strategy comparison

```
Strategy     │ How selection works              │ Best for
─────────────┼──────────────────────────────────┼──────────────────
basic        │ Explicit model → tag → default   │ Fixed model, no tuning
cost_aware   │ Three tiers by request char size │ Simple savings baseline
ml           │ Score cost × quality × latency   │ Many models, dynamic tuning
```

### MLRouter flow

```
Request arrives
    │
    ▼
┌──────────────────┐
│ Explicit model?  │──── yes ──► Use that model
└────────┬─────────┘
         │ no
         ▼
┌──────────────────┐
│ quality_tier     │
│ filter?          │
│                  │
│ economy: cheaper │
│   half of pool   │
│ premium: pricier │
│   half of pool   │
└────────┬─────────┘
         │
         ▼
┌──────────────────────────────────────┐
│ Score each candidate model           │
│                                      │
│  score = 0.4 × CostScore             │
│        + 0.35 × QualityScore         │
│        + 0.25 × LatencyScore         │
│                                      │
│  CostScore = normalized (cheapest=1, │
│              most expensive=0)       │
│  QualityScore = historical success   │
│              rate (EMA)              │
│  LatencyScore = normalized (fastest=1, │
│              slowest=0)                 │
└────────┬─────────────────────────────┘
         │
         ▼
┌──────────────────┐
│ Pick highest     │
│ score            │
└────────┬─────────┘
         │
         ▼
    Invoke model
         │
         ▼
┌──────────────────┐
│ reportOutcome    │  Feed back latency and success/failure
│ updates EMA      │  for more accurate routing next time
└──────────────────┘
```

### MLRouter configuration

```yaml
routing:
  type: ml
  ml:
    cost_weight: 0.4       # cost weight (higher = more cost savings)
    quality_weight: 0.35   # quality weight (higher = more focus on success rate)
    latency_weight: 0.25   # latency weight (higher = more focus on response speed)
```

**Savings-first** tuning:

```yaml
routing:
  type: ml
  ml:
    cost_weight: 0.6       # increase cost weight
    quality_weight: 0.25   # relax quality requirement
    latency_weight: 0.15   # relax latency requirement
```

### A/B test routing

A/B tests compare cost-effectiveness across models to find a better default mix:

```
┌──────────────────────────────────────────────────────┐
│        A/B experiment: gpt4o-vs-claude               │
│                                                      │
│  request_id  ──► hash(exp + id) % 100               │
│                                                      │
│  ┌─── 0~49 (50%) ──► gpt-4o ────────┐              │
│  │                                    │              │
│  │    Compare: cost, quality score,   │              │
│  │    latency                         │              │
│  │                                    │              │
│  └─── 50~99 (50%) ──► claude-sonnet ─┘              │
│                                                      │
│  Results written to ctx.ab_experiment /              │
│  ctx.ab_variant                                    │
│  Compare arms via CostTracker + QualityScorer      │
└──────────────────────────────────────────────────────┘
```

Example configuration:

```yaml
routing:
  type: cost_aware            # base routing strategy
  ab_tests:
    - name: "gpt4o-vs-claude"
      variants:
        - model: "gpt-4o"
          weight: 50
        - model: "claude-3-sonnet"
          weight: 50
      enabled: true
      tenant_id: ""           # empty = applies globally
    - name: "mini-vs-qwen"
      variants:
        - model: "gpt-4o-mini"
          weight: 70
        - model: "qwen-turbo"
          weight: 30
      enabled: true
      tenant_id: "tenant-dev" # applies only to tenant-dev
```

## Layer 4: Tenant cost limits

```
Request arrives
    │
    ▼
┌────────────────────────┐
│ Look up tenant spend   │
│ for current day        │
│ getTenantCostInPeriod  │
└────────┬───────────────┘
         │
    Spent ≥ daily cap?
    ┌────┴────┐
   YES       NO
    │         │
    ▼         ▼
429 reject   Continue
"Daily cost  processing
 limit       (same check for
 exceeded"   monthly cap)
```

Configure via Admin API or CLI:

```bash
./aegisctl tenant create \
  --name "team-dev" \
  --daily-cost-limit 50.0 \
  --monthly-cost-limit 1000.0
```

## Layer 5: Usage forecasting

### Forecasting algorithm

```
Historical data (daily aggregated cost)
    │
    ▼
┌──────────────────────┐
│ OLS linear regression │
│                      │
│  cost = slope × day  │
│       + intercept    │
│                      │
│  R² for goodness of  │
│  fit                 │
└────────┬─────────────┘
         │
    ┌────┴────┐
    │         │
    ▼         ▼
 Trend      Budget
 forecast    runout date
 next N days estimate
 cost path
```

### API usage

**Usage trend forecast:**

```bash
curl "http://localhost:8080/admin/api/predict/usage?\
tenant_id=team-dev&history_days=30&forecast_days=7" \
  -H "Cookie: aegis_session=..."
```

Response:

```json
{
  "daily_trend": 12.5,
  "r_squared": 0.87,
  "historical": [
    {"date": "2026-03-20", "total_cost": 45.2, "request_count": 1520},
    {"date": "2026-03-21", "total_cost": 58.7, "request_count": 1890}
  ],
  "predicted": [
    {"date": "2026-03-27", "total_cost": 71.2, "request_count": 0},
    {"date": "2026-03-28", "total_cost": 83.7, "request_count": 0}
  ]
}
```

**Budget runout estimate:**

```bash
curl "http://localhost:8080/admin/api/predict/budget?\
tenant_id=team-dev&budget=1000&history_days=30" \
  -H "Cookie: aegis_session=..."
```

Response:

```json
{
  "daily_trend": 12.5,
  "r_squared": 0.87,
  "budget": 1000.0,
  "budget_exhaustion_date": "2026-04-15",
  "historical": [...],
  "predicted": [...]
}
```

## Configuration reference (token path)

In addition to cache / routing / budgets, the gateway can shrink prompt and output size via the top-level `token_optimization` YAML block (see `config/aegisgate.yaml`):

```yaml
token_optimization:
  prompt_compression:
    enabled: true
    max_context_messages: 20
    compress_whitespace: true
    dedup_system_prompts: true
  smart_max_tokens:
    enabled: true
    default_max_output: 2048
    max_output_ratio: 2.0
    min_output_tokens: 100
```

| Key | Role |
|-----|------|
| `token_optimization.prompt_compression.*` | Caps history length, optional whitespace / system-prompt dedup before upstream |
| `token_optimization.smart_max_tokens.*` | Derives a bounded `max_tokens` instead of oversized fixed defaults |

These keys live under the top-level `token_optimization.*` root (see `config/aegisgate.yaml`). Cost caps that reject or downgrade when exceeded are configured under `budget_guard.*` (Layer 4).

## Cost optimization best practices

### 1. Enable semantic cache + ONNX embeddings

ONNX embeddings catch paraphrases with similar meaning (e.g. “What is Python?” vs. “Tell me about Python”); Hash embeddings only match near-identical text.

### 2. Prefer MLRouter over a single fixed model

Let the system pick models from live signals instead of sending everything to the most expensive option.

### 3. Run A/B tests before changing the default model

Quantify cost and quality deltas between two models before promoting one as default.

### 4. Set sensible cost caps

Per-tenant daily and monthly limits blunt surprise spend spikes.

### 5. Review forecasts regularly

Use `/admin/api/predict/budget` to spot budget pressure early.

### 6. Tune cache policy

- Disable or narrow caching for high temperature (>0.8)
- Skip cache for generative workloads (e.g. DALL-E)
- Raise cache TTL for support / FAQ style traffic

## Related documents

- [Architecture guide](./architecture.md) — System overview and component interactions
- [Admin API reference](./admin-api.md) — Prediction endpoint parameters
- [Performance tuning](./performance-tuning.md) — Cache and rate-limit settings
