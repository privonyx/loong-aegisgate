# FeedbackBus Guide

> Feature: Process-internal feedback event bus (Phase 11.0)
> Available: v1.1+ (delivered incrementally under the v3.0 Phase 11 roadmap)

`FeedbackBus` is the **data loop backbone** for every autonomy-related module in AegisGate — adaptive guards, self-tuning routers, self-healing ops, and the cost optimizer 2.0 all publish their observations to the bus and subscribe to the events they need. This guide explains how to enable it, publish events, subscribe to topics, and integrate it with your own Phase 11.x subsystem.

## Why a feedback bus?

Before Phase 11.0, every autonomy prototype would have to wire its own channel from the request path to its learner/trainer. That led to:

- fragmented event schemas (every module rolls its own),
- un-composable subscribers (same feedback can't feed two learners), and
- broken observability (audit/metrics/tracing have to re-integrate per module).

`FeedbackBus` fixes all three with a single, lightweight in-memory bus.

## FeedbackBus vs AuditLogger

| | FeedbackBus | AuditLogger |
|---|---|---|
| Intent | Best-effort signals for learning & observation | Compliance-grade audit trail |
| Delivery | **Lossy** — drops oldest on overflow | **Durable** — chained hash + encryption + persistent store |
| Typical consumers | Trainers, routers, optimizers, Prometheus | SIEM, compliance exports, forensic inspection |
| Default state | `enabled: false` | Always enabled |

They are complementary, not competing. An event can be published to both.

## Event model

```cpp
enum class FeedbackEventType {
    GuardFeedback,        // "guard.feedback"
    GuardAnomalyFlagged,  // "guard.anomaly"
    RouterOutcome,        // "router.outcome"
    RouterDecision,       // "router.decision"
    QualityFeedback,      // "quality.feedback"
    QualityDrift,         // "quality.drift"
    CostObservation,      // "cost.observation"
    BudgetAlert,          // "cost.budget_alert"
    OpsIncident,          // "ops.incident"
    OpsRollbackTriggered, // "ops.rollback"
    Custom,               // "custom"
};

struct FeedbackEvent {
    FeedbackEventType type;
    std::string topic;       // stable topic string (e.g. "guard.feedback")
    std::string request_id;  // optional correlation id
    std::string tenant_id;
    std::string source;      // who produced it
    std::chrono::system_clock::time_point timestamp;
    nlohmann::json payload;  // open-ended module-specific data
};
```

`topicOf(T)` ↔ `typeOf(s)` is a **stable public mapping**. Adding a new topic is a MINOR version change; renaming or removing one is MAJOR.

## Enable it

```yaml
# config/aegisgate.yaml
autonomy:
  feedback_bus:
    enabled: true
    max_queue_size: 10000        # drops oldest when full
    drop_policy: oldest           # v0 only supports "oldest"
```

The gateway exposes a process-wide singleton:

```cpp
auto& bus = aegisgate::FeedbackBus::instance();
```

Bootstrap (typically in `GatewayRuntime::initialize`):

```cpp
if (config.feedbackBusEnabled()) {
    aegisgate::FeedbackBusConfig cfg;
    cfg.enabled = true;
    cfg.max_queue_size = config.feedbackBusMaxQueueSize();
    cfg.drop_policy = config.feedbackBusDropPolicy();
    bus.reconfigure(cfg);
    bus.start();
}
```

## Publishing events

```cpp
#include "aegisgate/feedback_event.h"
#include "observe/feedback_bus.h"

aegisgate::FeedbackEvent ev;
ev.type        = aegisgate::FeedbackEventType::RouterOutcome;
ev.topic       = aegisgate::FeedbackEvent::topicOf(ev.type);
ev.request_id  = ctx.request_id;
ev.tenant_id   = ctx.tenant_id;
ev.source      = "MLRouter";
ev.timestamp   = std::chrono::system_clock::now();
ev.payload     = {{"model", "gpt-4o"}, {"latency_ms", 1234}, {"success", true}};

aegisgate::FeedbackBus::instance().publish(std::move(ev));
```

`publish()` is **non-blocking O(1)**: it pushes the event onto the bounded queue and returns. Returns `false` if the bus is disabled or stopped.

## Subscribing

```cpp
auto& bus = aegisgate::FeedbackBus::instance();

// Subscribe to every event
auto id_all = bus.subscribe([](const aegisgate::FeedbackEvent& e) {
    // process...
});

// Subscribe to a prefix — receives guard.feedback, guard.anomaly, etc.
auto id_guard = bus.subscribe([](const aegisgate::FeedbackEvent& e) {
    // process guard feedback
}, "guard.");

// Subscribe to an exact topic
auto id_router_outcome = bus.subscribe([](const aegisgate::FeedbackEvent& e) {
    // process router outcome only
}, "router.outcome");

// Unsubscribe with constant-time removal
bus.unsubscribe(id_all);
```

Subscribers run on the dispatcher thread. **Do not block** in them — offload heavy work to your own thread if needed. Exceptions in one subscriber are isolated from others (caught + logged via `spdlog::warn` + counted in `delivery_errors`).

## Built-in Metrics subscriber

The SDK ships an example subscriber that bridges the bus to Prometheus:

```cpp
#include "observe/metrics_feedback_subscriber.h"

aegisgate::Counter events("feedback_events_total", "Feedback events by topic");
aegisgate::MetricsFeedbackSubscriber metrics_sub(events);
metrics_sub.attach(aegisgate::FeedbackBus::instance());

// Now every event increments feedback_events_total{type=<topic>}
```

## Lifecycle

| Call | Effect |
|------|--------|
| `start()` | Starts the dispatcher thread (idempotent) |
| `shutdown()` | Stops dispatcher + **synchronously drains** remaining events |
| `flush(timeout)` | Blocks until queue is empty AND in-flight dispatches finish |
| `reconfigure(cfg)` | Dynamically updates enable/max_queue_size |

## Observability

`FeedbackBus::stats()` returns:

```
published            total events enqueued
delivered            total callbacks invoked successfully
dropped_queue_full   events dropped due to full queue
delivery_errors      subscriber exceptions
queue_size           current queue depth
subscriber_count     currently registered subscribers
```

Expose these via your existing metrics pipeline (e.g. `feedback_bus_events_total`, `feedback_bus_drops_total`, `feedback_bus_subscriber_errors_total`).

## Runtime integration (Phase 11.0)

`GatewayRuntime::initialize()` automatically wires the bus when
`autonomy.feedback_bus.enabled=true`:

1. Configures `FeedbackBus::instance()` from `Config::feedbackBus*()` getters
2. Calls `bus.start()` to launch the dispatcher thread
3. Lazily creates a process-wide `MetricsFeedbackSubscriber` and attaches it
   to the bus — every published event increments `feedback_events_total{type=<topic>}`
4. On `GatewayRuntime::shutdown()`, the bus is `flush()`ed (≤ 2 s) and
   `shutdown()`ed **before** the audit logger and persistent stores close,
   so subscriber-emitted writes still have their dependencies.

The result: no manual `bus.start()` calls in production code; just enable
the YAML key and the Prometheus counter starts populating.

## Roadmap

| Phase | Item |
|-------|------|
| ✅ 11.0.1 | FeedbackBus + event schema + metrics subscriber (this PR) |
| ✅ 11.0.2 | Runtime wiring (GatewayRuntime + Prometheus) — this section |
| 11.1.1 | `POST /admin/api/guard/feedback` → publishes `GuardFeedback` |
| 11.1.2 | Online trainer subscribes `guard.*` for data set construction |
| 11.2.1 | `MultiObjectiveRouter` subscribes `router.outcome` for real-time updates |
| 11.4.2 | `AutoRollbackController` subscribes `ops.incident` + `quality.drift` |
| 11.5.1 | `CostOptimizer 2.0` subscribes `cost.observation` + `quality.feedback` |

## Related docs

- Specification: `docs/specs/2026-04-17-phase11.0-feedback-bus-design.md`
- Lock ordering: `docs/LOCK_ORDERING.md` (Layer 3)
- Public header: `include/aegisgate/feedback_event.h`
- Roadmap: `docs/ROADMAP.md`
