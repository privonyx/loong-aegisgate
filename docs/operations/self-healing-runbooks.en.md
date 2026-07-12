# Self-Healing Operations Guide: Authoring RCA Rules and Runbooks (v1)

**Task:** TASK-20260519-01 Phase 11.4 Self-Healing Operations
**Version:** v1 (created 2026-05-20)
**Audience:** Operations engineers, SREs, platform developers

---

## 1. Overview

AegisGate v1 self-healing chains **3 components**:

```
RootCauseAnalyzer (RCA)  →  RunbookEngine  →  RecoveryApplier
   rule-based scoring        triggers + actions    apply / propose / audit
```

Data flow:

```
metrics + FeedbackBus + spdlog ringbuffer  →  RcaSignals
                       ↓
                 RootCauseAnalyzer.analyze()  →  hypotheses
                       ↓
                 RunbookEngine.evaluate()      →  matches
                       ↓
                 buildProposal()               →  ApprovalProposal
                       ↓
        AutonomyApprovalWorkflow + RecoveryApplier
                       ↓
               MLRouter / BudgetGuardStage / audit
```

**v1 design tradeoffs (YAGNI):**

- 5 advisory actions (`switch_router_fallback` / `switch_connector` /
  `propose_hpa_scale` / `send_webhook` / `audit_only`) only write audit +
  spdlog warn; the operator reads audit then runs kubectl / failover
  scripts manually.
- 2 active actions (`override_quality_tier` / `apply_budget_cap`) actually
  mutate runtime state.
- v1 does NOT call the K8s API directly; the HPA proposal is a
  `kubectl scale ...` command string for the operator.

---

## 2. Authoring RCA Rules

### 2.1 File location and loading

- Default: `config/rca_rules/rca_rules_v1.yaml`
- SR-NEW1: canonical path must end with `.yaml`/`.yml` and not contain
  `/..`. The body sha256 is logged for audit on every reload.

### 2.2 Rule schema

```yaml
rules:
  - id: "rca_<unique_snake_case>"             # required
    category: "internal | upstream | abuse | config"
    summary: "human-readable summary"
    min_matched_conditions: 1
    score_when_all_matched: 0.9
    score_per_condition: 0.45
    conditions:
      - kind: "metric_threshold"
        signal: "p99_latency_ms"
        op: ">"
        value: 1500
        evidence_label: "p99=${current}ms exceeds ${expected}ms"
      - kind: "feedback_event_count"
        event_type: "OpsIncident"
        topic_match: "upstream"
        window_seconds: 300
        count_threshold: 3
        evidence_label: "${count} upstream incidents"
      - kind: "log_pattern_count"
        pattern: "5\\d\\d Bad Gateway"
        window_seconds: 300
        count_threshold: 5
        evidence_label: "${count} 5xx log lines"
    suggested_actions:
      - "rb_upstream_provider_failover"
```

**Scoring formula:**

```
score = clamp(
    matched_count >= min_matched_conditions
      ? min(score_per_condition * matched_count, score_when_all_matched)
      : 0.0,
    [0, 1]
)
```

**Global threshold:** only hypotheses with `score >= 0.3` are surfaced.

### 2.3 Three condition kinds

| Kind | Key fields | Use case |
|---|---|---|
| `metric_threshold` | `signal` / `op` / `value` | single-point metric (p99 / error_rate / custom) |
| `feedback_event_count` | `event_type` / `topic_match` / `window_seconds` / `count_threshold` | FeedbackBus event count over a window |
| `log_pattern_count` | `pattern` (RE2) / `window_seconds` / `count_threshold` | regex match count in spdlog ringbuffer |

**evidence_label template variables:**

- `${current}` `${expected}` (metric_threshold)
- `${count}` `${window}` (feedback_event_count)
- `${count}` `${sample}` (log_pattern_count)

### 2.4 Security notes

- **PII**: all 3 evidence string fields are masked through `PIIFilter::mask`
  before being emitted (depth-in-defence; even if the log sink misses a
  pattern, evidence is still safe).
- **Catastrophic regex backtracking**: patterns are compiled with RE2,
  immune to backtracking explosions.
- **Hot reload failure**: `reloadRules` keeps the previous rule set on
  failure; the engine never goes empty.

---

## 3. Authoring Runbooks

### 3.1 File location and loading

- Default directory: `config/runbooks/`
- One runbook per `.yaml` file; the directory scanner only reads
  `.yaml`/`.yml` files.
- SR-NEW1 same as RCA: canonical check + extension allow-list.

### 3.2 Runbook schema

