# AegisGate Performance Tuning

This document is for self-hosted deployments and describes configuration related to latency, throughput, and memory. All paths follow the repository default layout: `config/aegisgate.yaml`, `config/models.yaml`.

## Semantic Cache

Relevant section: `cache` (see the default `aegisgate.yaml`).

| Parameter | Role | Tuning guidance |
|-----------|------|-----------------|
| `threshold` | Vector similarity threshold; higher values make hits harder | When hit rate is low, **lower** moderately (e.g. 0.90→0.88); when false hits are frequent, **raise** it |
| `ttl_seconds` | Cache entry time-to-live | Shorten for time-sensitive scenarios; extend for static Q&A |
| `max_entries` | Maximum entries (with LRU) | Increase when memory is ample; watch footprint from embedding dimension × partition count |
| `max_partitions` | Number of partitions | More partitions reduce per-partition lock contention; too many add management and memory overhead |
| `context_aware` | Whether context is considered | More accurate but more compute; for minimum latency consider disabling (accept false-hit risk) |
| `policy` | Skip certain models / high temperature / streaming, etc. | Skipping requests unsuitable for cache reduces wasted embeddings and index writes |
| `adaptive_threshold` | Dynamic threshold | Enable when traffic patterns vary a lot, and set `min_threshold` / `max_threshold` bounds |

**Embedding backend**: Enabling ONNX (`-DENABLE_ONNX=ON`) and BGE models improves semantic quality but increases CPU and first-load time; hash embeddings have lower latency and weaker semantics. See the root `README.md`.

## Rate Limit

Section: `rate_limit`.

- **`max_tokens`**: Token bucket capacity (burst allowance). Increase moderately for large bursts; too large weakens protection.
- **`refill_rate`**: Tokens added per second. Sustained RPS is roughly bounded by this and bucket size together.

During load tests, if you see many **429 / AEGIS-2001**, distinguish gateway rate limiting from upstream limits: tune `rate_limit` for the former; for the latter, add Key `weight` in `models.yaml`, rotate multiple keys, or reduce concurrency.

## Connections and Upstream

On each provider in `config/models.yaml`:

- **`timeout_ms`**: Too large stretches tail latency; too small increases **AEGIS-4003**. Leave 20%–50% headroom over model P95 latency.
- **`max_retries`**: Slightly improves success rate but amplifies tail latency; balance with circuit-breaker policy.

On the gateway: `limits.max_connections` caps concurrent load on the process; tune using machine memory and file descriptor limits.

## Threads and Drogon

Section: `server`:

```yaml
server:
  host: "0.0.0.0"
  port: 8080
  threads: 0   # 0 = auto-detect based on CPU cores
```

For CPU-bound work (e.g. ONNX on the same host), too many threads can increase context-switch cost. Try setting `threads` to **physical core count** or slightly lower, validated under load.

## Storage Backend

- **`storage.cache_backend`**: `memory` has lowest latency; Redis (enterprise build) suits shared cache across instances.
- **`storage.sqlite.wal_mode: true`**: WAL is on by default and helps concurrent reads; on slow disks consider SSD or Postgres (enterprise).

## Baseline with `aegisctl bench`

```bash
./build/aegisctl --url http://127.0.0.1:8080 \
  --api-key "$AEGISGATE_API_KEY" \
  bench --concurrency 20 --requests 200 --model gpt-4o-mini --prompt "Say hello"
```

Output includes RPS and p50/p90/p99 latency (ms). Re-run the same command after config changes to compare.

Common flags:

- `--concurrency`: Concurrent connections (default 10)
- `--requests`: Total requests (default 100)
- `--model` / `--prompt`: Override default model and prompt

## Baseline Performance Expectations (rules of thumb)

The following are **order-of-magnitude** only; actual behavior depends on hardware, cache hits, upstream models, and network:

- **Pure gateway overhead** (loopback, tiny body): usually sub-millisecond to a few milliseconds (excluding upstream).
- **Cache hits**: Can be much lower than the full inference path; benefit ties to `threshold` and embedding cost.
- **First request**: May include model load and connection pool warm-up; send warm-up requests before benchmarking.

Do not compare community single-node QPS directly to a cloud vendor’s managed gateway; use `bench` results in your environment.

## Common Issues and Mitigations

| Symptom | Likely cause | Action |
|---------|--------------|--------|
| Very high P99 | Upstream sporadic slowness, retries stacking | Tune `timeout_ms`, cap retries, enable fallback |
| CPU saturated | ONNX embeddings, too many guard rules | Lower concurrency, switch to hash embeddings, slim rules |
| Memory keeps rising | Large cache entries, `max_entries` too high | Lower TTL/entry cap, review partition settings |
| Throughput capped | Single-key upstream limits | Multi-key load balancing, multiple providers |
| Normal latency but many errors | Circuit breaker or abuse detection | Check audit logs and metrics; fix upstream or adjust abuse thresholds |

## Observability

- **`/metrics`**: Prometheus text format for finding bottlenecks (request volume, error rates, histograms).
- **Audit logs**: `audit.log_path`; use `aegisctl logs tail` (admin key required) for live tail.

### Metrics worth watching (conceptual)

On dashboards fed by Prometheus scraping, prioritize:

- Request rate and status-code breakdown (whether 4xx/5xx spike).
- Whether latency histogram p95/p99 match upstream SLA.
- If cache metrics are exported, compare hit rate with `threshold` tuning.

Exact metric names match the current `/metrics` output; use `curl -s http://127.0.0.1:8080/metrics | head` for a quick look.

## Request Body and Serialization

`limits.max_request_body_size` caps single JSON payload size. Very large context causes:

- Longer parse and guard scan time;
- Higher memory peaks.

If the product must move huge messages, shard, summarize, or use external RAG **in front of** the gateway instead of only raising the limit (which widens the attack surface).

## Guardrails and ONNX Classifier

With `security.guard_model` enabled, you pay one (or more) local inference passes:

- At high QPS, evaluate **batching** and **sampling** (if supported) or enable only on sensitive routes.
- A `threshold` that is too low adds compute and false positives; too high raises miss risk.

When disabled, keyword/regex and other heuristics still apply with different performance characteristics—validate under load.

## Abuse Detection and Throughput

`security.abuse_detection` maintains per-window counts and is usually cheap; under **extreme concurrency**, watch for lock contention. For mostly internal services, slightly looser thresholds can reduce how many clients enter throttling.

## Warm-up and Cold Start

Before formal load tests:

1. After start, send a few light `/v1/chat/completions` or `aegisctl chat` calls to warm the pool and (if enabled) ONNX session init.
2. Then run `aegisctl bench` so “first inference latency” is not mistaken for steady-state performance.

## Trade-offs with Correctness

Performance tuning can conflict with security and cache consistency, for example:

- Relaxing PII or injection rules may reduce 403s but increases data-exfiltration risk;
- Higher cache hit rate may return answers that are “semantically close but not identical.”

After changes, regress with a **business golden case set** in staging, then watch production metrics.

## Related Documents

- [Quick Start](./quick-start.md)
- [Troubleshooting](./troubleshooting.md)
- [Security Best Practices](./security-best-practices.md)
