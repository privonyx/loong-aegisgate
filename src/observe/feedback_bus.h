#pragma once
#include "aegisgate/feedback_event.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace aegisgate {

struct FeedbackBusConfig {
    bool enabled = false;
    size_t max_queue_size = 10000;
    std::string drop_policy = "oldest";  // v0 only supports "oldest"
};

struct FeedbackBusStats {
    uint64_t published = 0;
    uint64_t delivered = 0;
    uint64_t dropped_queue_full = 0;
    uint64_t delivery_errors = 0;
    size_t queue_size = 0;
    size_t subscriber_count = 0;
};

// Process-internal publish-subscribe event bus for autonomy-related
// feedback signals. Unlike AuditLogger (which is a compliance-grade,
// durable channel), FeedbackBus is lossy-by-design: queue overflow
// drops the oldest event and increments a counter. This matches the
// needs of online learning / adaptive systems that tolerate best-effort
// delivery.
class FeedbackBus {
public:
    using Subscriber = std::function<void(const FeedbackEvent&)>;

    FeedbackBus();
    explicit FeedbackBus(FeedbackBusConfig cfg);
    ~FeedbackBus();

    FeedbackBus(const FeedbackBus&) = delete;
    FeedbackBus& operator=(const FeedbackBus&) = delete;

    // Starts the background dispatcher thread. Idempotent; a second call
    // returns immediately. Not required for publish() to enqueue events
    // (events wait in the queue until a dispatcher is available).
    void start();

    // Stops the dispatcher and synchronously drains any remaining events.
    // After shutdown(), publish() returns false.
    void shutdown();

    // Non-blocking O(1) enqueue. Returns:
    //   false — bus is disabled or stopped
    //   true  — event is in the queue (it may still be dropped later
    //           if the queue becomes full before the dispatcher drains)
    bool publish(FeedbackEvent event);

    // Register a subscriber. topic_filter="" matches all topics; otherwise
    // the filter is treated as a prefix (e.g. "guard." matches
    // "guard.feedback" and "guard.anomaly"). Returns an opaque id suitable
    // for unsubscribe().
    size_t subscribe(Subscriber callback, std::string topic_filter = {});
    void unsubscribe(size_t id);

    // Blocks until the queue is drained and every in-flight event has
    // been delivered (or dropped). Useful for tests and graceful drain.
    // Returns true on full drain, false on timeout.
    bool flush(std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

    FeedbackBusStats stats() const;

    // Dynamically update configuration. Safe to call from any thread.
    // If the new config disables the bus, in-flight events are still
    // delivered but future publish() calls are rejected.
    void reconfigure(FeedbackBusConfig cfg);

    // Process-wide singleton used by the HTTP layer and runtime wiring.
    static FeedbackBus& instance();

private:
    void dispatcherLoop();
    void deliver(const FeedbackEvent& event);
    bool matchesFilter(const std::string& topic,
                       const std::string& filter) const;

    FeedbackBusConfig config_;

    mutable std::mutex mutex_;  // Lock Layer 3 — protects subscribers_
    struct SubscriberEntry {
        Subscriber cb;
        std::string topic_filter;
    };
    std::unordered_map<size_t, SubscriberEntry> subscribers_;
    std::atomic<size_t> next_id_{1};

    // Internal queue lock — never nested with mutex_.
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<FeedbackEvent> queue_;
    std::atomic<uint64_t> in_flight_{0};  // events currently being dispatched
    std::condition_variable flush_cv_;

    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};
    std::thread dispatcher_;

    std::atomic<uint64_t> published_{0};
    std::atomic<uint64_t> delivered_{0};
    std::atomic<uint64_t> dropped_queue_full_{0};
    std::atomic<uint64_t> delivery_errors_{0};
};

} // namespace aegisgate
