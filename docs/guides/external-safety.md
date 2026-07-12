# External Safety APIs and Shadow Mode Guide

> Feature: Phase 6.3 `ExternalSafetyStage` (OpenAI Moderation + Google Perspective) + shadow_mode (Epic 4)
> Available since: v1.2 (TASK-20260513-01 added shadow_mode)
> 中文版：[`external-safety_zh.md`](external-safety_zh.md)

`ExternalSafetyStage` is AegisGate's L4 content-safety guardrail. It calls cloud
moderation APIs (OpenAI Moderation, Google Perspective) after the local
PII / Topic / Guard stages and folds their verdicts into the guardrail chain.

## What it solves

- Regulatory / customer compliance that "cloud Moderation must be called"
- Different cloud providers disagree markedly on false positives / false negatives for the same text, so multi-provider voting is needed
- **When onboarding a new cloud provider, observe its verdicts for 1-2 weeks before wiring it into the real Reject path** — this is the `shadow_mode` added in Epic 4

## Sync mode (default)

```yaml
security:
  external_safety:
    enabled: true
    mode: any              # any | all | majority — when to trigger Reject
    fail_policy: open      # open: allow on API failure; closed: reject on API failure
    parallel: true         # call multiple providers concurrently
    openai_moderation:
      enabled: true
      api_key: "${OPENAI_API_KEY}"
    perspective:
      enabled: true
      api_key: "${PERSPECTIVE_API_KEY}"
      threshold: 0.7
```

Behaviour: `process()` calls each enabled provider serially/concurrently; with
`mode=any`, a single flagged verdict triggers Reject.

## Shadow mode (Epic 4)

```yaml
security:
  external_safety:
    enabled: true
    mode: any
    fail_policy: open
    shadow_mode: true              # ← the key switch
    shadow_max_inflight: 1000      # SR6 backpressure cap
    shadow_audit_ttl_seconds: 86400 # SR3 audit retention 24h
    openai_moderation:
      enabled: true
      api_key: "${OPENAI_API_KEY}"
```

Behaviour changes:

1. `process()` **returns Continue immediately** (< 10 ms, independent of actual provider latency)
2. Providers are called asynchronously via `std::async`; results are written to the audit log tagged `shadow=true`
3. The main request path is never blocked and never Rejects due to a slow / failing provider
4. When concurrent in-flight shadow workers exceed `shadow_max_inflight`, new dispatches are skipped (SR6 backpressure)

Startup log:

```
ExternalSafetyStage: L4 active with 1 provider(s), mode=any, fail=open, shadow=on (cap=1000, ttl=86400s)
```

## Recommended rollout

1. **Week 1** `shadow_mode: true` — go live, pull `shadow=true`-tagged verdicts from the audit log, measure FP/FN ratios
2. **Week 2** tune thresholds (model-level for OpenAI, `threshold` for Perspective)
3. **Week 3** `shadow_mode: false`, switch to real blocking

You can **roll back to shadow** at any stage without affecting production traffic.

## Security (SR3 + SR6)

- **SR3 shadow audit**: every shadow scan writes an audit entry, `action="external_safety_shadow"`, `detail=shadow=true; <provider>:flagged|ok|error;...`, with the `tenant_id` field preserved. Ops and compliance can compare shadow vs real-mode verdicts through the audit log.
- **SR6 inflight backpressure**: default 1000 concurrent shadow workers; beyond that, dispatch is skipped plus a warn log — threads never pile up unbounded.
- **fail-open + shadow_mode do not cancel each other**: under shadow_mode, `process()` always returns Continue regardless of `fail_policy`; `fail_policy` only takes effect once you switch back to real sync mode.

## Mutation evidence

- Disabling the `audit_logger->logAction` call → the SR3 test `Sr3ShadowWritesAuditTaggedShadow` fails immediately (verified in Epic 4 exit Checkpoint 4).
- Setting `shadow_max_inflight` to 0 → the SR6 test `ConfiguredMaxInflightHonored` verifies every dispatch is skipped.

## FAQ

- **Does shadow_mode increase cost?** Yes. Providers are still really called; only their verdicts don't affect the main path. Monitor the `shadow_dispatched` / `shadow_skipped` counters to assess the extra cost.
- **Can shadow be enabled for only some tenants?** Not currently; `ExternalSafetyStage` is a global stage. You can filter by tenant in a plugin layer.
- **Is there backpressure on the shadow path?** Yes. `shadow_max_inflight` is a hard cap; requests beyond it still get an immediate Continue from `process()`, just without an audit record.

## Verification

- `tests/unit/guardrail/test_external_safety_shadow.cpp` has 12 tests covering:
  - `ShadowDisabledKeepsSyncBlocking` (default behaviour unchanged)
  - `ShadowReturnsContinueImmediately` (< 20 ms with a 30 ms provider)
  - `Sr3ShadowWritesAuditTaggedShadow` (SR3 audit tag)
  - `Sr6InflightCapSkipsOverflow` (SR6 backpressure)
  - `SlowProviderStaysUnderTenMs` (Epic 4.3 integration: 500 ms provider, < 10 ms process)
  - `BurstStaysFireAndForget` (Epic 4.3: 5-burst < 50 ms)
