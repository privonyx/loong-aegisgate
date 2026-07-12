#pragma once

// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic B.2.
//
// CR1 creative design — naive ring buffer + per-query sort:
//   memory-bank/creative/creative-rollout-metrics-algorithm.md
//
// Design summary:
//   - Per-version std::deque<Sample> holding {ts_ms, latency_ms, is_error}.
//   - Append path: O(1). Oldest sample is pop_front()'d when the cap is hit.
//   - Query path: reverse-scan window filter (exit early on time-monotone
//     data) → copy latencies → std::sort → pick index ⌊n * 0.99⌋ for p99.
//   - Error rate is a precise count of is_error over samples in the window.
//
// SR15 (noexcept subscriber): the FeedbackBus callback body is wrapped in
// try/catch(...) so a malformed payload can never escape to the dispatcher.
//
// Thread-safety:
//   - versions_ map is guarded by a shared_mutex (shared on hit, unique
//     only on the first-sample-per-version path).
//   - Each Ring owns its own std::mutex; query snapshots copy the small
//     latency vector under the lock then sort outside the critical section.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace aegisgate {

class FeedbackBus;
struct FeedbackEvent;

// Aggregated view returned to RolloutController.
struct VersionMetrics {
    std::int64_t sample_count = 0;
    double       error_rate = 0.0;      // [0.0, 1.0]
    double       p99_latency_ms = 0.0;
};

// Abstract dependency boundary so RolloutController can be unit-tested
// against a FakeMetricsProvider without standing up a FeedbackBus.
class RolloutMetricsProvider {
public:
    virtual ~RolloutMetricsProvider() = default;
    virtual VersionMetrics forVersion(std::string_view version_id,
                                       std::chrono::seconds window) = 0;
};

// Production implementation backed by FeedbackBus RouterOutcome events.
//
// The class also supports a bus-less constructor for unit tests: in that
// mode recordRouterOutcome() is called directly and no subscription is
// installed. This matches the creative-doc testing strategy of
// "ground-truth comparison" without requiring a background dispatcher.
class FeedbackBusMetricsProvider : public RolloutMetricsProvider {
public:
    static constexpr std::size_t kDefaultMaxSamples = 50'000;

    // Bus-less constructor (tests).
    explicit FeedbackBusMetricsProvider(
        std::size_t max_samples_per_version = kDefaultMaxSamples);

    // Production constructor. RAII-subscribes on construction; the
    // destructor unsubscribes.
    FeedbackBusMetricsProvider(
        FeedbackBus& bus,
        std::size_t max_samples_per_version = kDefaultMaxSamples);

    ~FeedbackBusMetricsProvider() override;

    FeedbackBusMetricsProvider(const FeedbackBusMetricsProvider&) = delete;
    FeedbackBusMetricsProvider& operator=(const FeedbackBusMetricsProvider&) = delete;

    VersionMetrics forVersion(std::string_view version_id,
                               std::chrono::seconds window) override;

    // Deterministic variant — tests inject `now_ms` rather than relying
    // on wall-clock. Both overloads share the same snapshot path.
    VersionMetrics forVersionAt(std::string_view version_id,
                                 std::chrono::seconds window,
                                 std::int64_t now_ms);

    // Direct sample injection. Used by the bus callback and by tests.
    // Strictly O(1) on the hot path (push_back + conditional pop_front).
    void recordRouterOutcome(std::string_view version_id,
                              std::int64_t ts_ms,
                              double latency_ms,
                              bool is_error);

private:
    struct Sample {
        std::int64_t ts_ms;
        double       latency_ms;
        bool         is_error;
    };

    struct Ring {
        std::deque<Sample> samples;
        std::mutex         mu;
    };

    Ring* getOrCreateRing(std::string_view version_id);
    Ring* findRing(std::string_view version_id);

    // SR15: invoked from the FeedbackBus dispatcher thread. noexcept.
    void onFeedbackEvent(const FeedbackEvent& ev) noexcept;

    mutable std::shared_mutex map_mu_;
    std::unordered_map<std::string, std::unique_ptr<Ring>> versions_;
    std::size_t  max_samples_;
    FeedbackBus* bus_{nullptr};
    std::size_t  subscription_id_{0};
};

} // namespace aegisgate
