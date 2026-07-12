#pragma once

// Phase 9.3.4 Epic D.3 — RouterOutcome publisher.
//
// Publishes RouterOutcome events to FeedbackBus after each provider
// invocation. The RolloutMetricsProvider subscribes to these events
// to build sliding-window metrics for auto-pause / auto-rollback
// decisions.
//
// SR15: publish() is noexcept — failures are silently swallowed so
// the data-plane hot path is never disrupted by feedback subsystem
// issues.

#include "aegisgate/feedback_event.h"
#include "observe/feedback_bus.h"

#include <chrono>
#include <string>

namespace aegisgate {

class RouterOutcomePublisher {
public:
    explicit RouterOutcomePublisher(FeedbackBus& bus) : bus_(&bus) {}
    RouterOutcomePublisher() : bus_(nullptr) {}

    void publish(const std::string& request_id,
                 const std::string& tenant_id,
                 const std::string& version_id,
                 const std::string& region,
                 const std::string& provider,
                 const std::string& model,
                 double latency_ms,
                 const std::string& outcome) noexcept {
        if (bus_ == nullptr) return;
        try {
            FeedbackEvent ev;
            ev.type = FeedbackEventType::RouterOutcome;
            ev.topic = "router.outcome";
            ev.request_id = request_id;
            ev.tenant_id = tenant_id;
            ev.source = "api_controller";
            ev.timestamp = std::chrono::system_clock::now();
            ev.payload = {
                {"version_id", version_id},
                {"region",     region},
                {"provider",   provider},
                {"model",      model},
                {"latency_ms", latency_ms},
                {"outcome",    outcome},
            };
            bus_->publish(std::move(ev));
        } catch (...) {
            // SR15: swallow
        }
    }

private:
    FeedbackBus* bus_;
};

} // namespace aegisgate
