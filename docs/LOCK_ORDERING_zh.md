# AegisGate 锁顺序规范

> 本文档定义所有 mutex 的层级顺序，防止死锁。任何新增 mutex 必须在此注册。

## 核心规则

1. **同层锁禁止嵌套持有** — 不得在持有 Layer N 锁时获取同层另一个锁
2. **仅允许递增方向** — 持有 Layer N 锁时，仅可获取 Layer M 锁（M > N）
3. **新增 mutex 必须注册** — 在本文档和头文件中标注层级

## 锁层级

| Layer | 组件 | 锁类型 | 头文件 | 说明 |
|-------|------|--------|--------|------|
| 0 | `Config` | `shared_mutex` | `core/config.h` | 全局配置读写锁，最高优先级 |
| 1 | `SemanticCache` | `mutex` | `cache/semantic_cache.h` | 缓存状态（entries/LRU） |
| 1 | `RateLimiter::Shard` | `mutex` | `gateway/rate_limiter.h` | 令牌桶分片（16 个独立锁） |
| 1 | `CircuitBreaker::Shard` | `mutex` | `gateway/circuit_breaker.h` | 熔断器分片（16 个独立锁） |
| 1 | `Balancer` | `mutex` | `gateway/balancer.h` | 负载均衡权重 |
| 2 | `MemoryCacheStore` | `mutex` | `storage/memory_cache_store.h` | 内存 KV 缓存 |
| 2 | `MemoryPersistentStore` | `mutex` | `storage/memory_persistent_store.h` | 内存持久化存储 |
| 2 | `SQLitePersistentStore` | `mutex` | `storage/sqlite_persistent_store.h` | SQLite 操作序列化 |
| 2 | `ConnectionPool` | `mutex` | `storage/connection_pool.h` | 连接池获取/释放 |
| 3 | `VectorIndex` | `mutex` | `cache/vector_index.h` | hnswlib 向量操作 |
| 3 | `Metrics::Counter` | `mutex` | `observe/metrics.h` | 指标计数器 |
| 3 | `Metrics::Histogram` | `mutex` | `observe/metrics.h` | 指标直方图 |
| 3 | `Metrics::Gauge` | `mutex` | `observe/metrics.h` | 指标仪表 |
| 3 | `AuditLogger::mutex_` | `mutex` | `guardrail/audit.h` | 审计日志内存条目 + sink |
| — | `AuditLogger::queue_mutex_` | `mutex` | `guardrail/audit.h` | 异步持久化队列（内部实现，不在层级体系中） |
| 3 | `CostTracker` | `mutex` | `observe/cost_tracker.h` | 成本记录 |
| 3 | `AlertManager` | `mutex` | `observe/alerting.h` | 告警记录 |
| 3 | `RequestLogger` | `mutex` | `observe/request_logger.h` | 请求日志 |

## 已知多锁调用路径

### SemanticCache → CacheStore (Layer 1 → Layer 2)

`SemanticCache::put()` Phase 3 持有 SC 锁调用 `persistEntry()` → `CacheStore::set()`。
当 CacheStore 为 MemoryCacheStore 时，获取 Layer 2 锁。合规。

```
SC::mutex_ [Layer 1] → MemoryCacheStore::mutex_ [Layer 2]  ✅
SC::mutex_ [Layer 1] → ConnectionPool::mutex_ [Layer 2]    ✅ (Redis 路径)
```

### SemanticCache → VectorIndex (Layer 1, Layer 3 交替)

`SemanticCache::put()` 采用 3 阶段设计，**不嵌套**持有两个锁：
- Phase 1: SC 锁（分配 ID + 淘汰）
- Phase 2: **无 SC 锁**，仅 VI 锁（remove + insert）
- Phase 3: SC 锁（更新 entry + persist）

```
SC::mutex_ [Layer 1] → release → VectorIndex::mutex_ [Layer 3] → release → SC::mutex_ [Layer 1]  ✅
```

### CostTracker → PersistentStore (锁外 I/O)

`CostTracker::record()` 先在锁内更新内存状态，**锁外**调用 PersistentStore 持久化。不涉及嵌套。

### ConnectionPool::acquire (Layer 2 → 锁外)

锁内取连接，**锁外**执行健康检查和重建。不涉及嵌套。

### AuditLogger::log → 异步队列 (Layer 3 → 内部队列锁)

`AuditLogger::log()` 先在 `mutex_` [Layer 3] 内更新内存条目，释放后在 `queue_mutex_`（内部实现锁）内入队。
后台线程持有 `queue_mutex_` 批量出队后锁外调用 PersistentStore。`mutex_` 和 `queue_mutex_` 不嵌套。

## 新增 mutex 流程

1. 确定所属层级（参考上表的同类组件）
2. 在头文件 mutex 声明处添加注释：`// Lock Layer N — see docs/LOCK_ORDERING.md`
3. 在本文档的层级表中注册
4. 验证调用路径不违反递增规则
