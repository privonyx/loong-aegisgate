# AegisGate Lock Ordering Specification

> This document defines the hierarchical ordering of all mutexes to prevent deadlocks. Any new mutex must be registered here.

## Core rules

1. **No nested locks at the same layer** — While holding a Layer N lock, you must not acquire another lock at Layer N
2. **Only increasing direction** — While holding a Layer N lock, you may only acquire a Layer M lock where M > N
3. **New mutexes must be registered** — Document the layer in this file and in the header

## Lock layers

| Layer | Component | Lock Type | Header File | Description |
|-------|-----------|-----------|-------------|-------------|
| 0 | `Config` | `shared_mutex` | `core/config.h` | Global configuration read-write lock; highest priority |
| 1 | `SemanticCache` | `mutex` | `cache/semantic_cache.h` | Cache state (entries/LRU) |
| 1 | `RateLimiter::Shard` | `mutex` | `gateway/rate_limiter.h` | Token bucket shards (16 independent locks) |
| 1 | `CircuitBreaker::Shard` | `mutex` | `gateway/circuit_breaker.h` | Circuit breaker shards (16 independent locks) |
| 1 | `AbuseDetector::Shard` | `mutex` | `gateway/abuse_detector.h` | Abuse frequency + SimHash fingerprint shards (16 independent locks) |
| 1 | `Balancer` | `mutex` | `gateway/balancer.h` | Load-balancing weights |
| 2 | `MemoryCacheStore` | `mutex` | `storage/memory_cache_store.h` | In-memory KV cache |
| 2 | `MemoryPersistentStore` | `mutex` | `storage/memory_persistent_store.h` | In-memory persistent storage |
| 2 | `SQLitePersistentStore` | `mutex` | `storage/sqlite_persistent_store.h` | Serialize SQLite operations |
| 2 | `ConnectionPool` | `mutex` | `storage/connection_pool.h` | Connection pool acquire/release |
| 3 | `VectorIndex` | `mutex` | `cache/vector_index.h` | hnswlib vector operations |
| 3 | `Metrics::Counter` | `mutex` | `observe/metrics.h` | Metrics counter |
| 3 | `Metrics::Histogram` | `mutex` | `observe/metrics.h` | Metrics histogram |
| 3 | `Metrics::Gauge` | `mutex` | `observe/metrics.h` | Metrics gauge |
| 3 | `AuditLogger::mutex_` | `mutex` | `guardrail/audit.h` | Audit log in-memory entries + sink |
| — | `AuditLogger::queue_mutex_` | `mutex` | `guardrail/audit.h` | Async persistence queue (internal implementation; not part of the layer hierarchy) |
| 3 | `CostTracker` | `mutex` | `observe/cost_tracker.h` | Cost recording |
| 3 | `AlertManager` | `mutex` | `observe/alerting.h` | Alert recording |
| 3 | `RequestLogger` | `mutex` | `observe/request_logger.h` | Request logging |
| 3 | `FeedbackBus::mutex_` | `mutex` | `observe/feedback_bus.h` | Subscriber registry for feedback events |
| — | `FeedbackBus::queue_mutex_` | `mutex` | `observe/feedback_bus.h` | Async event queue (internal implementation; NEVER nested with `FeedbackBus::mutex_`) |
| 3 | `ApprovalQueue::mutex_` | `mutex` | `observe/autonomy/approval_queue.h` | In-memory cache of autonomy proposals (mirror of `PersistentStore::autonomy_proposals`); store I/O issued OUTSIDE the lock (CostTracker pattern) |
| 3 | `AutonomyApprovalWorkflow::mutex_` | `mutex` | `observe/autonomy/approval_workflow.h` | Applier dispatch table; never nested with `ApprovalQueue::mutex_` (workflow calls queue lock-free) |

## Known multi-lock call paths

### SemanticCache → CacheStore (Layer 1 → Layer 2)

`SemanticCache::put()` Phase 3 holds the SC lock and calls `persistEntry()` → `CacheStore::set()`.
When the CacheStore is MemoryCacheStore, the Layer 2 lock is taken. Compliant.

```
SC::mutex_ [Layer 1] → MemoryCacheStore::mutex_ [Layer 2]  ✅
SC::mutex_ [Layer 1] → ConnectionPool::mutex_ [Layer 2]    ✅ (Redis path)
```

### SemanticCache → VectorIndex (Layer 1, Layer 3 alternating)

`SemanticCache::put()` uses a three-phase design and does **not** nest the two locks:
- Phase 1: SC lock (allocate ID + eviction)
- Phase 2: **no SC lock**, only VI lock (remove + insert)
- Phase 3: SC lock (update entry + persist)

```
SC::mutex_ [Layer 1] → release → VectorIndex::mutex_ [Layer 3] → release → SC::mutex_ [Layer 1]  ✅
```

### CostTracker → PersistentStore (I/O outside lock)

`CostTracker::record()` updates in-memory state under the lock first, then calls PersistentStore persistence **outside** the lock. No nesting.

### ConnectionPool::acquire (Layer 2 → outside lock)

Connection is taken under the lock; health checks and rebuild run **outside** the lock. No nesting.

### AuditLogger::log → async queue (Layer 3 → internal queue lock)

`AuditLogger::log()` first updates in-memory entries under `mutex_` [Layer 3], then after release enqueues under `queue_mutex_` (internal implementation lock).
The background thread holds `queue_mutex_`, batches dequeue, then calls PersistentStore outside the lock. `mutex_` and `queue_mutex_` are never nested.

### ApprovalQueue → PersistentStore (Layer 3 → I/O outside lock)

`ApprovalQueue::insert()` / `update()` / `prune()` call `PersistentStore` (Layer 2) FIRST
without holding `mutex_`, then re-acquire `mutex_` solely to mutate `cache_`. This matches
the CostTracker pattern and stays compliant with the increasing-layer rule. `get()` and
`list()` only touch the cache.

### AutonomyApprovalWorkflow → ApprovalQueue + AuditLogger (Layer 3 alternating, never nested)

`AutonomyApprovalWorkflow::propose/approve/reject/apply/rollback` call
`ApprovalQueue` (Layer 3) and `AuditLogger::logAction()` (Layer 3) sequentially without
holding `mutex_`. The workflow's own `mutex_` only guards the `appliers_` dispatch table
and is acquired briefly during `registerApplier()` / `findApplier()` — those calls do
not nest into queue/audit.

### FeedbackBus::publish + dispatcher (Layer 3 + internal queue lock)

`FeedbackBus::publish()` takes only `queue_mutex_` (internal) to push the event and release; it does not touch `mutex_`.
The dispatcher thread's `deliver()` phase takes `mutex_` [Layer 3] solely to **snapshot** the subscriber map into a local vector, then releases the lock before invoking any callback (lock-outside-I/O pattern). `mutex_` and `queue_mutex_` are never nested.

## Adding a new mutex

1. Determine the layer (compare with similar components in the table above)
2. Add a comment at the mutex declaration in the header: `// Lock Layer N — see docs/LOCK_ORDERING.md`
3. Register it in the layer table in this document
4. Verify call paths do not violate the increasing-order rule
