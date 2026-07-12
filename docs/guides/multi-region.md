# Multi-Region Routing Guide

> Feature: `GeoRouter` (Phase 9.1.1)
> Available: v1.1+ (delivered incrementally under the v3.0 Phase 9 roadmap)

This guide explains how to enable **region-aware routing (Geo-aware Routing)** in AegisGate. `GeoRouter` constrains the candidate models to those that match the caller's geographic region. It is implemented as a decorator — the underlying router (`BasicRouter` / `CostAwareRouter` / `MLRouter` / `ABTestRouter`) is not modified.

## Why region-awareness?

| Scenario | Pain | How `GeoRouter` helps |
|----------|------|----------------------|
| Cross-ocean round-trips | APAC users hitting a us-east endpoint add 150-250 ms latency | Infer caller's region and prefer same-region models |
| GDPR / data residency | EU traffic might be routed to non-EU providers | `residency: strict` enforces data residency |
| Multi-cloud deployments | Same model available in multiple regions | Tag models with `region:<name>` and filter accordingly |

## Quick start (3 steps)

### Step 1 — Tag each model with its region

Edit `config/models.yaml` and add a `region:<name>` to each model's `tags`:

```yaml
models:
  - id: gpt-4o-us
    provider: openai-us
    tags: ["region:us-east", "high-quality"]
  - id: gpt-4o-eu
    provider: openai-eu
    tags: ["region:eu-central", "high-quality"]
  - id: qwen-multi
    provider: local
    # A model may belong to multiple regions simultaneously.
    tags: ["region:us-east", "region:eu-central", "cheap"]
```

### Step 2 — Enable `routing.geo`

Edit `config/aegisgate.yaml`:

```yaml
routing:
  type: cost_aware        # underlying router is unchanged
  geo:
    enabled: true
    affinity: prefer      # strict | prefer | any
    default_client_region: us-east
    header_names:
      - X-AegisGate-Region
      - X-Client-Region
    ip_region_map:        # optional
      - cidr: 10.0.0.0/8
        region: us-east
      - cidr: 172.16.0.0/12
        region: eu-central
```

### Step 3 — (optional) let clients announce their region

The region may be declared by, in priority order:

1. `X-AegisGate-Region: eu-central` header
2. `X-Client-Region: eu-central` header
3. Request JSON `extra.client_region = "eu-central"`
4. Source IP matched against `ip_region_map` (injected by the HTTP layer into `extra.client_ip`)
5. `routing.geo.default_client_region`

## Affinity policies

| Policy | When no candidate matches | When base router picks a non-compliant model | Typical use |
|--------|--------------------------|----------------------------------------------|-------------|
| `strict` | Return empty (triggers fallback) | Force re-pick from the allowed set | Compliance, residency |
| `prefer` | Fall back to the full candidate set (warn) | Allow through (warn) | Performance-first |
| `any` | Fall back to the full set | Allow through | Pure observation |

## Residency

The tenant or the request itself can declare a **hard residency constraint** that overrides `affinity`:

```bash
curl -X POST https://api.example.com/v1/chat/completions \
  -H 'Authorization: Bearer YOUR_KEY' \
  -H 'X-AegisGate-Region: eu-central' \
  -H 'Content-Type: application/json' \
  -d '{
    "model": "gpt-4o-eu",
    "messages": [{"role":"user","content":"hi"}],
    "extra": { "residency": "strict" }
  }'
```

With `residency: strict`:
- Models without any `region:<name>` tag are excluded
- Even under `affinity=prefer`, non-compliant picks are force-rejected
- If no compliant candidate exists, the router returns empty (business layer handles fallback)

## Observability fields

When `enabled: true`, `GeoRouter` writes these fields to `chat_request.extra`:

| Field | Meaning |
|-------|---------|
| `_geo_client_region` | Inferred client region |
| `_geo_selected_region` | Region tag of the chosen model; `unknown` if the model has no region tag |
| `_geo_allowed_models` | Filtered candidate model IDs |

Downstream audit / trace / metrics stages can consume these to observe region-routing effectiveness.

## Troubleshooting

| Symptom | Check |
|---------|-------|
| GeoRouter not wrapping | Verify `routing.geo.enabled: true`; look for log `Router: wrapped with GeoRouter` |
| Everything fails under `strict` | Ensure target models have `region:<name>` tags; double-check `default_client_region` |
| CIDR mismatch | IPv4 only; IPv6 addresses automatically fall through to the next inference tier; ensure `client_ip` is injected by the HTTP layer |
| Region names not normalized | Add entries to `region_aliases`, e.g. `us: us-east` |

## Router composition order

```
Base (Basic / CostAware / ML)
  └─► ABTestRouter (optional)
        └─► GeoRouter (optional, outermost)
```

`GeoRouter` is always the **outermost** wrapper so that region compliance is the final gate before returning the selected model.

## Roadmap follow-ups

- **Phase 9.1.2** MaxMind GeoLite2 integration (replaces hand-written CIDR map)
- **Phase 9.1.3** Cross-region cache replication (CRDT LWW)
- **Phase 9.1.4** Hard-enforcement residency stage (rejects non-compliant writes)
