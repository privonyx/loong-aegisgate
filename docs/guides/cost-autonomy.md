# Cost Autonomy & Approval Workflow Guide

> Feature: Cross-cutting autonomy approval workflow + budget guard (Phase 11.5)
> Available: v1.1+ (TASK-20260518-02)

Cost Autonomy is the **decision-governance plane** for every autonomy-driven
change that touches money. Until now, modules like CostOptimizer v1 emitted
recommendations as read-only reports — operators still had to copy/paste
recommended models into config and reload. Phase 11.5 closes that loop:

- **CostOptimizer 2.0** now `propose()`s its decisions to a workflow that
  audits and routes every transition (PROPOSED → APPROVED → APPLIED).
- **BudgetGuardStage** sits in the inbound pipeline between `ResolveStage`
  and the router, fail-closes per-tenant 24h spend caps, and downgrades the
  request to the `economy` quality tier before any upstream call happens.
- **AutonomyApprovalWorkflow** is the cross-cutting funnel that future
  autonomy phases (AdaptiveGuard, AutoRecovery, BanditRouter, Workflow 2.0)
  will reuse, so all "propose → approve → apply → rollback" lifecycles
  share one audit trail, one kill switch, and one Admin UI.

This guide covers seven topics: the high-level model, the state machine,
the three `auto_apply` modes, the Budget Guard knobs, the FinOps Admin UI,
how to troubleshoot common error codes, and the migration path from the
legacy CostOptimizer v1.

---

## 1. Overview

Three independent YAML flags drive the whole feature set. All default to
`false`, so a legacy `aegisgate.yaml` keeps its exact behaviour after the
upgrade:

```yaml
autonomy:
  enabled: false                 # master gate for the approval workflow
  auto_apply_mode: manual_only   # manual_only | auto_low_risk | auto_all
  proposal_retention_days: 90
  cost_optimizer:
    enabled: false               # CostOptimizer v2 propose() path

budget_guard:
  enabled: false                 # inbound stage that enforces caps
  per_tenant_24h_usd: 100.0
  per_request_max_usd: 1.0
  fail_open_on_error: true
  downgrade_tier: economy
```

The `AEGISGATE_DISABLE_AUTONOMY=1` environment variable is a global kill
switch that wins over both YAML flags and the in-memory override — see
"Troubleshooting" below.

What you get when everything is on:

| Surface | Behaviour |
|---|---|
| `CostOptimizer::proposeRecommendations()` | Returns advisories AND submits each to the workflow as a `PROPOSED` proposal. |
| `BudgetGuardStage` | Stamps `quality_tier=economy` + sets `X-AegisGate-Budget-Guard=triggered` whenever the 24h spend cap or per-request max would be exceeded. |
| `AutonomyApprovalWorkflow` | Owns the 5-state lifecycle, payload SHA-256 integrity check (T01 defence), per-source applier dispatch (currently CostAutonomyApplier), and audited transitions. |
| Admin UI `/admin/finops` | Lists proposals, filters by state/source, shows payload + `decision_trace`, lets a TenantAdmin approve / reject / rollback with confirmation modals. |
| API `/admin/api/autonomy/*` | 5 endpoints: list, approve, reject, delete (rollback), report. All audited. |

---

## 2. State machine

```
PROPOSED ──approve()──► APPROVED ──apply()──► APPLIED ──rollback()──► ROLLED_BACK
   │                      │                     ▲
   │ reject()             │ reject()            │  apply() failure auto-rolls back
   ▼                      ▼                     │
REJECTED              REJECTED                  └──────► ROLLED_BACK
```

Transition matrix:

| From \ To   | PROPOSED | APPROVED | APPLIED | REJECTED | ROLLED_BACK |
|---|:---:|:---:|:---:|:---:|:---:|
| PROPOSED    | —        | approve  | —       | reject   | —            |
| APPROVED    | —        | —        | apply   | reject   | —            |
| APPLIED     | —        | —        | —       | —        | rollback / auto |
| REJECTED    | —        | —        | —       | —        | —            |
| ROLLED_BACK | —        | —        | —       | —        | —            |

Every transition writes one audit entry tagged `autonomy.<verb>` (e.g.
`autonomy.approve`). `apply()` first verifies `payload_sha256` to defend
against T01 (payload tampered between propose and apply); on mismatch the
proposal is rejected with `AEGIS-6005 (PayloadTampered)` and a forensic
audit entry is written.

---

## 3. `auto_apply` modes

`autonomy.auto_apply_mode` selects how aggressively the workflow auto-
approves a fresh proposal:

### `manual_only` (default)
Every proposal sits in `PROPOSED` until a TenantAdmin acts via Admin UI or
`aegisctl`. Recommended for first-time rollouts and regulated environments.

