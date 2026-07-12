# External Safety APIs and Shadow Mode Guide

> Feature: Phase 6.3 `ExternalSafetyStage` (OpenAI Moderation + Google Perspective) + shadow_mode (Epic 4)
> Available since: v1.2 (TASK-20260513-01 added shadow_mode)
> 中文版本：[`external-safety_zh.md`](external-safety_zh.md) — authoritative source

`ExternalSafetyStage` is the L4 cloud safety guardrail invoked after the
local PII / Topic / Guard stages. It calls cloud moderation APIs and
folds their verdicts into the guardrail chain.

## Sync mode (default)

```yaml
security:
  external_safety:
    enabled: true
    mode: any              # any | all | majority
    fail_policy: open
    parallel: true
    openai_moderation: { enabled: true, api_key: "${OPENAI_API_KEY}" }
    perspective:        { enabled: true, api_key: "${PERSPECTIVE_API_KEY}", threshold: 0.7 }
```

## Shadow mode (Epic 4)

```yaml
security:
  external_safety:
    enabled: true
    mode: any
    fail_policy: open
    shadow_mode: true
    shadow_max_inflight: 1000
    shadow_audit_ttl_seconds: 86400
    openai_moderation: { enabled: true, api_key: "${OPENAI_API_KEY}" }
```

Behaviour:

1. `process()` returns `Continue` immediately (< 10 ms regardless of
   provider latency).
2. `std::async` fires the provider scan; verdict is written to the
   audit log tagged `shadow=true`.
3. Hot path is never blocked.
4. New dispatches are skipped when `shadow_max_inflight` is reached
   (SR6 backpressure).

Startup log:

```
ExternalSafetyStage: L4 active with 1 provider(s), mode=any, fail=open, shadow=on (cap=1000, ttl=86400s)
```

## Recommended rollout

1. Week 1 — `shadow_mode: true`. Mine the audit log (`shadow=true`) for FP/FN ratios.
2. Week 2 — tune thresholds.
3. Week 3 — `shadow_mode: false`, real blocking.

## Security

- **SR3** — every shadow scan writes an audit entry; mutation removing
  the call fails `Sr3ShadowWritesAuditTaggedShadow` immediately.
- **SR6** — `shadow_max_inflight` caps in-flight workers; overflow is
  skipped + warn-logged.
- **fail_policy** is irrelevant under `shadow_mode: true` (process()
  always returns Continue).

## Tests

`tests/unit/guardrail/test_external_safety_shadow.cpp` ships 12 cases
including the Epic 4.3 integration assertions
`SlowProviderStaysUnderTenMs` (single 500 ms provider, hot path < 10 ms)
and `BurstStaysFireAndForget` (5 dispatches < 50 ms wall-clock).

## References

- Design: `docs/specs/2026-05-13-phase6-completion-design.md` §7
- Plan: `docs/plans/2026-05-13-phase6-completion.md` §7
