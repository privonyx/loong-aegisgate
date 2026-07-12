# AegisGate Savings Dashboard 用户指南

The Savings Dashboard is the admin-panel module that visualises how much cost
AegisGate has saved you across its three core cost-optimisation capabilities:

1. **Semantic cache hits** — short-circuiting to a cached response avoids the
   upstream LLM call entirely.
2. **Prompt compression** — `PromptCompressor` shrinks the prompt before sending
   to the upstream model, reducing input tokens billed.
3. **Intelligent routing (potential)** — `CostOptimizer` recommends switching
   to a cheaper model when telemetry suggests it is safe to do so.

Use this dashboard to:

- Justify AegisGate's deployment ROI to operations / finance teams.
- Drive sales demos with real per-tenant savings figures.
- Help end users / developers understand which optimisation paths are firing.

---

## Access

| Role          | Scope                              |
|---------------|------------------------------------|
| `SuperAdmin`  | Global view + `top_tenants` Top 10 |
| `TenantAdmin` | Own-tenant only, no leaderboard    |
| `Viewer`      | Own-tenant, read-only              |

Navigate to **AegisGate Admin Panel → Sidebar → 省钱 (Savings)**, or go directly
to `/admin/savings`. The "已节省（近30天）" KPI card on the Dashboard also links
here.

---

## Time windows

The page supports four built-in windows; the URL parameter `from` / `to` use
ISO 8601 UTC timestamps when calling the API directly.

| Selector | from / to behaviour                       |
|----------|-------------------------------------------|
| 近24小时 | `now - 24h` → `now`                       |
| 近7天    | `now - 7d` → `now` (default)              |
| 近30天   | `now - 30d` → `now`                       |
| 全部     | empty → backend serves `since aggregator` |

> **DoS safeguard (SR-NEW3)**: time ranges greater than 365 days are rejected
> with `400 InvalidRequest`.

---

## API

```
GET /admin/api/savings/summary?from=<iso>&to=<iso>&tenant_id=<id>
Cookie: aegis_session=<jwt>
```

Response shape (`SavingsSummary`):

```json
{
  "from": "2026-05-03T00:00:00Z",
  "to": "2026-05-10T00:00:00Z",
  "aggregator_since": "2026-05-01T08:00:00Z",
  "total_cost_saved": 12.34,
  "total_cost_actual": 100.00,
  "roi_percent": 11.0,
  "total_tokens_saved": 12345,
  "total_cache_hits": 50,
  "fallback_pricing_count": 2,
  "by_type": [
    { "type": "cache_hit", "cost_saved": 8.0, "tokens_saved": 8000, "event_count": 50 },
    { "type": "compression", "cost_saved": 4.34, "tokens_saved": 4345, "event_count": 100 }
  ],
  "by_model":     [ ... ],
  "time_series":  [ ... ],
  "top_tenants":  [ ... ],
  "routing_recommendations": [ ... ]
}
```

`top_tenants` is empty for non-SuperAdmin roles by design (SR1 multi-tenant
isolation).

---

## How savings are calculated

Each event recorded in the in-process `SavingsAggregator` carries
`tokens_saved` and `cost_saved` for one of the three sources:

| Source                | tokens_saved                                      | cost_saved                          | Notes |
|-----------------------|---------------------------------------------------|-------------------------------------|-------|
| `cache_hit`           | `request.tokens_estimated + estimate(cached_response)` | `CostTracker.calculate(model, in, out)` | output tokens estimated by `TokenEstimator::estimateTokens` |
| `compression`         | `tokens_saved_compression` (input only)           | `CostTracker.calculate(model, saved, 0)` | only counts input-side savings |
| `routing_potential`   | 0 (potential only)                                | `CostOptimizer.potential_savings`   | not yet realised; flagged separately |

When `CostTracker.calculate` returns `0` because pricing is missing (e.g. a
custom model not in `models.yaml`), the aggregator marks
`fallback_pricing=true` so the UI can warn the operator. The "算法说明" panel on
the Savings page shows this count and the affected models.

---

## Persistence model

> **The aggregator is process-local and resets on restart.** This is by design
> for v1: it adds zero schema migrations and no extra DB writes on the hot
> path.

The Savings page renders the `aggregator_since` timestamp at the top so users
know which historical window the figures represent. For long-term trend
analysis, use the `cost_records` table (queried via `/admin/api/costs`) which
*is* persistent — the Savings module specifically reports avoided cost which
is not in `cost_records` by definition.

A future iteration may persist savings events to dedicated DB tables; the
current API contract is forwards-compatible (clients see the same JSON
shape).

---

## Security considerations

| Concern                 | Mitigation                                                         |
|-------------------------|---------------------------------------------------------------------|
| Cross-tenant disclosure | `requireTenantAccess` + non-SuperAdmin `top_tenants=[]` (SR1)       |
| DoS via huge time range | 365-day cap returns `400 InvalidRequest` (SR-NEW3)                  |
| Hot-path stability      | `recordCacheHit` / `recordCompression` are `noexcept` (SR-NEW4)     |
| Pricing data trust      | `fallback_pricing_count` exposed in UI (SR-NEW1, transparency)      |

See `docs/specs/2026-05-10-admin-savings-dashboard-design.md` §4.3 for the
full STRIDE threat model (T01–T10).

---

## Troubleshooting

**The page shows zero savings everywhere.**
The aggregator initialises on `GatewayRuntime::initialize()` after
`CostTracker.loadPricing()`. If pricing failed to load (check
`models.yaml` path), every record will be flagged
`fallback_pricing=true` with `cost_saved=0`. Token counts still increment.

**`top_tenants` is empty.**
Either you are not `SuperAdmin`, or no tenants have produced savings events
yet within the selected window. Switch the time window to "全部 (since
aggregator)" to verify.

**`aggregator_since` is `null`.**
The deployment was started before this feature shipped, or
`SavingsAggregator` failed to construct (check spdlog for warnings around
`cost_tracker` initialisation).