### `auto_low_risk`
Workflow calls `IApprovalApplier::isLowRisk(p)` on receipt. The reference
implementation `CostAutonomyApplier::isLowRisk` encodes four conservative
rules:

- **R1** — only allow **downgrade** (rank(to) < rank(from)).
- **R2** — no two-tier jumps (`rank(from) - rank(to) ≤ 1`).
- **R3** — capped at $50 / 24h estimated savings.
- **R4** — capped at 1000 requests-per-hour blast radius.

Proposals matching all four are auto-approved + applied synchronously.
Anything else falls back to `manual_only` for that proposal. Audit shows
`auto_approved=true` on the transition entry.

### `auto_all`
Workflow auto-approves and applies every proposal. **Not recommended
outside dev environments under operator watch.** Useful for shadow-mode
A/B testing where production traffic is mirrored to a non-prod gateway.

> Switching `auto_apply_mode` is hot-reload safe (it lives under the
> `autonomy:` section, which `reloadConfig()` re-reads). Proposals already
> in `PROPOSED` retain their state — the new mode only governs new
> arrivals.

---

## 4. Budget Guard

`BudgetGuardStage` is an inbound stage installed by `GatewayRuntime` when
`budget_guard.enabled: true`. It runs after `ResolveStage` (so the model
is known) and before the router (so the downgrade decision can rewrite
`ctx.chat_request.extra["quality_tier"]`).

### Configuration

```yaml
budget_guard:
  enabled: true
  per_tenant_24h_usd: 100.0     # rolling 24h cap across the tenant
  per_request_max_usd: 1.0      # hard ceiling per single request
  fail_open_on_error: true      # see "Failure modes" below
  downgrade_tier: economy        # MLRouter quality tier on trigger
  downgrade_header_name: X-AegisGate-Budget-Guard
  downgrade_header_value: triggered
```

### Trigger logic

A request is downgraded when either:

- `tenant_24h_cost_so_far + estimated_request_cost > per_tenant_24h_usd`, OR
- `estimated_request_cost > per_request_max_usd`.

`estimated_request_cost` uses `tokens_estimated` when populated, otherwise a
small floor so first-of-window requests are never zero-cost. The response
includes the configured header, and a `budget_guard_triggered{tenant_id}`
metric counter is incremented.

The request still **continues** — it is **not** rejected. This is a
deliberate UX-preserving design choice: callers experience graceful quality
degradation instead of `429 Too Many Requests` storms.

### Failure modes

| `fail_open_on_error` | What happens if BudgetGuardStage itself throws (e.g. CostTracker mutex unreachable) |
|---|---|
| `true` (default) | Request continues unchanged, ERROR logged so SRE notices. |
| `false`          | Stage returns `PipelineResult::Error` and the request 503s. |

The default protects the data plane from observability bugs.

### Metrics

- `budget_guard_triggered_total{tenant_id, reason}` — counter, `reason` ∈
  `tenant_24h | per_request`.
- `budget_guard_error_total` — counter, internal errors.
- `budget_guard_estimated_cost_usd{tenant_id}` — histogram, per-request
  estimate.

---

## 5. Admin UI workflow

The new `/admin/finops` page is available to any role >= `TenantAdmin`.

### Layout

- **Top strip** — three KPIs: pending count, applied count, estimated 24h
  savings sum.
- **Filters** — state + source dropdowns; result count line updates live.
- **Top-5 table** — proposals ranked by `estimated_savings_usd_24h` for
  quick wins.
- **Card grid** — all proposals (subject, source, state badge, recommended
  model, tier delta, savings, tenant).
- **Side drawer** — full proposal detail (state + reviewer info, payload
  JSON, decision_trace pretty-printed, payload_sha256), with action buttons
  filtered by state:
  - PROPOSED → Approve / Reject
  - APPLIED → Rollback

### Action flow

1. Click a card → drawer opens.
2. Click Approve → confirmation modal → confirm → `POST /approve` →
   toast "已批准" → drawer closes → list reloads.
3. Click Reject → confirmation modal with required reason textarea →
   confirm with non-empty reason → `POST /reject` → toast "已拒绝".
4. Click Rollback (on an APPLIED proposal) → confirmation modal → confirm →
   `DELETE /proposals/{id}` → toast "已发起回滚".

### Backing API

| Endpoint | Method | Purpose |
|---|---|---|
| `/admin/api/autonomy/proposals` | GET | List + state/source filter + paging |
| `/admin/api/autonomy/proposals/{id}/approve` | POST | PROPOSED → APPROVED + auto-apply |
| `/admin/api/autonomy/proposals/{id}/reject` | POST | PROPOSED \| APPROVED → REJECTED (body.reason required) |
| `/admin/api/autonomy/proposals/{id}` | DELETE | APPLIED → ROLLED_BACK |
| `/admin/api/autonomy/report` | GET | Aggregated counts by source × state + 24h savings sum |

