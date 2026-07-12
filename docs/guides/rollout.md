# Rollout Guide — AegisGate Phase 9.3.4

> Phased configuration rollouts with metrics-driven gatekeeping and
> automatic rollback.

## Architecture Overview

```
 ┌─────────────┐     gRPC      ┌────────────────────┐
 │  aegisctl   │────────────>│  RolloutService     │
 │  rollout *  │              │  (control-plane)    │
 └─────────────┘              │                     │
                              │  RolloutController  │
                              │    ├─ StateMachine  │
                              │    ├─ Ticker (1s)   │
                              │    ├─ MetricsProvider│
                              │    └─ AuditBridge   │
                              └────────┬────────────┘
                                       │ merged YAML
                              ┌────────▼────────────┐
                              │  Data-Plane          │
                              │  resolveActiveConfigId│
                              │  RouterOutcome →     │
                              │    FeedbackBus       │
                              └─────────────────────┘
```

## Quick Start

### 1. Establish a baseline active config

```bash
export AEGISGATE_CP_API_KEY="$YOUR_KEY"
aegisctl config apply config/new.yaml --comment "v2 features"
aegisctl config approve <version_id> --comment "LGTM"
aegisctl config activate <version_id> --comment "go live"
```

### 2. Create a rollout spec

```yaml
# rollout-spec.yaml
target_version_id: "01ABC..."    # must be APPROVED
sticky_key: "tenant_id"
auto_rollback_on_pause: true
auto_rollback_grace_seconds: 600
creator_comment: "canary rollout for v2"
stages:
  - name: canary
    scope:
      tenant_globs: ["beta-*"]
      regions: ["us-east-1"]
      percentage: 5
    observation:
      min_duration_seconds: 300
      min_sample_count: 1000
    auto_pause:
      error_rate_gt: 0.02
      p99_latency_ratio_gt: 2.0
      absolute_error_rate_gt: 0.10
      absolute_p99_latency_ms_gt: 5000
  - name: full
    scope:
      percentage: 100
    observation:
      min_duration_seconds: 600
      min_sample_count: 5000
    auto_pause:
      error_rate_gt: 0.01
      p99_latency_ratio_gt: 1.5
```

### 3. Execute the rollout

```bash
aegisctl rollout create --spec rollout-spec.yaml
aegisctl rollout start --rollout-id <id>
# Monitor
aegisctl rollout status --rollout-id <id> --output json
# Promote through stages
aegisctl rollout promote --rollout-id <id>
```

## Metrics Gatekeeping

The Ticker evaluates each PROGRESSING rollout every second:

| Threshold | Type | Meaning |
|-----------|------|---------|
| `error_rate_gt` | Relative | target error rate − baseline error rate |
| `p99_latency_ratio_gt` | Relative | target p99 / baseline p99 |
| `absolute_error_rate_gt` | Absolute | safety net if baseline is degraded |
| `absolute_p99_latency_ms_gt` | Absolute | hard p99 latency ceiling |

Both the observation `min_duration_seconds` and `min_sample_count` must be
satisfied before auto-pause decisions fire.

## Auto-Rollback

When `auto_rollback_on_pause: true`:

1. Rollout pauses automatically when thresholds are breached
2. Grace timer starts (`auto_rollback_grace_seconds`)
3. If not manually resumed within the grace period → status transitions to
   FAILED and the previous active config version is restored

**Kill switch (SR17):** Set `AEGISGATE_DISABLE_AUTO_ROLLBACK=1` to disable
auto-rollback system-wide.

## CLI Reference

| Command | Description |
|---------|-------------|
| `aegisctl rollout create --spec FILE` | Create a new rollout |
| `aegisctl rollout start --rollout-id ID` | Start a PENDING rollout |
| `aegisctl rollout status --rollout-id ID` | Get rollout status |
| `aegisctl rollout list [--output json]` | List all rollouts |
| `aegisctl rollout pause --rollout-id ID` | Manually pause |
| `aegisctl rollout resume --rollout-id ID` | Resume a paused rollout |
| `aegisctl rollout promote --rollout-id ID` | Advance to next stage |
| `aegisctl rollout abort --rollout-id ID` | Abort and restore previous |

All commands support `--output json` and `--output table` (default).

## Troubleshooting

| Error | Cause | Resolution |
|-------|-------|------------|
| `FAILED_PRECONDITION: target version not APPROVED` | Target config not yet approved | Run `aegisctl config approve` first |
| `FAILED_PRECONDITION: active rollout exists for target` | Concurrent rollout conflict | Abort existing rollout first |
| `RESOURCE_EXHAUSTED: tenant quota exceeded` | SR16: too many rollouts in 24h | Wait for the quota window to pass |
| Status stuck at PAUSED | Auto-pause triggered by metrics | Check `pause_detail` field; fix the issue or `abort` |
| `PERMISSION_DENIED` | Missing SuperAdmin role (SR1) | Verify API key has SuperAdmin |
