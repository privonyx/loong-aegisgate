#pragma once
#include "observe/feedback_bus.h"
#include "observe/metrics.h"
#include <cstddef>

namespace aegisgate {

// Official example subscriber that bridges FeedbackBus events to Prometheus
// counters. Attaches with topic_filter="" (receives every event) and
// increments the given Counter labelled by topic. Intentionally minimal —
// acts as reference implementation for downstream Phase 11.x subscribers.
//
// RAII contract (Phase 11.0 runtime wiring): on destruction, the subscriber
// detaches from the bus it was attached to. This guarantees that going out
// of scope cannot leave the bus holding a dangling lambda capturing `this`.
class MetricsFeedbackSubscriber {
public:
    explicit MetricsFeedbackSubscriber(Counter& counter);
    ~MetricsFeedbackSubscriber();

    MetricsFeedbackSubscriber(const MetricsFeedbackSubscriber&) = delete;
    MetricsFeedbackSubscriber& operator=(const MetricsFeedbackSubscriber&) = delete;

    // Subscribes to the given bus. Returns the subscription id. Calling
    // attach() twice without an intervening detach() detaches first.
    size_t attach(FeedbackBus& bus);
    void detach(FeedbackBus& bus);

private:
    Counter& counter_;
    FeedbackBus* bus_ = nullptr;  // tracked for RAII detach
    size_t id_ = 0;
};

} // namespace aegisgate
