# Adaptive Guard (Phase 11.1) Guide

> Feature: Learning guardrail (L5) on top of the existing L1-L4 stack
> Available: v1.1+ (TASK-20260523-01)

Adaptive Guard turns the existing four-layer guardrail (`L1 regex` / `L2
RuleEngine` / `L3 ONNX classifier` / `L4 ExternalSafetyStage`) into a
**learning** system. Reviewers submit signed feedback through a dedicated
admin API, the runtime collects training rows in process, and operators
promote new candidate models through the same `AutonomyApprovalWorkflow`
that Phase 11.5 introduced for cost autonomy. The whole loop reuses
existing primitives (`PIIFilter`, `AuditLogger`, `FeedbackBus`,
`IApprovalApplier`) instead of inventing parallel storage or wiring.

This guide covers seven topics: the high-level model, the runtime
architecture, the three admin endpoints, the model registry semantics,
the feedback loop with its anti-poisoning defenses, the promotion
workflow, and the security / operational handles.

---

## 1. Overview

The runtime exposes three admin endpoints on the same Drogon HTTP layer
that already serves `/admin/api/autonomy/*`:

| Endpoint | Verb | Purpose |
|----------|------|---------|
| `/admin/api/guard/feedback` | POST | Reviewer submits FP / FN / confirmed_block labels |
| `/admin/api/guard/explanation/{request_id}` | GET | Operator inspects why a request was blocked |
| `/admin/api/guard/model/promote` | POST | Operator proposes promoting a shadow model to live |

Feedback flows through a three-stage path (PII mask -> audit log -> feedback
bus). A separate trainer process consumes the bus output and produces new
model candidates. Promotion proposals always travel through
`AutonomyApprovalWorkflow`, the same governance plane Phase 11.5 added to
the cost track. The kill switch `AEGISGATE_DISABLE_AUTONOMY` short-circuits
both the workflow and the applier (defense-in-depth).

---

## 2. Architecture

Five logical layers participate in the loop. Each one is wired by
`GatewayRuntime::initialize` so a single config flag enables the entire
plane.

### 2.1 Five Layers

1. **Guardrail stages (L1-L4)** keep emitting `InjectionResult` /
   `RuleResult` / `GuardResult` as before. The new
   `GuardExplanationBuilder` translates each one into the canonical
   `GuardExplanation` payload (seven stable fields).
2. **Feedback ingestion** lives in `GuardAdminController::postFeedback`.
   Role allowlist -> rate limiter -> anomaly detector -> sink, all in
   one short call.
3. **Model registry** stores `(model_id, version, status, artifact_sha256,
   created_at)` rows. `MemoryGuardModelRegistry` runs in production by
   default; `SQLiteGuardModelRegistry` is wired in for the cold path.
4. **Trainer** is an in-process collector. It subscribes to the
   `guard.feedback` topic, snapshots rows to JSONL, and hands them off to
   an out-of-band sidecar that produces the next candidate model.
5. **Applier** is `GuardModelApplier` (`AutonomySource::AdaptiveGuard`).
   It implements the four-method `IApprovalApplier` contract and reuses
   the workflow's audit / rollback / dry-run machinery.

---

## 3. API Reference

All three endpoints require an authenticated admin session. The HTTP
layer translates `Role::SuperAdmin` to `security_admin` and
`Role::TenantAdmin` to `platform_admin` before forwarding to
`GuardAdminController`, which enforces the canonical reviewer allowlist
`{security_admin, platform_admin, trust_safety}`.

### 3.1 POST /admin/api/guard/feedback

Submit a reviewer label. The request body is a JSON object with these
fields: `request_id`, `trace_id`, `label` (one of `false_positive`,
`false_negative`, `confirmed_block`), `reviewer_user_id`,
`reviewer_role`, `comment` (free text, PII-masked at ingest),
`original_text_redacted` (optional). Successful submissions return HTTP
200 with `{accepted: true, request_id}`. Common error codes:
`AEGIS-1002` (role denied), `AEGIS-2001` (rate limit), `AEGIS-5001`
(malformed body, including anomaly-flagged feedback).

### 3.2 GET /admin/api/guard/explanation/{request_id}

Returns the structured `GuardExplanation` object recorded when the
request was blocked, including `trigger_layer`, `trigger_rule_id`,
`model_version`, `threshold`, `matched_pattern`, `confidence`,
`explanation_text`. Returns `AEGIS-6003` (re-used `ApprovalNotFound`)
when the request id is unknown.

### 3.3 POST /admin/api/guard/model/promote

Creates an `ApprovalProposal` on the autonomy workflow. The body must
include `action` (`promote_shadow_to_live` or `revert_to_previous`),
`model_id`, `version`, and a `shadow_metrics` object with `win_rate`,
`shadow_duration_min`, and `fp_rate_delta`. The response carries the
proposal id; promotion only takes effect after the workflow transitions
through `APPROVED` and the applier returns `ok`. The kill switch is
checked twice: once on `propose`, once inside `GuardModelApplier::apply`.

