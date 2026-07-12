# AegisGate 性能调优

本文面向自建部署，说明与延迟、吞吐、内存相关的配置项。所有路径均以仓库默认布局为准：`config/aegisgate.yaml`、`config/models.yaml`。

## 语义缓存（Semantic Cache）

相关配置节：`cache`（见默认 `aegisgate.yaml`）。

| 参数 | 作用 | 调优建议 |
|------|------|----------|
| `threshold` | 向量相似度阈值，越高越难命中 | 命中率低时适当**降低**（如 0.90→0.88）；误命中多时**升高** |
| `ttl_seconds` | 缓存条目生存时间 | 对时效敏感场景缩短；静态问答可延长 |
| `max_entries` | 最大条目数（配合 LRU） | 内存充足时可增大；注意与嵌入维度、分区数乘积带来的占用 |
| `max_partitions` | 分区数 | 提高可减少单分区锁竞争；过大增加管理与内存开销 |
| `context_aware` | 是否考虑上下文 | 更准但计算更多；极致延迟可评估关闭（需接受误命中风险） |
| `policy` | 跳过某些模型/高温/流式等 | 对不适合缓存的请求跳过，可减少无效嵌入与索引写入 |
| `adaptive_threshold` | 动态阈值 | 流量模式变化大时可开启，并设置 `min_threshold` / `max_threshold` 边界 |

**嵌入后端**：启用 ONNX（`-DENABLE_ONNX=ON`）与 BGE 模型可提高语义质量，但增加 CPU/首次加载时间；哈希嵌入延迟更低、语义较弱。详见根目录 `README.md`。

## 限流（Rate Limit）

配置节：`rate_limit`。

- **`max_tokens`**：令牌桶容量（突发允许量）。突发流量大时适度增大；过大则失去保护意义。
- **`refill_rate`**：每秒补充速率。持续 RPS 上限约受此值与桶容量共同约束。

压测时若大量 **429 / AEGIS-2001**，应区分是网关限流还是上游限流：前者调 `rate_limit`；后者需在 `models.yaml` 中增加 Key `weight`、多 Key 轮询或降低并发。

## 连接与上游

在 `config/models.yaml` 的每个 provider 上：

- **`timeout_ms`**：过大拖长尾延迟；过小增加 **AEGIS-4003**。按模型 P95 延迟留出 20%～50% 余量。
- **`max_retries`**：略可提高成功率，但会放大尾部延迟；与熔断策略一起权衡。

网关侧：`limits.max_connections` 防止过多并发拖垮进程；根据机器内存与文件描述符上限调整。

## 线程与 Drogon

配置节：`server`：

```yaml
server:
  host: "0.0.0.0"
  port: 8080
  threads: 0   # 0 表示按 CPU 核心自动
```

CPU 密集（如同机 ONNX 推理）时，线程数过高可能导致上下文切换开销。可尝试将 `threads` 设为**物理核心数**或略低，并以压测为准。

## 存储后端

- **`storage.cache_backend`**：`memory` 延迟最低；Redis（企业版构建）适合多实例共享缓存。
- **`storage.sqlite.wal_mode: true`**：默认已开启 WAL，有利于并发读；磁盘慢时考虑 SSD 或换 Postgres（企业版）。

## 使用 `aegisctl bench` 做基线

```bash
./build/aegisctl --url http://127.0.0.1:8080 \
  --api-key "$AEGISGATE_API_KEY" \
  bench --concurrency 20 --requests 200 --model gpt-4o-mini --prompt "Say hello"
```

输出包含 RPS 与 p50/p90/p99 延迟（毫秒）。变更配置后重复同一命令对比。

常用参数：

- `--concurrency`：并发连接数（默认 10）
- `--requests`：总请求数（默认 100）
- `--model` / `--prompt`：覆盖默认模型与提示词

## 基线性能预期（经验值）

以下仅为**数量级参考**，实际取决于硬件、是否命中缓存、上游模型与网络：

- **纯网关开销**（本地回环、极小 Body）：通常亚毫秒级～数毫秒级（不含上游）。
- **缓存命中**：可显著低于完整推理路径；收益与 `threshold`、嵌入成本相关。
- **首次请求**：可能包含模型加载、连接池预热，宜在压测前发送预热请求。

勿将社区版单机 QPS 与云厂商托管网关直接对比；请以你的环境下的 `bench` 结果为准。

## 常见问题与对策

| 现象 | 可能原因 | 处理 |
|------|----------|------|
| P99 很高 | 上游偶发慢、重试叠加 | 调 `timeout_ms`、限制重试、启用 fallback |
| CPU 打满 | ONNX 嵌入、护栏规则过多 | 降并发、换哈希嵌入、精简规则 |
| 内存持续上涨 | 缓存条目大、`max_entries` 过大 | 调低 TTL/条目上限、检查分区配置 |
| 吞吐上不去 | 单 Key 上游限流 | 多 Key 负载均衡、多 Provider |
| 延迟正常但错误多 | 熔断或滥用检测 | 查审计日志与 metrics，修复上游或调整滥用阈值 |

## 可观测性

- **`/metrics`**：Prometheus 文本格式，用于定位瓶颈（请求量、错误率、直方图）。
- **审计日志**：`audit.log_path`；配合 `aegisctl logs tail`（需管理员 Key）实时查看。

### 建议关注的指标（概念）

结合 Prometheus 拉取后的面板，可优先观察：

- 请求速率与按状态码分布（4xx/5xx 是否突增）。
- 延迟直方图 p95/p99 是否与上游 SLA 一致。
- 若导出缓存相关指标，对比命中率与 `threshold` 调整效果。

具体指标名称以当前版本 `/metrics` 输出为准，可用 `curl -s http://127.0.0.1:8080/metrics | head` 快速查看。

## 请求体与序列化

`limits.max_request_body_size` 限制单次 JSON 大小。超大上下文会导致：

- 解析与护栏扫描时间变长；
- 内存峰值升高。

若业务必须传输极大消息，应在网关前做分段、摘要或外挂 RAG，而不是一味调大上限（安全面扩大）。

## 护栏与 ONNX 分类器

`security.guard_model` 启用后，会增加一次（或多次）本地推理成本：

- 高 QPS 场景评估 **批量** 与 **采样** 策略（若版本支持）或仅在敏感路由开启。
- `threshold` 过低会增加计算与误报；过高则漏检风险上升。

未启用时，仍有关键字/正则与其它启发式，性能特征不同，需以压测为准。

## 滥用检测对吞吐的影响

`security.abuse_detection` 在窗口内维护计数，通常开销较小；但在**极端高并发**下，仍需观察是否成为锁竞争热点。若主要为内网服务，可适当放宽阈值以减少进入节流状态的客户端数量。

## 预热与冷启动

正式压测前建议：

1. 启动进程后先发送数次轻量 `/v1/chat/completions` 或 `aegisctl chat`，触发连接池与（若启用）ONNX 会话初始化。
2. 再运行 `aegisctl bench`，避免将「首次推理延迟」误认为稳态性能。

## 与「正确性」的权衡

性能调优有时会与安全、缓存一致性冲突，例如：

- 放宽 PII 或注入规则可减少 403，但增加数据泄露风险；
- 提高缓存命中率可能返回「语义接近但不完全一致」的答案。

变更配置后应在预发环境用**业务黄金用例集**回归，再观察生产 metrics。

## 相关文档

- [快速开始](./quick-start_zh.md)
- [故障排查](./troubleshooting_zh.md)
- [安全最佳实践](./security-best-practices_zh.md)