```yaml
id: "rb_<unique_snake_case>"
description: "human-readable summary"
triggers:
  # OR semantics — any matching trigger fires the runbook
  - kind: "rca_required"
    rca_rule_id: "rca_high_p99_internal"
    rca_min_score: 0.7
  - kind: "metric"
    signal: "error_rate"
    op: ">"
    value: 0.10
actions:
  # v1: single-action runbooks dispatch via ApprovalProposal.payload.action
  # straight into RecoveryApplier. Multi-action runbooks will be driven by
  # RunbookEngine in v2.
  - action: "override_quality_tier"
    payload:
      tenant_id: "${tenant_id}"
      to_quality_tier: "economy"
      from_quality_tier: "standard"
rollback_actions:
  - action: "override_quality_tier"
    payload:
      tenant_id: "${tenant_id}"
      to_quality_tier: "standard"
approval_required: true                       # true = manual approval; false = auto-approve
cooldown_seconds: 600
```

### 3.3 Three trigger kinds

| Kind | Key fields | When |
|---|---|---|
| `metric` | `signal` / `op` / `value` | simple single-point metric trigger |
| `feedback_event_count` | `event_type` / `topic_match` / `count_threshold` | FeedbackBus event accumulation |
| `rca_required` | `rca_rule_id` / `rca_min_score` | **SR7 dual signal**: requires the named RCA hypothesis to be present with sufficient score |

**Strongly recommended:** high-risk active actions
(`apply_budget_cap` / `override_quality_tier`) should use `rca_required`
to ensure at least 2 independent signals concur.

### 3.4 Seven action types

| Action | Class | Key payload | Behaviour |
|---|---|---|---|
| `override_quality_tier` | active | `tenant_id` / `to_quality_tier` | MLRouter::overrideQualityTier |
| `apply_budget_cap` | active | `new_per_tenant_24h_usd` | BudgetGuardStage::setConfig |
| `switch_router_fallback` | advisory | `model_id` / `off_until_ms` | audit + spdlog warn |
| `switch_connector` | advisory | `from_provider` / `to_provider` | audit + spdlog warn |
| `propose_hpa_scale` | advisory | `target_replicas` / `suggested_kubectl_command` | audit + spdlog warn |
| `send_webhook` | advisory | `url` / `body` | audit + spdlog warn (v1 stub) |
| `audit_only` | advisory | any | audit only |

**isLowRisk rules** (only relevant for auto-approve):

- `override_quality_tier`: only downgrade (rank(to) < rank(from)) / no
  two-tier jumps / ≤$50/24h savings / ≤1000 RPS
- `apply_budget_cap`: `new_cap ≥ current_cap × 0.5`
- All advisory actions: always false (require manual approval)

### 3.5 cooldown and approval choice

- `approval_required: false` → auto-approve but still goes propose →
  approve → apply (SR2 audit chain). Only safe for low-risk runbooks
  (isLowRisk=true).
- `approval_required: true` → proposal lands in ApprovalQueue, awaiting
  `/admin/approvals/<id>/approve`.
- `cooldown_seconds`: how long after a hit the same runbook is suppressed
  (M3 anti-flap).

### 3.6 Security notes

- **payload schema**: missing `tenant_id`/`to_quality_tier` for
  `override_quality_tier` → fail closed (apply returns `schema_invalid`).
- **dry_run**: every active action supports `apply(p, dry_run=true)` for
  the Admin UI preview drawer (before/after snapshot).
- **rollback**: every action should have a corresponding rollback action
  (at minimum `audit_only`) so that ROLLED_BACK transitions are auditable.

---

## 4. Debugging and operations

### 4.1 Inspect active rules and runbooks

```bash
# RCA rule count + last reload timestamp
curl http://localhost:18080/admin/rca/health

# Runbook loader state
curl http://localhost:18080/admin/runbooks
```

### 4.2 Inspect the audit chain

```bash
curl 'http://localhost:18080/admin/audit?action_prefix=auto_recovery&limit=20'
```

Each entry: `action="auto_recovery.apply.<action_type>"`, `detail` carries
a before/after summary.

### 4.3 Emergency kill switch (SR17)

```bash
export AEGISGATE_DISABLE_AUTONOMY=1
# Restart the process; subsequent RCA/RunbookEngine evaluate calls
# short-circuit at the entry point.
```

### 4.4 PII mask defence-in-depth verification

```bash
# Inject a log line containing an email
curl -X POST http://localhost:18080/admin/test/log \
  -d '{"msg":"failed for alice@example.com"}'

# Dump recent logs (SuperAdmin only)
curl http://localhost:18080/admin/logs/dump?limit=10
```

The response must replace `alice@example.com` with `[EMAIL]`.

---

## 5. v1 limitations / future work

- Multi-action runbook serial execution (v2, driven by RunbookEngine)
- HPA direct K8s API call (v2, needs K8s client; v1 only proposes)
- Cross-runbook DAG dependencies (v3, Workflow 2.0)
- ML-learned root-cause scoring (v3)

---