---

## 4. Model Registry

The registry is a typed table that records every candidate model the
trainer ever produced, plus the single Live model that is currently
serving traffic.

### 4.1 State Machine

Allowed transitions: `Shadow -> Live` (demotes the previous Live row),
`Live -> Retired` (revert), `Retired -> Retired` (idempotent). All other
transitions are rejected as `illegal_transition`. The `Live` invariant is
enforced **twice**: in C++ by `MemoryGuardModelRegistry::promote` taking
the registry-wide mutex before swapping the status fields, and in SQLite
by a partial `UNIQUE INDEX ... WHERE status = 'live'`. The schema-level
`CHECK (status IN ('shadow','live','retired'))` makes hand-crafted
INSERTs impossible to break.

### 4.2 Storage

Two storage backends ship with v1: `MemoryGuardModelRegistry` (in-process,
zero IO, used by default and by every unit test) and
`SQLiteGuardModelRegistry` (file-backed, schema migrations on startup,
prepared statements + `BEGIN IMMEDIATE` transactions for promotion).
Postgres mirroring is tracked as v2 work (TASK-W17).

---

## 5. Feedback Loop & Anomaly Detection

The feedback path is hostile-input territory. v1 ships three independent
defenses; each one is anchored in `tests/rules/test-phase11.1-sr-presence.sh`
and verified by a dedicated mutation test.

### 5.1 Sink

`GuardFeedbackSink::ingest` is the only entry point into the trainer
pipeline. It always masks `comment` and `original_text_redacted` through
`PIIFilter::mask` before writing anything; it then logs the masked payload
to `AuditLogger` (action `guard_feedback`, stage `AdaptiveGuard`) so the
chain hash carries the audit forward; finally it publishes to the
`guard.feedback` topic on `FeedbackBus`. Missing audit logger -> hard
fail (`audit_unavailable`); missing PII filter -> still works but logs a
warning.

### 5.2 Rate Limiter

`GuardFeedbackRateLimiter` runs three sliding-window counters (1 minute,
strict) keyed by tenant, reviewer, and a global bucket. Default
allowances: 100 per tenant per minute, 30 per reviewer per minute, 10000
globally per minute. Rejections surface as HTTP 429 with stable
`reject_reason` strings (`tenant_quota_exceeded`,
`reviewer_quota_exceeded`, `global_quota_exceeded`).

### 5.3 Anomaly Detector

`GuardFeedbackAnomalyDetector` tracks per-reviewer `false_positive`
counts in a one-hour sliding window (default threshold: 50). Once a
reviewer crosses the threshold, future submissions return HTTP 409 with
`error_code = reviewer_fp_burst`, and an audit row
(`action = feedback_anomaly_flag`) is written. The trainer corpus stays
clean because the sink is never reached.

---

## 6. Promotion Workflow

Candidate models never go live unattended. Even the auto-approve path
re-uses `AutonomyApprovalWorkflow` so audit + kill-switch + rollback
behave identically across phases.

### 6.1 isLowRisk Rules

`GuardModelApplier::isLowRisk` evaluates four rules in order; all four
must pass for the workflow to auto-approve:

1. **R1**: action is not `revert_to_previous` (reverts always need a
   human signature).
2. **R2**: shadow `win_rate >= 0.55`.
3. **R3**: `shadow_duration_min >= 60` (one hour minimum bake-in).
4. **R4**: `fp_rate_delta >= -0.10` (the candidate's false-positive rate
   cannot regress more than ten percentage points).

Mutation M6 verifies the R1 branch; the SR drift test pins the
`return false;` branch count at five (one schema, four rules).

### 6.2 Kill Switch

`AEGISGATE_DISABLE_AUTONOMY=1` shuts down the entire autonomy plane. The
workflow refuses to enqueue new proposals (`AEGIS-6002 AutonomyDisabled`)
and `GuardModelApplier::apply` short-circuits with the same reason — the
double check exists so out-of-band callers (test fixtures, future
schedulers) cannot bypass the workflow.

---

## 7. Security & Operations

Adaptive Guard ships with a thin operational surface: the same audit
log, the same metrics taxonomy, the same kill switch. No new
dependencies, no new ports, no new sidecars in v1.

### 7.1 Audit Trail

Every accepted feedback, every anomaly flag, every promotion proposal,
every apply outcome lands in `AuditLogger` and carries the chained hash.
`SR5` is verified by mutation M4 (skipping the audit call must fail the
test). Operators can query `/admin/api/audits` with `stage=AdaptiveGuard`
to pull the full chain.

---

## See Also

- [Cost Autonomy & Approval Workflow Guide](./cost-autonomy.md) — the
  same `AutonomyApprovalWorkflow` plane is shared across phases.
- [Feedback Bus Guide](./feedback-bus.md) — topic semantics and
  delivery guarantees behind `guard.feedback`.
- `docs/specs/2026-05-23-phase11.1-adaptive-guard-design.md` — design
  spec with STRIDE threat model and SR-NEW1 / SR-NEW2 four-tuples.
