# AegisGate 生产环境部署指南

本指南涵盖 AegisGate 在生产环境中的部署，从单节点 Docker 部署到带完整监控的 Kubernetes 集群。

## 目录

- [硬件要求](#硬件要求)
- [Docker 部署（单节点）](#docker-部署单节点)
- [Docker Compose 集群部署](#docker-compose-集群部署)
- [Kubernetes Helm 部署](#kubernetes-helm-部署)
- [生产环境配置](#生产环境配置)
- [监控配置（Prometheus + Grafana）](#监控配置prometheus--grafana)
- [健康检查配置](#健康检查配置)
- [日志管理](#日志管理)
- [备份与恢复](#备份与恢复)
- [安全加固](#安全加固)

---

## 硬件要求

### 最低要求（社区版，单节点）

| 资源 | 最低配置 | 推荐配置 |
|------|----------|----------|
| CPU | 2 核 | 4+ 核 |
| 内存 | 512 MB | 2 GB |
| 磁盘 | 1 GB | 10 GB（推荐 SSD） |
| 操作系统 | Linux (glibc 2.31+) | Ubuntu 22.04 LTS / Debian 12 |

### 推荐配置（企业版，集群）

| 资源 | 每节点 | 说明 |
|------|--------|------|
| CPU | 4-8 核 | AegisGate 以 I/O 为主；更多核心有助于并发请求处理 |
| 内存 | 4-8 GB | 启用 ONNX 嵌入器时需增加（模型约 500 MB）；高缓存条目数也需更多内存 |
| 磁盘 | 20 GB SSD | WAL 模式 SQLite 和审计日志需要快速顺序写入 |
| 网络 | 1 Gbps | 到上游 LLM 提供商的低延迟连接至关重要 |

### 扩容指南

| 并发目标 | 推荐方案 |
|----------|----------|
| < 100 RPS | 单节点，社区版 |
| 100-1,000 RPS | 2-3 节点 + Nginx 负载均衡，企业版 |
| 1,000-10,000 RPS | 3-5 个 Kubernetes Pod，配合 HPA，企业版 |
| > 10,000 RPS | 5+ Pod，专用 Redis 集群，外部 PostgreSQL |

---

## Docker 部署（单节点）

### 构建镜像

```bash
docker build -t aegisgate:latest .
```

构建可选功能：

```bash
# 启用 Redis 支持（企业版）
docker build -t aegisgate:latest --build-arg ENABLE_REDIS=ON .

# 启用 OpenTelemetry 追踪
docker build -t aegisgate:latest --build-arg ENABLE_OPENTELEMETRY=ON .
```

### 生产档位构建（Redis + PG + OTel + 控制面）—— `prod profile`

> **为什么单独有一个档位？** 默认 docker 构建保持 Redis / PG / OpenTelemetry / 控制面四档全 OFF（与 `scripts/build.sh` 精简档位一致）。**生产部署的容器必须用与运行时同档位的构建**，否则只在精简档位下验证的镜像在生产形态会暴露链接/运行时缺陷（详见 `memory-bank/systemPatterns.md` 「验证必须在『生产档位』全开下进行」教训段）。
>
> 因此面向生产的容器构建必须显式给 4 个 build-arg —— 这正是 `scripts/build.sh -t Release`（一键宿主机构建）的容器化等价形式。

```bash
docker build -t aegisgate:prod \
  --build-arg ENABLE_REDIS=ON \
  --build-arg ENABLE_PG=ON \
  --build-arg ENABLE_OPENTELEMETRY=ON \
  --build-arg ENABLE_CONTROL_PLANE=ON \
  --build-arg VCPKG_FEATURES="guard-spm;redis;pg;otel;control-plane" \
  .
```

`docker compose` 等价用法（shell 或 `.env` 设置环境变量）：

```bash
ENABLE_REDIS=ON \
ENABLE_PG=ON \
ENABLE_OPENTELEMETRY=ON \
ENABLE_CONTROL_PLANE=ON \
VCPKG_FEATURES="guard-spm;redis;pg;otel;control-plane" \
  docker compose build
```

两种形式都会产出与 `scripts/build.sh -t Release` 能力集等价的容器 —— 并且会触发那次修复了 `sentencepiece` / `protobuf` 符号冲突导致的生产启动 SIGSEGV（commit `3542b0c`）的 vcpkg overlay-port 链路。

> **裸机/VM 对应脚本：** 不上容器、直接装在裸机/VM 上时，用 `scripts/setup-prod-deps.sh` 一键装 Redis + PostgreSQL + OTel Collector，默认行为已加固（PG 口令随机生成并写入 `~/.aegisgate_prod_creds` chmod 600 / OTel deb 强制 SHA256 校验，详见 CHANGELOG TASK-20260618-01 条目）。

### 运行容器

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

### 验证部署

```bash
# 健康检查
curl http://localhost:8080/health
# 预期返回: {"status":"ok","version":"1.0.0"}

# 就绪检查
curl http://localhost:8080/health/ready
```

### Docker 运行最佳实践

- **配置目录始终以只读方式挂载**（`:ro`），防止意外修改
- **使用命名卷**存储 `data/` 和 `logs/`，确保容器重启后数据持久化
- **设置资源限制**（`--memory`、`--cpus`），防止资源争抢
- **使用 `--restart unless-stopped`** 实现崩溃后自动重启
- **永远不要在镜像或 Dockerfile 中放置密钥** — 通过环境变量传递

---

## Docker Compose 集群部署

支持 2 个以上 AegisGate 实例的高可用部署，带 Redis 共享状态和 Nginx 负载均衡。

### 架构

```
                    ┌──────────┐
   客户端 ─────────► │  Nginx   │ :8080
                    │ (负载均衡)│
                    └────┬─────┘
                    ┌────┴─────┐
              ┌─────┤          ├─────┐
              ▼     │          │     ▼
        ┌──────────┐│    ┌──────────┐
        │AegisGate ││    │AegisGate │
        │ 节点 1   ││    │ 节点 2   │
        └─────┬────┘│    └────┬─────┘
              │     │         │
              └─────┼─────────┘
                    ▼
              ┌──────────┐
              │  Redis   │
              └──────────┘
```

### 启动集群

```bash
# 从项目根目录
cd deploy

# 设置必要的环境变量
export OPENAI_API_KEY="sk-your-key"
export AEGISGATE_API_KEY="your-gateway-key"

# 启动所有服务
docker compose -f docker-compose.cluster.yaml up -d

# 验证
docker compose -f docker-compose.cluster.yaml ps
curl http://localhost:8080/health
```

### 服务列表

| 服务 | 端口 | 说明 |
|------|------|------|
| `nginx` | 8080 | 负载均衡器（轮询） |
| `aegisgate-1` | 8081 | AegisGate 实例 1 |
| `aegisgate-2` | 8082 | AegisGate 实例 2 |
| `redis` | 6379 | 共享状态存储 |
| `prometheus` | 9090 | 指标采集 |
| `grafana` | 3000 | 指标仪表板（admin/admin） |

### 扩容

```bash
# 添加更多 AegisGate 实例
docker compose -f docker-compose.cluster.yaml up -d --scale aegisgate-1=3
```

---

## Kubernetes Helm 部署

### 前提条件

- Kubernetes 1.24+
- Helm 3.10+
- 包含 AegisGate 镜像的容器仓库
- Redis 实例（集群模式必需）

### 安装 Helm Chart

```bash
# 先打标签并推送镜像
docker tag aegisgate:latest your-registry.example.com/aegisgate:1.0.0
docker push your-registry.example.com/aegisgate:1.0.0

# 使用 Helm 安装
helm install aegisgate helm/aegisgate/ \
  --namespace aegisgate \
  --create-namespace \
  --set image.repository=your-registry.example.com/aegisgate \
  --set image.tag=1.0.0 \
  --set config.edition=enterprise \
  --set redis.host=redis.aegisgate.svc.cluster.local \
  --set redis.port=6379
```

### 自定义配置

创建 `values-production.yaml` 文件：

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

使用自定义配置安装：

```bash
helm install aegisgate helm/aegisgate/ \
  --namespace aegisgate \
  --create-namespace \
  -f values-production.yaml
```

### 创建 Secret

```bash
kubectl create namespace aegisgate

kubectl create secret generic aegisgate-secrets \
  --namespace aegisgate \
  --from-literal=api-key='your-gateway-api-key' \
  --from-literal=openai-api-key='sk-your-openai-key'
```

### 验证部署

```bash
# 检查 Pod
kubectl get pods -n aegisgate

# 检查 Service
kubectl get svc -n aegisgate

# 端口转发到本地测试
kubectl port-forward svc/aegisgate 8080:8080 -n aegisgate

# 健康检查
curl http://localhost:8080/health
```

### 升级

```bash
helm upgrade aegisgate helm/aegisgate/ \
  --namespace aegisgate \
  -f values-production.yaml \
  --set image.tag=1.1.0
```

---

## 生产环境配置

### 安全设置

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

### 性能调优

```yaml
server:
  host: "0.0.0.0"
  port: 8080
  threads: 0                    # 0 = 自动检测 CPU 核心数
  request_timeout_seconds: 120

limits:
  max_request_body_size: 65536  # 64 KB
  max_connections: 10000

cache:
  threshold: 0.92               # 略低于默认值以提高命中率
  ttl_seconds: 7200             # 生产环境 2 小时
  max_entries: 50000            # 高流量部署需增大
  max_partitions: 64

  adaptive_threshold:
    enabled: true               # 生产环境启用自适应阈值
    min_threshold: 0.85
    max_threshold: 0.98
    adjustment_rate: 0.01
    window_size: 200

rate_limit:
  max_tokens: 50000.0           # 生产环境更高突发容量
  refill_rate: 500.0            # 持续 500 请求/秒

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

### 存储配置（企业版）

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

## 监控配置（Prometheus + Grafana）

### Prometheus 配置

AegisGate 在 `/metrics` 端点暴露 Prometheus 指标。配置 Prometheus 抓取此端点：

```yaml
# prometheus.yml
global:
  scrape_interval: 15s
  evaluation_interval: 15s

scrape_configs:
  - job_name: "aegisgate"
    metrics_path: "/metrics"
    # Docker Compose 部署
    static_configs:
      - targets: ["aegisgate-1:8080", "aegisgate-2:8080"]
    # Kubernetes 部署使用服务发现：
    # kubernetes_sd_configs:
    #   - role: pod
    #     namespaces:
    #       names: ["aegisgate"]
    # relabel_configs:
    #   - source_labels: [__meta_kubernetes_pod_label_app_kubernetes_io_name]
    #     action: keep
    #     regex: aegisgate
```

### 关键指标

| 指标 | 类型 | 说明 |
|------|------|------|
| `aegisgate_requests_total` | Counter | 按模型和状态统计的总请求数 |
| `aegisgate_request_duration_seconds` | Histogram | 请求延迟分布 |
| `aegisgate_tokens_total` | Counter | 处理的总 Token 数 |
| `aegisgate_tokens_saved_total` | Counter | 优化节省的 Token 数 |
| `aegisgate_cache_hits_total` | Counter | 语义缓存命中数 |
| `aegisgate_guardrail_blocks_total` | Counter | 被安全护栏拦截的请求数 |
| `aegisgate_upstream_errors_total` | Counter | 上游提供商错误数 |

### Grafana 仪表板

项目包含预构建的 Grafana 仪表板，位于 `deploy/grafana/dashboards/`：

```bash
# 通过 provisioning 自动导入（docker-compose.cluster.yaml 已配置）
# 或通过 Grafana UI 手动导入 JSON 文件：
#   仪表板 → 导入 → 上传 JSON 文件
```

仪表板包含以下面板：
- 请求速率和错误率（按模型）
- 延迟百分位（P50、P95、P99）
- 缓存命中率
- 安全护栏拦截率
- Token 使用量和节省量
- 上游提供商健康状态

### 告警规则（示例）

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
          summary: "AegisGate 错误率超过 5%"

      - alert: HighLatency
        expr: histogram_quantile(0.99, rate(aegisgate_request_duration_seconds_bucket[5m])) > 30
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "AegisGate P99 延迟超过 30 秒"

      - alert: CacheHitRateLow
        expr: rate(aegisgate_cache_hits_total[5m]) / rate(aegisgate_requests_total[5m]) < 0.1
        for: 15m
        labels:
          severity: info
        annotations:
          summary: "AegisGate 缓存命中率低于 10%"
```

---

## 健康检查配置

AegisGate 提供两个健康检查端点：

| 端点 | 需要认证 | 说明 |
|------|----------|------|
| `GET /health` | 否 | 基本存活检查 — 返回 `{"status":"ok","version":"..."}` |
| `GET /health/ready` | 否 | 就绪检查 — 验证管道已组装并可以提供服务 |

### Docker 健康检查

已包含在 Dockerfile 中：

```dockerfile
HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
    CMD wget --no-verbose --tries=1 --spider http://127.0.0.1:8080/health/ready || exit 1
```

### Kubernetes 探针

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

## 日志管理

### 日志配置

```yaml
logging:
  level: info           # debug | info | warn | error
  file: "logs/aegisgate.log"   # 为空则仅输出到 stdout
```

### 结构化日志格式

AegisGate 使用 spdlog 进行结构化日志记录。日志条目格式：

```
[2026-04-15 10:30:45.123] [info] [api_controller] Request processed model=gpt-4 status=ok latency_ms=1523 tokens=150
```

### 日志轮转

对于文件日志，使用 `logrotate` 配置日志轮转：

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

### Docker / Kubernetes 日志收集

在容器化环境中，将 `logging.file` 设为空（`""`）使日志输出到 stdout，然后使用日志聚合工具收集：

- **Docker**: `docker logs aegisgate` 或 Docker 日志驱动（fluentd、gelf 等）
- **Kubernetes**: 通过 Fluentd/Fluent Bit DaemonSet 或 Loki + Promtail 收集

### 审计日志

审计日志独立于应用日志，包含完整的请求/响应审计轨迹及完整性链哈希：

```yaml
audit:
  log_path: "logs/audit.log"
  retention_days: 90    # 0 = 永久保留
```

审计日志应存储在持久化存储上，并定期备份以满足合规要求。

---

## 备份与恢复

### 备份内容

| 组件 | 路径 | 频率 | 方法 |
|------|------|------|------|
| 配置文件 | `config/` | 变更时 | Git 版本控制 |
| SQLite 数据库 | `data/aegisgate.db` | 每天 | 文件复制（先执行 WAL checkpoint） |
| 审计日志 | `logs/audit.log` | 每天 | 文件复制或日志转发 |
| 语义缓存 | 内存中（重启后丢失） | 不适用 | 启动时从 CacheStore 预热 |
| TLS 证书 | `/app/certs/` | 轮换时 | 安全备份 |

### SQLite 备份

```bash
# 安全备份（推荐，含 WAL checkpoint）
sqlite3 data/aegisgate.db ".backup 'backup/aegisgate-$(date +%Y%m%d).db'"

# 或使用在线备份 API 实现零停机备份
sqlite3 data/aegisgate.db "PRAGMA wal_checkpoint(TRUNCATE);"
cp data/aegisgate.db backup/aegisgate-$(date +%Y%m%d).db
```

### PostgreSQL 备份（企业版）

```bash
pg_dump -h $POSTGRES_HOST -U aegisgate -d aegisgate \
  --format=custom \
  --file=backup/aegisgate-$(date +%Y%m%d).pgdump
```

### 恢复步骤

1. **停止 AegisGate**（或在 Kubernetes 中缩容到 0 副本）
2. **恢复配置**：复制配置文件到 `config/`
3. **恢复数据库**：复制备份到 `data/aegisgate.db`（SQLite）或使用 `pg_restore`（PostgreSQL）
4. **恢复审计日志**：复制到 `logs/`
5. **启动 AegisGate**：语义缓存会在启动时从 CacheStore 后端预热
6. **验证**：运行健康检查并发送测试请求

---

## 安全加固

### 检查清单

- [ ] 启用 TLS（`tls.enabled: true`）或在负载均衡器 / Ingress 处终止 TLS
- [ ] 设置强 API 密钥（至少 32 个字符，随机生成）
- [ ] 启用滥用检测（`security.abuse_detection.enabled: true`）
- [ ] 启用 Unicode 规范化和编码检测
- [ ] 设置 `limits.max_request_body_size` 防止超大载荷（默认：64 KB）
- [ ] 以非 root 用户运行（Docker 镜像默认使用 `aegisgate` 用户）
- [ ] 以只读方式挂载配置卷
- [ ] 将密钥存储在环境变量或 Kubernetes Secret 中，永远不要放在配置文件里
- [ ] 启用审计日志并设置足够的保留期以满足合规要求
- [ ] 尽可能通过 IP 限制管理端点（`admin.allowed_ips`）
- [ ] 定期轮换 API 密钥
- [ ] 及时更新 AegisGate 镜像以获取安全补丁

完整的安全指南请参阅[安全最佳实践](security-best-practices_zh.md)。
