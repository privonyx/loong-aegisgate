# FeedbackBus 指南

> 覆盖功能：进程内反馈事件总线（Phase 11.0）
> 可用版本：v1.1+（在 v3.0 路线图 Phase 11 框架下渐进交付）

`FeedbackBus` 是 AegisGate 所有自治模块（学习型护栏 / 自演进路由 / 自愈运维 / 成本优化器 2.0）的**数据闭环底座** — 它们都向总线发布观测，并订阅自己需要的事件。本指南介绍如何启用、发布事件、订阅 topic，并把 Phase 11.x 的自治子系统接入这条总线。

## 为什么需要反馈总线

在 Phase 11.0 之前，每个自治原型都要自己从请求路径到 trainer/learner 拉一条专用通道，结果：

- 事件格式碎片化（每个模块自己一套）
- 订阅者无法组合（同一条反馈无法同时喂两个学习器）
- 可观测性断裂（审计 / 指标 / 追踪要逐个模块对接）

`FeedbackBus` 一次性解决这三个问题。

## FeedbackBus vs AuditLogger

| | FeedbackBus | AuditLogger |
|---|---|---|
| 定位 | 尽力而为的学习 / 观测信号 | 合规级审计链 |
| 投递 | **可丢** — 队列满丢最早 | **不丢** — 链式哈希 + 加密 + 持久化 |
| 典型消费者 | Trainer / Router / Optimizer / Prometheus | SIEM / 合规导出 / 司法取证 |
| 默认状态 | `enabled: false` | 始终启用 |

两者**互补不替代**。一条事件可同时发到两个。

## 事件模型

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
    std::string topic;       // 稳定 topic 字符串（如 "guard.feedback"）
    std::string request_id;  // 可选关联 id
    std::string tenant_id;
    std::string source;      // 谁发布的
    std::chrono::system_clock::time_point timestamp;
    nlohmann::json payload;  // 模块自定义载荷
};
```

`topicOf(T)` ↔ `typeOf(s)` 是**稳定公共映射**。新增 topic 算 MINOR；重命名/删除算 MAJOR。

## 启用

```yaml
# config/aegisgate.yaml
autonomy:
  feedback_bus:
    enabled: true
    max_queue_size: 10000        # 满时丢最早
    drop_policy: oldest          # v0 仅支持 "oldest"
```

网关暴露进程单例：

```cpp
auto& bus = aegisgate::FeedbackBus::instance();
```

启动装配（通常在 `GatewayRuntime::initialize` 中）：

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

## 发布事件

```cpp
#include "aegisgate/feedback_event.h"
#include "observe/feedback_bus.h"

aegisgate::FeedbackEvent ev;
ev.type       = aegisgate::FeedbackEventType::RouterOutcome;
ev.topic      = aegisgate::FeedbackEvent::topicOf(ev.type);
ev.request_id = ctx.request_id;
ev.tenant_id  = ctx.tenant_id;
ev.source     = "MLRouter";
ev.timestamp  = std::chrono::system_clock::now();
ev.payload    = {{"model", "gpt-4o"}, {"latency_ms", 1234}, {"success", true}};

aegisgate::FeedbackBus::instance().publish(std::move(ev));
```

`publish()` 是**非阻塞 O(1)**：入有界队列立刻返回。总线未启用或已停机时返回 `false`。

## 订阅

```cpp
auto& bus = aegisgate::FeedbackBus::instance();

// 订阅所有事件
auto id_all = bus.subscribe([](const aegisgate::FeedbackEvent& e) {
    // process...
});

// 订阅前缀 — guard.feedback / guard.anomaly 都会命中
auto id_guard = bus.subscribe([](const aegisgate::FeedbackEvent& e) {
    // process guard feedback
}, "guard.");

// 精确订阅
auto id_router_outcome = bus.subscribe([](const aegisgate::FeedbackEvent& e) {
    // process router outcome
}, "router.outcome");

// 常量时间取消
bus.unsubscribe(id_all);
```

订阅者在 dispatcher 线程中执行，**请勿阻塞**，重任务需 offload。单个订阅者异常会被隔离（try/catch + `spdlog::warn` + 计入 `delivery_errors`）。

## 内置 Metrics 订阅者

SDK 附带官方示例把总线桥接到 Prometheus：

```cpp
#include "observe/metrics_feedback_subscriber.h"

aegisgate::Counter events("feedback_events_total", "Feedback events by topic");
aegisgate::MetricsFeedbackSubscriber metrics_sub(events);
metrics_sub.attach(aegisgate::FeedbackBus::instance());

// 现在每条事件都会递增 feedback_events_total{type=<topic>}
```

## 生命周期

| 调用 | 行为 |
|------|------|
| `start()` | 启动 dispatcher（幂等） |
| `shutdown()` | 停止 dispatcher + **同步 drain** 剩余事件 |
| `flush(timeout)` | 阻塞等队列清空且 in-flight 投递完成 |
| `reconfigure(cfg)` | 动态更新 enabled / max_queue_size |

## 观测

`FeedbackBus::stats()` 返回：

```
published            总发布事件数
delivered            成功投递次数
dropped_queue_full   队列满丢弃数
delivery_errors      订阅者异常次数
queue_size           当前队列深度
subscriber_count     当前订阅者数量
```

通过 `Prometheus`（如 `feedback_bus_events_total` / `feedback_bus_drops_total` / `feedback_bus_subscriber_errors_total`）暴露。

## Runtime 装配（Phase 11.0）

当 `autonomy.feedback_bus.enabled=true` 时，`GatewayRuntime::initialize()` 会自动接入总线：

1. 用 `Config::feedbackBus*()` getter 配置 `FeedbackBus::instance()`
2. 调 `bus.start()` 启动 dispatcher 线程
3. 惰性创建进程内 `MetricsFeedbackSubscriber` 并 attach — 每条事件都递增 `feedback_events_total{type=<topic>}`
4. `GatewayRuntime::shutdown()` 时先 `flush()`（≤ 2 s）再 `shutdown()` 总线，**早于** audit logger 和 persistent store 关闭，确保订阅者发起的写操作还有可用依赖

效果：生产代码不需手动 `bus.start()`；只要开启 YAML 开关，Prometheus 计数器就开始填充。

## 路线图

| Phase | 项目 |
|-------|------|
| ✅ 11.0.1 | FeedbackBus + 事件 schema + metrics 订阅者（本 PR） |
| ✅ 11.0.2 | Runtime 装配（GatewayRuntime + Prometheus）— 本节 |
| 11.1.1 | `POST /admin/api/guard/feedback` → 发布 `GuardFeedback` |
| 11.1.2 | Online trainer 订阅 `guard.*` 构建数据集 |
| 11.2.1 | `MultiObjectiveRouter` 订阅 `router.outcome` 实时更新 |
| 11.4.2 | `AutoRollbackController` 订阅 `ops.incident` + `quality.drift` |
| 11.5.1 | `CostOptimizer 2.0` 订阅 `cost.observation` + `quality.feedback` |

## 相关文档

- 设计：`docs/specs/2026-04-17-phase11.0-feedback-bus-design.md`
- 锁分层：`docs/LOCK_ORDERING.md`（Layer 3）
- 公共头：`include/aegisgate/feedback_event.h`
- 路线图：`docs/ROADMAP.md`
