# Upgrade Guide: v0.x → v1.0

This guide covers upgrading AegisGate from any v0.x release to v1.0.0 GA.

## Overview

AegisGate v1.0.0 is the first General Availability (GA) release. Starting from v1.0.0, the `/v1/` API endpoints are **stable** — breaking changes will only occur in future major versions (v2.0+) with at least two minor versions of deprecation notice.

## Breaking Changes from v0.9.0

**There are no breaking API changes between v0.9.0 and v1.0.0.** The v1.0.0 release adds new features (Phase 8: Agent Orchestration, RAG Pipeline, Smart Cache 2.0, Advanced Observability) and establishes the API stability commitment.

## Upgrade Steps

### 1. Update Binary / Docker Image

```bash
# Docker
docker pull ghcr.io/privonyx/loong-aegisgate:1.0.0

# Or build from source
git checkout v1.0.0
cmake -B build -DCMAKE_BUILD_TYPE=Release ...
cmake --build build
```

### 2. Configuration Compatibility

v1.0.0 is **fully backward-compatible** with v0.9.0 configuration files. New configuration sections are optional and default to disabled:

```yaml
# New in v1.0.0 — all optional
agent:
  enabled: false          # Agent orchestration (Enterprise)

rag:
  enabled: false          # RAG pipeline (Enterprise)

observability:
  cost_attribution:
    enabled: false
  anomaly_detection:
    enabled: false
  quality_monitoring:
    enabled: false
  cost_optimization:
    enabled: false
```

### 3. SDK Updates

Update your client SDKs to v1.0.0:

```bash
# Python
pip install aegisgate==1.0.0

# Node.js
npm install @aegisgate/sdk@1.0.0

# Go
go get github.com/privonyx/loong-aegisgate-go@v1.0.0
```

SDK v1.0.0 is backward-compatible with v0.9.0. No client code changes are required.

### 4. Helm Chart

If using Kubernetes deployment:

```bash
helm upgrade aegisgate ./helm/aegisgate --set image.tag=1.0.0
```

### 5. Enterprise License

Enterprise features (Agent Orchestration, RAG Pipeline) require an updated license with the new feature flags:

- `agent_orchestration` — enables Agent orchestration module
- `rag_pipeline` — enables RAG pipeline integration

Contact your account representative for an updated license file.

## New Features Available After Upgrade

| Feature | Edition | Config Key |
|---------|---------|------------|
| Agent Orchestration | Enterprise | `agent.enabled` |
| RAG Pipeline | Enterprise | `rag.enabled` |
| Smart Cache 2.0 | Community | `cache.feedback.enabled`, `cache.cross_tenant.enabled` |
| Cost Attribution | Community | `observability.cost_attribution.enabled` |
| Anomaly Detection | Community | `observability.anomaly_detection.enabled` |
| Quality Monitoring | Community | `observability.quality_monitoring.enabled` |
| Cost Optimization | Community | `observability.cost_optimization.enabled` |

## New Prometheus Metrics

The following metrics are available after upgrade (zero impact if features are disabled):

| Metric | Type | Description |
|--------|------|-------------|
| `aegisgate_rag_retrievals_total` | Counter | RAG retrieval operations |
| `aegisgate_agent_steps_total` | Counter | Agent orchestration steps |
| `aegisgate_anomaly_events_total` | Counter | Anomaly events detected |
| `aegisgate_groundedness_score` | Histogram | RAG groundedness score |
| `aegisgate_cache_feedback_total` | Counter | Cache quality feedback |

Update your Grafana dashboards to include these metrics if desired.

## Rollback

If you need to rollback to v0.9.0:

1. Stop v1.0.0 and start the v0.9.0 binary
2. No configuration changes needed — v0.9.0 ignores unknown config sections
3. No data migration needed — storage schema is backward-compatible

## Version Verification

After upgrade, verify the version via:

```bash
# Health endpoint
curl -s http://localhost:8080/health/ready | jq .version

# Response header
curl -sI http://localhost:8080/v1/chat/completions | grep X-AegisGate-Version
# X-AegisGate-Version: 1.0.0
```

## Support

For upgrade issues, please:
1. Check the [CHANGELOG](../../CHANGELOG.md) for detailed changes
2. Open a [GitHub Issue](https://github.com/privonyx/loong-aegisgate/issues) with the `upgrade` label
3. Review the [API Stability Policy](../../VERSIONING.md)
