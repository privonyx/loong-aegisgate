# Multimodal Routing Guide

> Feature: Phase 6.1 multimodal API + ModalityRouter (CR2 scheme A — thin Handler + thick Router)
> Available since: v1.2 (TASK-20260513-01)
> 中文版本：[`multimodal_zh.md`](multimodal_zh.md) — authoritative source

This page mirrors the Chinese guide. Refer to it for the design tree
and FAQ.

## What it solves

The multimodal endpoints (`/v1/embeddings`, `/v1/images/generations`,
`/v1/audio/transcriptions`, `/v1/audio/translations`, `/v1/audio/speech`,
`/v1/moderations`) used
to be raw passthroughs with no per-modality cost attribution, no
per-modality rate limiting, and no policy when multiple providers
covered the same modality.

| Component | File |
|---|---|
| `Modality` enum + `ModalityRouter` | `src/multimodal/modality.{h,cpp}` + `modality_router.{h,cpp}` |
| 5 OpenAI{Modality}Handler | `src/multimodal/openai_modality_handlers.{h,cpp}` |
| `OpenAIModalityUpstreamAdapter` | `src/multimodal/openai_modality_upstream_adapter.h` |
| `CostTracker.modality` dimension | `src/observe/cost_tracker.{h,cpp}` |
| `ModalityRateLimiter` | `src/multimodal/modality_rate_limiter.{h,cpp}` |

## Enable

```yaml
multimodal:
  enabled: true
  policy: cheapest          # cheapest | round_robin | fastest_p99
  cost_attribution:
    enabled: true
  rate_limit:
    enabled: true
    quotas:
      - modality: image_gen
        identity: "*"
        max_tokens: 60
        refill_rate: 1.0
```

> ⚠️ ModalityRateLimiter is wired and yaml quotas are loaded, but the
> per-request enforcement in `processProxyRequest` ships in a follow-up
> Epic 5.1c. Configured quotas are not lost.

## RoutingPolicy

| Strategy | Selection | Use case |
|---|---|---|
| `cheapest` | min `estimateCost(req)` | cost-sensitive, multi-provider |
| `round_robin` | monotonic index modulo | fair share / canary |
| `fastest_p99` | lowest historical p99 | UX-first |

N=1 short-circuits to `front()` (O(1)). N=0 → router returns null →
legacy ConnectorRegistry path.

## Security

- **SR5** — `CostTracker.modality` queries respect existing RBAC.
- **SR6** — `ModalityRateLimiter` decorates the existing token bucket
  with prefixed keys (`modality:<name>:<identity>`); reuses the 429
  reject path so no new bypass surface is introduced.

## References

- Design: `docs/specs/2026-05-13-phase6-completion-design.md` §6
- Creative: `memory-bank/creative/creative-modality-handler.md`
- Plan: `docs/plans/2026-05-13-phase6-completion.md` §5