All require RBAC role `TenantAdmin` minimum. SuperAdmin sees every tenant's
proposals; TenantAdmin only sees its own tenant via row-level filters at
the workflow layer.

---

## 6. Troubleshooting

### Common error codes

| Code | HTTP | Meaning | Action |
|---|:---:|---|---|
| `AEGIS-6001 BudgetExceeded` | 429 | Request would exceed tenant 24h or per-request cap (also stamped via `X-AegisGate-Budget-Guard`) | Raise `per_tenant_24h_usd` or upgrade the tenant's plan |
| `AEGIS-6002 AutonomyDisabled` | 503 | Workflow is null because `autonomy.enabled: false` OR `AEGISGATE_DISABLE_AUTONOMY=1` is set | Enable autonomy + unset the env var, then `kill -HUP <pid>` (or restart) |
| `AEGIS-6003 ApprovalNotFound` | 404 | Proposal id unknown (typo, pruned past `proposal_retention_days`, or never persisted) | Refresh the FinOps page; check audit log for prune event |
| `AEGIS-6004 ApprovalStateInvalid` | 409 | Tried to approve from REJECTED, rollback from PROPOSED, etc. | Re-check current state; reload the proposal first |
| `AEGIS-6005 PayloadTampered` | 422 | `payload_sha256` mismatch between propose and apply (T01 defence) | Re-submit the proposal; investigate storage corruption / SQL injection |

### Kill switch

The fastest way to disable every autonomy decision worldwide:

```bash
export AEGISGATE_DISABLE_AUTONOMY=1
# Then restart or hot-reload — the workflow's isAutonomyEnabled() check
# is read on every propose() / approve() / apply() call.
```

The env var **wins** over `autonomy.enabled: true` in YAML. There is no
per-tenant override on the kill switch; this is by design (SR17 — autonomy
must be killable in one command).

### Health-check sentinel

`/admin/api/autonomy/report` is a cheap GET that doesn't write any state
and confirms wiring is healthy. If it returns `AEGIS-6002`, autonomy is
disabled. If it returns a valid JSON body with `sample_size >= 0`, the
workflow + queue are both wired.

---

## 7. Migration from CostOptimizer v1

CostOptimizer v1 (`getRecommendations()`) is **not removed** — the
read-only dashboard path stays available for any consumer that doesn't
want to opt into the workflow. v2 is purely additive:

- Set `autonomy.cost_optimizer.enabled: true` AND `autonomy.enabled: true`.
- Optionally set `auto_apply_mode: auto_low_risk` to get hands-free
  downgrades that satisfy the conservative R1–R4 whitelist.
- Existing dashboards reading `getRecommendations()` continue to work
  unchanged; they now show the same advisories that the workflow received.

### Side-by-side comparison

| | v1 (`getRecommendations`) | v2 (`proposeRecommendations`) |
|---|---|---|
| Output | Read-only advisory list | Same list + workflow submission |
| Audit | None | `autonomy.propose` per advisory |
| Apply | Manual config + reload | Synchronous via `CostAutonomyApplier` |
| Rollback | Manual config revert | Audited `applier->rollback(p)` |
| Risk gating | None | C2 rule R1–R4 via `isLowRisk()` |

### Recommended rollout staging

1. **Week 0** — Set `autonomy.enabled: true`, leave
   `cost_optimizer.enabled: false`. This wires the queue + workflow + Admin
   UI without changing CostOptimizer behaviour. Verify FinOps page loads.
2. **Week 1** — Flip `cost_optimizer.enabled: true` with
   `auto_apply_mode: manual_only`. CostOptimizer now submits to the
   workflow, but every decision needs a human click.
3. **Week 2+** — If review queues feel reliable, switch to
   `auto_apply_mode: auto_low_risk`. Keep an eye on
   `budget_guard_triggered_total` and the FinOps Top-5 table for surprises.

### Rolling back the migration

Set `autonomy.enabled: false` and reload. CostOptimizer v1's read-only
path remains, the workflow goes dormant, and `/admin/finops` will display
`AEGIS-6002` (which the UI surfaces as a "Autonomy disabled" empty state).
No data is lost — proposals persist in `autonomy_proposals` per
`proposal_retention_days`.

---

**See also:**

- `docs/guides/cost-optimization.md` — CostOptimizer v1 background
- `docs/guides/feedback-bus.md` — Phase 11.0 event bus that
  CostOptimizer reads from
- `docs/guides/admin-savings.md` — savings dashboard (Phase 10)
- `docs/guides/error-codes.md` — full error code reference
