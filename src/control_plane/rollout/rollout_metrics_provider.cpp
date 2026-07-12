#include "control_plane/rollout/rollout_metrics_provider.h"

#include "aegisgate/feedback_event.h"
#include "observe/feedback_bus.h"

#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>

namespace aegisgate {

FeedbackBusMetricsProvider::FeedbackBusMetricsProvider(std::size_t cap)
    : max_samples_(cap == 0 ? kDefaultMaxSamples : cap) {}

FeedbackBusMetricsProvider::FeedbackBusMetricsProvider(FeedbackBus& bus,
                                                       std::size_t cap)
    : max_samples_(cap == 0 ? kDefaultMaxSamples : cap), bus_(&bus) {
    subscription_id_ = bus_->subscribe(
        [this](const FeedbackEvent& ev) { onFeedbackEvent(ev); },
        /*topic_filter=*/"router.outcome");
}

FeedbackBusMetricsProvider::~FeedbackBusMetricsProvider() {
    if (bus_ != nullptr && subscription_id_ != 0) {
        bus_->unsubscribe(subscription_id_);
    }
}

FeedbackBusMetricsProvider::Ring*
FeedbackBusMetricsProvider::findRing(std::string_view version_id) {
    std::shared_lock lk(map_mu_);
    auto it = versions_.find(std::string(version_id));
    if (it == versions_.end()) return nullptr;
    return it->second.get();
}

FeedbackBusMetricsProvider::Ring*
FeedbackBusMetricsProvider::getOrCreateRing(std::string_view version_id) {
    {
        std::shared_lock lk(map_mu_);
        auto it = versions_.find(std::string(version_id));
        if (it != versions_.end()) return it->second.get();
    }
    std::unique_lock lk(map_mu_);
    auto& slot = versions_[std::string(version_id)];
    if (!slot) slot = std::make_unique<Ring>();
    return slot.get();
}

void FeedbackBusMetricsProvider::recordRouterOutcome(std::string_view version_id,
                                                     std::int64_t ts_ms,
                                                     double latency_ms,
                                                     bool is_error) {
    Ring* r = getOrCreateRing(version_id);
    std::lock_guard lk(r->mu);
    if (r->samples.size() >= max_samples_) {
        r->samples.pop_front();
    }
    r->samples.push_back(Sample{ts_ms, latency_ms, is_error});
}

void FeedbackBusMetricsProvider::onFeedbackEvent(const FeedbackEvent& ev) noexcept {
    // SR15: absorb any parsing / typing anomaly silently. A malformed
    // payload must never propagate to the bus dispatcher.
    try {
        if (ev.type != FeedbackEventType::RouterOutcome) return;
        const auto& p = ev.payload;
        if (!p.is_object()) return;

        auto vid_it = p.find("version_id");
        if (vid_it == p.end() || !vid_it->is_string()) return;
        std::string version_id = vid_it->get<std::string>();

        auto lat_it = p.find("latency_ms");
        if (lat_it == p.end() || !lat_it->is_number()) return;
        double latency_ms = lat_it->get<double>();

        bool is_error = false;
        auto err_it = p.find("is_error");
        if (err_it != p.end() && err_it->is_boolean()) {
            is_error = err_it->get<bool>();
        }

        std::int64_t ts_ms = 0;
        auto ts_it = p.find("ts_ms");
        if (ts_it != p.end() && ts_it->is_number_integer()) {
            ts_ms = ts_it->get<std::int64_t>();
        } else {
            ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        ev.timestamp.time_since_epoch())
                        .count();
        }

        recordRouterOutcome(version_id, ts_ms, latency_ms, is_error);
    } catch (...) {
        // Swallow per SR15.
    }
}

VersionMetrics FeedbackBusMetricsProvider::forVersion(std::string_view version_id,
                                                     std::chrono::seconds window) {
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    return forVersionAt(version_id, window, now_ms);
}

VersionMetrics FeedbackBusMetricsProvider::forVersionAt(std::string_view version_id,
                                                        std::chrono::seconds window,
                                                        std::int64_t now_ms) {
    VersionMetrics out;
    Ring* r = findRing(version_id);
    if (r == nullptr) return out;

    const std::int64_t window_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(window).count();
    const std::int64_t cutoff_ms = now_ms - window_ms;

    std::vector<double> latencies;
    std::int64_t errors = 0;
    std::int64_t total = 0;

    {
        std::lock_guard lk(r->mu);
        // Reverse iterate; time-monotone insertion lets us break early.
        for (auto it = r->samples.rbegin(); it != r->samples.rend(); ++it) {
            if (it->ts_ms < cutoff_ms) break;
            ++total;
            if (it->is_error) ++errors;
            latencies.push_back(it->latency_ms);
        }
    }

    if (total == 0) return out;

    std::sort(latencies.begin(), latencies.end());
    // Creative doc §5: idx = min(n-1, floor(n * 0.99)).
    const std::size_t n = latencies.size();
    std::size_t idx = static_cast<std::size_t>(double(n) * 0.99);
    if (idx >= n) idx = n - 1;

    out.sample_count   = total;
    out.error_rate     = double(errors) / double(total);
    out.p99_latency_ms = latencies[idx];
    return out;
}

} // namespace aegisgate
