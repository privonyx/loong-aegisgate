# Multimodal Routing Guide

> Feature: Phase 6.1 multimodal API + ModalityRouter (CR2 scheme A — thin Handler + thick Router)
> Available since: v1.2 (TASK-20260513-01)
> 中文版：[`multimodal_zh.md`](multimodal_zh.md)

This guide explains how AegisGate brings multimodal requests (embedding /
image generation / audio transcription / audio translation / speech synthesis /
moderation) onto the same guardrail / billing / audit path as regular chat
completions.

## What it solves

The multimodal endpoints (including `/v1/audio/translations`) used to be raw
passthroughs, lacking:

- Per-modality cost attribution (no way to tell how much was spent on image_gen this month)
- Per-modality rate limiting (one client suddenly pushing 10k images could overwhelm the whole gateway)
- Policy-based routing across providers ("I have both OpenAI and Voyage for embeddings — pick the cheaper one")

The upgrade solves all three through one set of interfaces:

| Component | File |
|---|---|
| `Modality` enum + `ModalityRouter` | `src/multimodal/modality.{h,cpp}` + `modality_router.{h,cpp}` |
| 5 × `OpenAI{Embedding,ImageGen,AudioTranscribe,AudioSpeech,Moderation}Handler` | `src/multimodal/openai_modality_handlers.{h,cpp}` |
| `OpenAIModalityUpstreamAdapter` (wraps the existing `OpenAIConnector`) | `src/multimodal/openai_modality_upstream_adapter.h` |
| `CostTracker.modality` dimension | `src/observe/cost_tracker.{h,cpp}` |
| `ModalityRateLimiter` (per-modality token bucket) | `src/multimodal/modality_rate_limiter.{h,cpp}` |

## Enable

```yaml
multimodal:
  enabled: true
  policy: cheapest          # cheapest | round_robin | fastest_p99
  cost_attribution:
    enabled: true           # record the CostTracker.modality dimension (on by default)
  rate_limit:
    enabled: true
    quotas:
      - modality: image_gen
        identity: "*"        # "*" is the global default
        max_tokens: 60
        refill_rate: 1.0     # 60 tokens / 60 s ≈ 1 req/s
      - modality: image_gen
        identity: "tenant-acme"
        max_tokens: 10
        refill_rate: 0.0028  # ≈ 10 / hour, a stricter targeted limit
```

Startup log:

```
ModalityRouter: wired with 5 OpenAI handlers (policy=cheapest)
ModalityRateLimiter: 2 quotas configured (per-request enforcement deferred to Epic 5.1c)
```

> ⚠️ The current version (TASK-20260513-01) already wires `ModalityRateLimiter`
> and loads the yaml quotas, but the per-request quota check on the
> `processProxyRequest` path is deferred to a follow-up (Epic 5.1c). Configured
> quotas are not lost — they are simply not enforced yet.

## Decision tree: should you enable it

| Situation | Recommendation |
|---|---|
| Chat completion only, no multimodal need | `multimodal.enabled: false` (default), no impact at all |
| You use any of the 5 multimodal endpoints | `enabled: true`, at minimum you get modality attribution + visible startup logs |
| Multiple providers cover the same modality | Set `policy`: `cheapest` for cost, `fastest_p99` for observed latency, `round_robin` for fair load |
| Worried a modality gets abused | Set `rate_limit.quotas`, use `identity: "*"` for a global default, then override problem tenants individually |

## RoutingPolicy behavior

| Strategy | Selection rule | Use case |
|---|---|---|
| `cheapest` | handler with the smallest `estimateCost(req)` | most cost-sensitive; significant per-provider price gaps |
| `round_robin` | monotonically increasing index modulo N | fair share; canary-introducing a new provider |
| `fastest_p99` | lowest historical p99 latency | UX-first; trade a little cost for stability |

When N=1 every policy short-circuits to `front()` — O(1), no policy overhead. When
N=0 the router returns a null pointer and the request falls back to the legacy
ConnectorRegistry.

## Security (SR5 + SR6)

- **SR5 RBAC modality view**: queries on the `CostTracker.modality` dimension respect existing RBAC — SuperAdmin sees the global aggregate, TenantAdmin/Viewer see only their own tenant. The admin Savings page previews modality attribution (`web/admin/src/pages/Savings.tsx`).
- **SR6 no widened rate-limit bypass surface**: `ModalityRateLimiter` is a thin decorator over the `RateLimiter` token bucket with key prefix `modality:<name>:<identity>`, so it never collides with existing IP/tenant keys; over-limit reuses the RateLimiter 429 path rather than inventing a new reject signal.

## Verification

- `ctest` (CP+ON) runs `ModalityTest`, `ModalityRouterTest`, `ModalityHandlersTest`, `ModalityRateLimiterTest`, and `CostTrackerModalityTest`.
- After startup, calling `POST /v1/embeddings` should show the `ModalityRouter:` handler-selection log (DEBUG level) and populate the `modality` field of `CostTracker`.

## FAQ

- **Is there a performance hit vs the old passthrough when N=1?** Almost none. The router fast path is `vector::front()` plus one virtual call — O(1).
- **Are the yaml quotas of `ModalityRateLimiter` enforced today?** Not yet (pending Epic 5.1c). But quotas are read and summarized in the logs.
- **Can I use a provider other than OpenAI?** The wiring code currently hardcodes `findByProvider("openai")`. To support another provider, extend the modality wire section of `GatewayRuntime::initialize` (add an adapter following the OpenAI adapter pattern).
