# AegisGate Production Deployment Guide

This guide covers deploying AegisGate in production environments, from single-node Docker deployments to Kubernetes clusters with full monitoring.

## Table of Contents

- [Hardware Requirements](#hardware-requirements)
- [Docker Deployment (Single Node)](#docker-deployment-single-node)
- [Docker Compose Cluster Deployment](#docker-compose-cluster-deployment)
- [Kubernetes Helm Deployment](#kubernetes-helm-deployment)
- [Production Configuration](#production-configuration)
- [Monitoring Setup (Prometheus + Grafana)](#monitoring-setup-prometheus--grafana)
- [Health Check Configuration](#health-check-configuration)
- [Log Management](#log-management)
- [Backup and Recovery](#backup-and-recovery)
- [Security Hardening](#security-hardening)

---

## Hardware Requirements

### Minimum Requirements (Community Edition, Single Node)

| Resource | Minimum | Recommended |
|----------|---------|-------------|
| CPU | 2 cores | 4+ cores |
| RAM | 512 MB | 2 GB |
| Disk | 1 GB | 10 GB (SSD recommended) |
| OS | Linux (glibc 2.31+) | Ubuntu 22.04 LTS / Debian 12 |

### Recommended Requirements (Enterprise Edition, Cluster)

| Resource | Per Node | Notes |
|----------|----------|-------|
| CPU | 4-8 cores | AegisGate is I/O-bound; more cores help with concurrent request processing |
| RAM | 4-8 GB | Increase if ONNX embedder is enabled (~500 MB for model) or high cache entry count |
| Disk | 20 GB SSD | WAL-mode SQLite and audit logs require fast sequential writes |
| Network | 1 Gbps | Low-latency connection to upstream LLM providers is critical |

### Scaling Guidelines

| Concurrency Target | Recommended Setup |
|-------------------|-------------------|
| < 100 RPS | Single node, community edition |
| 100-1,000 RPS | 2-3 nodes + Nginx LB, enterprise edition |
| 1,000-10,000 RPS | 3-5 Kubernetes pods with HPA, enterprise edition |
| > 10,000 RPS | 5+ pods, dedicated Redis cluster, external PostgreSQL |

---

## Docker Deployment (Single Node)

### Build the Image

```bash
docker build -t aegisgate:latest .
```

With optional features:

```bash
# With Redis support (enterprise)
docker build -t aegisgate:latest --build-arg ENABLE_REDIS=ON .

# With OpenTelemetry tracing
docker build -t aegisgate:latest --build-arg ENABLE_OPENTELEMETRY=ON .
```

### Production-profile Build (Redis + PG + OTel + control-plane) — `prod profile`

> **Why a separate profile?** The default Docker build keeps Redis / PG / OpenTelemetry / control-plane OFF (matches `scripts/build.sh` slim profile). Production deployments **must** validate link-time correctness with the same capabilities they will run with — a slim-profile-only build will not surface link/runtime defects that only appear when the full set is linked (see `memory-bank/systemPatterns.md` *"Validation must run under the production profile"*).
>
> Container builds intended for production therefore must pass the four build-args explicitly. This is the Docker equivalent of `scripts/build.sh -t Release` (one-shot host build).

```bash
docker build -t aegisgate:prod \
  --build-arg ENABLE_REDIS=ON \
  --build-arg ENABLE_PG=ON \
  --build-arg ENABLE_OPENTELEMETRY=ON \
  --build-arg ENABLE_CONTROL_PLANE=ON \
  --build-arg VCPKG_FEATURES="guard-spm;redis;pg;otel;control-plane" \
  .
```

> Fetch ONNX Runtime into `third_party/` first (`scripts/fetch-onnxruntime.sh`);
> the pinned vcpkg baseline has no `onnxruntime` port.

Equivalent for `docker compose` (set in shell or `.env`):

```bash
ENABLE_REDIS=ON \
ENABLE_PG=ON \
ENABLE_OPENTELEMETRY=ON \
ENABLE_CONTROL_PLANE=ON \
VCPKG_FEATURES="guard-spm;redis;pg;otel;control-plane" \
  docker compose build
```

Both forms produce a container whose capability set matches `scripts/build.sh -t Release` output — and exercises the same vcpkg overlay-port that fixed the production startup SIGSEGV from `sentencepiece` / `protobuf` symbol collisions (commit `3542b0c`).

> **Background companion script:** if you are deploying on bare-metal / VM rather than containers, use `scripts/setup-prod-deps.sh` to provision Redis + PostgreSQL + OTel Collector with hardened defaults (random PG password written to `~/.aegisgate_prod_creds` chmod 600, OTel deb SHA256-verified, see `CHANGELOG.md` entry for TASK-20260618-01).

### Run the Container

```bash
docker run -d \
  --name aegisgate \
  --restart unless-stopped \
  -p 8080:8080 \
  -v $(pwd)/config:/app/config:ro \
  -v aegisgate-data:/app/data \
  -v aegisgate-logs:/app/logs \
  -e AEGISGATE_API_KEY="your-gateway-api-key" \
  -e OPENAI_API_KEY="sk-your-openai-key" \
  --memory 2g \
  --cpus 2 \
  aegisgate:latest
```

### Verify Deployment

```bash
# Health check
curl http://localhost:8080/health
# Expected: {"status":"ok","version":"1.0.0"}

# Readiness check
curl http://localhost:8080/health/ready
```

### Docker Run Best Practices

- **Always mount config as read-only** (`:ro`) to prevent accidental modification
- **Use named volumes** for `data/` and `logs/` to persist across container restarts
- **Set resource limits** (`--memory`, `--cpus`) to prevent noisy-neighbor issues
- **Use `--restart unless-stopped`** for automatic restart on crash
- **Never put secrets in the image or Dockerfile** — pass them as environment variables

---

## Docker Compose Cluster Deployment

For high availability with 2+ AegisGate instances, Redis shared state, and an Nginx load balancer.

### Architecture

```
                    ┌──────────┐
   Client ────────► │  Nginx   │ :8080
                    │ (LB)     │
                    └────┬─────┘
                    ┌────┴─────┐
              ┌─────┤          ├─────┐
              ▼     │          │     ▼
        ┌──────────┐│    ┌──────────┐
        │AegisGate ││    │AegisGate │
        │ Node 1   ││    │ Node 2   │
        └─────┬────┘│    └────┬─────┘
              │     │         │
              └─────┼─────────┘
                    ▼
              ┌──────────┐
              │  Redis   │
              └──────────┘
```

### Start the Cluster

```bash
# From the project root
cd deploy

# Set required environment variables
export OPENAI_API_KEY="sk-your-key"
export AEGISGATE_API_KEY="your-gateway-key"

# Start all services
docker compose -f docker-compose.cluster.yaml up -d

# Verify
docker compose -f docker-compose.cluster.yaml ps
curl http://localhost:8080/health
```

### Services

| Service | Port | Description |
|---------|------|-------------|
| `nginx` | 8080 | Load balancer (round-robin) |
| `aegisgate-1` | 8081 | AegisGate instance 1 |
| `aegisgate-2` | 8082 | AegisGate instance 2 |
| `redis` | 6379 | Shared state store |
| `prometheus` | 9090 | Metrics collection |
| `grafana` | 3000 | Metrics dashboard (admin/admin) |

### Scaling

```bash
# Add more AegisGate instances
docker compose -f docker-compose.cluster.yaml up -d --scale aegisgate-1=3
```

---

## Kubernetes Helm Deployment

### Prerequisites

- Kubernetes 1.24+
- Helm 3.10+
- Container registry with the AegisGate image
- Redis instance (for cluster mode)

### Install the Helm Chart

```bash
# Add your registry and push the image first
docker tag aegisgate:latest your-registry.example.com/aegisgate:1.0.0
docker push your-registry.example.com/aegisgate:1.0.0

# Install with Helm
helm install aegisgate helm/aegisgate/ \
  --namespace aegisgate \
  --create-namespace \
  --set image.repository=your-registry.example.com/aegisgate \
  --set image.tag=1.0.0 \
  --set config.edition=enterprise \
  --set redis.host=redis.aegisgate.svc.cluster.local \
  --set redis.port=6379
```

### Custom Values

Create a `values-production.yaml` file:

```yaml
replicaCount: 3

image:
  repository: your-registry.example.com/aegisgate
  tag: "1.0.0"
  pullPolicy: IfNotPresent

service:
  type: ClusterIP
  port: 8080

ingress:
  enabled: true
  className: nginx
  annotations:
    cert-manager.io/cluster-issuer: letsencrypt-prod
    nginx.ingress.kubernetes.io/proxy-read-timeout: "120"
    nginx.ingress.kubernetes.io/proxy-send-timeout: "120"
    nginx.ingress.kubernetes.io/proxy-buffering: "off"
  hosts:
    - host: aegisgate.yourdomain.com
      paths:
        - path: /
          pathType: Prefix
  tls:
    - secretName: aegisgate-tls
      hosts:
        - aegisgate.yourdomain.com

resources:
  limits:
    cpu: "4"
    memory: 4Gi
  requests:
    cpu: "1"
    memory: 512Mi

autoscaling:
  enabled: true
  minReplicas: 3
  maxReplicas: 20
  targetCPUUtilizationPercentage: 70

podDisruptionBudget:
  enabled: true
  minAvailable: 2

deployment:
  mode: cluster

redis:
  host: redis-master.redis.svc.cluster.local
  port: 6379
  password: ""
  db: 0

config:
  edition: enterprise
  log_level: info

env:
  - name: AEGISGATE_API_KEY
    valueFrom:
      secretKeyRef:
        name: aegisgate-secrets
        key: api-key
  - name: OPENAI_API_KEY
    valueFrom:
      secretKeyRef:
        name: aegisgate-secrets
        key: openai-api-key

nodeSelector:
  node-role: worker

tolerations: []

affinity:
  podAntiAffinity:
    preferredDuringSchedulingIgnoredDuringExecution:
      - weight: 100
        podAffinityTerm:
          labelSelector:
            matchExpressions:
              - key: app.kubernetes.io/name
                operator: In
                values:
                  - aegisgate
          topologyKey: kubernetes.io/hostname
```

Install with custom values:

```bash
helm install aegisgate helm/aegisgate/ \
  --namespace aegisgate \
  --create-namespace \
  -f values-production.yaml
```

### Create Secrets

```bash
kubectl create namespace aegisgate

kubectl create secret generic aegisgate-secrets \
  --namespace aegisgate \
  --from-literal=api-key='your-gateway-api-key' \
  --from-literal=openai-api-key='sk-your-openai-key'
```

### Verify Deployment

```bash
# Check pods
kubectl get pods -n aegisgate

# Check service
kubectl get svc -n aegisgate

# Port-forward for local testing
kubectl port-forward svc/aegisgate 8080:8080 -n aegisgate

# Health check
curl http://localhost:8080/health
```

### Upgrade

```bash
helm upgrade aegisgate helm/aegisgate/ \
  --namespace aegisgate \
  -f values-production.yaml \
  --set image.tag=1.1.0
```

---

## Production Configuration

### Security Settings

```yaml
auth:
  enabled: true
  api_keys:
    - "${AEGISGATE_API_KEY}"

tls:
  enabled: true
  port: 8443
  cert_path: "/app/certs/tls.crt"
  key_path: "/app/certs/tls.key"

security:
  unicode_normalization: true
  encoding_detection: true

  abuse_detection:
    enabled: true
    window_seconds: 300
    warn_threshold: 5
    throttle_threshold: 10
    block_threshold: 20
    block_duration_seconds: 1800
```

### Performance Tuning

```yaml
server:
  host: "0.0.0.0"
  port: 8080
  threads: 0                    # 0 = auto-detect CPU cores
  request_timeout_seconds: 120

limits:
  max_request_body_size: 65536  # 64 KB
  max_connections: 10000

cache:
  threshold: 0.92               # Slightly lower than default for better hit rate
  ttl_seconds: 7200             # 2 hours for production
  max_entries: 50000            # Increase for high-traffic deployments
  max_partitions: 64

  adaptive_threshold:
    enabled: true               # Enable in production for self-tuning
    min_threshold: 0.85
    max_threshold: 0.98
    adjustment_rate: 0.01
    window_size: 200

rate_limit:
  max_tokens: 50000.0           # Higher burst for production
  refill_rate: 500.0            # 500 requests/sec sustained

token_optimization:
  prompt_compression:
    enabled: true
    max_context_messages: 20
    compress_whitespace: true
    dedup_system_prompts: true
  smart_max_tokens:
    enabled: true
    default_max_output: 2048
    max_output_ratio: 2.0
    min_output_tokens: 100
```

### Storage Configuration (Enterprise)

```yaml
storage:
  cache_backend: redis
  persistent_backend: postgres

  redis:
    host: "${REDIS_HOST}"
    port: 6379
    password: "${REDIS_PASSWORD}"
    db: 0
    pool_size: 8
    connect_timeout_ms: 3000
    command_timeout_ms: 1000

  postgres:
    url: "${POSTGRES_URL}"
    pool_size: 8
    connect_timeout_ms: 5000
```

---

## Monitoring Setup (Prometheus + Grafana)

### Prometheus Configuration

AegisGate exposes Prometheus metrics at `/metrics`. Configure Prometheus to scrape this endpoint:

```yaml
# prometheus.yml
global:
  scrape_interval: 15s
  evaluation_interval: 15s

scrape_configs:
  - job_name: "aegisgate"
    metrics_path: "/metrics"
    # For Docker Compose
    static_configs:
      - targets: ["aegisgate-1:8080", "aegisgate-2:8080"]
    # For Kubernetes, use service discovery instead:
    # kubernetes_sd_configs:
    #   - role: pod
    #     namespaces:
    #       names: ["aegisgate"]
    # relabel_configs:
    #   - source_labels: [__meta_kubernetes_pod_label_app_kubernetes_io_name]
    #     action: keep
    #     regex: aegisgate
```

### Key Metrics

| Metric | Type | Description |
|--------|------|-------------|
| `aegisgate_requests_total` | Counter | Total requests by model and status |
| `aegisgate_request_duration_seconds` | Histogram | Request latency distribution |
| `aegisgate_tokens_total` | Counter | Total tokens processed |
| `aegisgate_tokens_saved_total` | Counter | Tokens saved by optimization |
| `aegisgate_cache_hits_total` | Counter | Semantic cache hits |
| `aegisgate_guardrail_blocks_total` | Counter | Requests blocked by guardrails |
| `aegisgate_upstream_errors_total` | Counter | Upstream provider errors |

### Grafana Dashboard

A pre-built Grafana dashboard is included at `deploy/grafana/dashboards/`:

```bash
# Import via provisioning (already configured in docker-compose.cluster.yaml)
# Or manually import the JSON file via Grafana UI:
#   Dashboards → Import → Upload JSON file
```

The dashboard includes panels for:
- Request rate and error rate (by model)
- Latency percentiles (P50, P95, P99)
- Cache hit rate
- Guardrail block rate
- Token usage and savings
- Upstream provider health

### Alerting Rules (Example)

```yaml
# prometheus-alerts.yml
groups:
  - name: aegisgate
    rules:
      - alert: HighErrorRate
        expr: rate(aegisgate_requests_total{status="error"}[5m]) / rate(aegisgate_requests_total[5m]) > 0.05
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "AegisGate error rate above 5%"

      - alert: HighLatency
        expr: histogram_quantile(0.99, rate(aegisgate_request_duration_seconds_bucket[5m])) > 30
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "AegisGate P99 latency above 30s"

      - alert: CacheHitRateLow
        expr: rate(aegisgate_cache_hits_total[5m]) / rate(aegisgate_requests_total[5m]) < 0.1
        for: 15m
        labels:
          severity: info
        annotations:
          summary: "AegisGate cache hit rate below 10%"
```

---

## Health Check Configuration

AegisGate provides two health check endpoints:

| Endpoint | Auth Required | Description |
|----------|--------------|-------------|
| `GET /health` | No | Basic liveness — returns `{"status":"ok","version":"..."}` |
| `GET /health/ready` | No | Readiness — checks that pipeline is assembled and ready to serve |

### Docker Health Check

Included in the Dockerfile:

```dockerfile
HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
    CMD wget --no-verbose --tries=1 --spider http://127.0.0.1:8080/health/ready || exit 1
```

### Kubernetes Probes

```yaml
livenessProbe:
  httpGet:
    path: /health
    port: 8080
  initialDelaySeconds: 5
  periodSeconds: 15
  timeoutSeconds: 3
  failureThreshold: 3

readinessProbe:
  httpGet:
    path: /health/ready
    port: 8080
  initialDelaySeconds: 3
  periodSeconds: 10
  timeoutSeconds: 3
  failureThreshold: 2

startupProbe:
  httpGet:
    path: /health/ready
    port: 8080
  initialDelaySeconds: 2
  periodSeconds: 5
  failureThreshold: 30
```

---

## Log Management

### Log Configuration

```yaml
logging:
  level: info           # debug | info | warn | error
  file: "logs/aegisgate.log"   # empty = stdout only
```

### Structured Log Format

AegisGate uses spdlog for structured logging. Log entries include:

```
[2026-04-15 10:30:45.123] [info] [api_controller] Request processed model=gpt-4 status=ok latency_ms=1523 tokens=150
```

### Log Rotation

For file-based logging, configure log rotation with `logrotate`:

```
# /etc/logrotate.d/aegisgate
/app/logs/aegisgate.log {
    daily
    rotate 30
    compress
    delaycompress
    missingok
    notifempty
    copytruncate
}
```

### Docker / Kubernetes Log Collection

In containerized environments, configure `logging.file` to empty (`""`) so logs go to stdout, then collect with your preferred log aggregator:

- **Docker**: `docker logs aegisgate` or Docker log driver (fluentd, gelf, etc.)
- **Kubernetes**: Collect via Fluentd/Fluent Bit DaemonSet or Loki + Promtail

### Audit Logs

Audit logs are separate from application logs and contain the full request/response audit trail with integrity chain hashing:

```yaml
audit:
  log_path: "logs/audit.log"
  retention_days: 90    # 0 = keep forever
```

Audit logs should be stored on persistent storage and backed up regularly for compliance requirements.

---

## Backup and Recovery

### What to Back Up

| Component | Path | Frequency | Method |
|-----------|------|-----------|--------|
| Configuration | `config/` | On change | Git version control |
| SQLite database | `data/aegisgate.db` | Daily | File copy (with WAL checkpoint) |
| Audit logs | `logs/audit.log` | Daily | File copy or log shipping |
| Semantic cache | In-memory (lost on restart) | N/A | Warm-up from CacheStore on startup |
| TLS certificates | `/app/certs/` | On rotation | Secure backup |

### SQLite Backup

```bash
# Safe backup with WAL checkpoint (recommended)
sqlite3 data/aegisgate.db ".backup 'backup/aegisgate-$(date +%Y%m%d).db'"

# Or use the online backup API for zero-downtime backup
sqlite3 data/aegisgate.db "PRAGMA wal_checkpoint(TRUNCATE);"
cp data/aegisgate.db backup/aegisgate-$(date +%Y%m%d).db
```

### PostgreSQL Backup (Enterprise)

```bash
pg_dump -h $POSTGRES_HOST -U aegisgate -d aegisgate \
  --format=custom \
  --file=backup/aegisgate-$(date +%Y%m%d).pgdump
```

### Recovery Procedure

1. **Stop AegisGate** (or scale to 0 replicas in Kubernetes)
2. **Restore configuration**: Copy config files to `config/`
3. **Restore database**: Copy backup to `data/aegisgate.db` (SQLite) or `pg_restore` (PostgreSQL)
4. **Restore audit logs**: Copy to `logs/`
5. **Start AegisGate**: The semantic cache will warm up from the CacheStore backend on startup
6. **Verify**: Run health check and send a test request

---

## Security Hardening

### Checklist

- [ ] Enable TLS (`tls.enabled: true`) or terminate TLS at the load balancer / ingress
- [ ] Set strong API keys (minimum 32 characters, randomly generated)
- [ ] Enable abuse detection (`security.abuse_detection.enabled: true`)
- [ ] Enable Unicode normalization and encoding detection
- [ ] Set `limits.max_request_body_size` to prevent oversized payloads (default: 64 KB)
- [ ] Run as non-root user (the Docker image uses `aegisgate` user by default)
- [ ] Mount config volumes as read-only
- [ ] Store secrets in environment variables or Kubernetes Secrets, never in config files
- [ ] Enable audit logging with sufficient retention for compliance
- [ ] Restrict admin endpoints by IP if possible (`admin.allowed_ips`)
- [ ] Rotate API keys periodically
- [ ] Keep the AegisGate image updated with security patches

For a comprehensive security guide, see [Security Best Practices](security-best-practices.md).
