#include "observe/feedback_bus.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <utility>
#include <vector>

namespace aegisgate {

FeedbackBus::FeedbackBus() = default;

FeedbackBus::FeedbackBus(FeedbackBusConfig cfg) : config_(std::move(cfg)) {}

FeedbackBus::~FeedbackBus() {
    shutdown();
}

void FeedbackBus::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return;  // already running
    }
    stopping_.store(false);
    dispatcher_ = std::thread([this] { dispatcherLoop(); });
}

void FeedbackBus::shutdown() {
    if (!started_.exchange(false)) {
        return;  // was never started
    }
    stopping_.store(true);
    queue_cv_.notify_all();
    if (dispatcher_.joinable()) {
        dispatcher_.join();
    }
}

bool FeedbackBus::publish(FeedbackEvent event) {
    if (!config_.enabled) return false;
    if (stopping_.load()) return false;

    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        if (queue_.size() >= config_.max_queue_size) {
            // drop_policy "oldest" is the only v0 option.
            queue_.pop_front();
            dropped_queue_full_.fetch_add(1, std::memory_order_relaxed);
        }
        queue_.push_back(std::move(event));
    }
    published_.fetch_add(1, std::memory_order_relaxed);
    queue_cv_.notify_one();
    return true;
}

size_t FeedbackBus::subscribe(Subscriber callback, std::string topic_filter) {
    const size_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(mutex_);
    subscribers_.emplace(id, SubscriberEntry{std::move(callback),
                                              std::move(topic_filter)});
    return id;
}

void FeedbackBus::unsubscribe(size_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    subscribers_.erase(id);
}

bool FeedbackBus::flush(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lk(queue_mutex_);
    while (!queue_.empty() || in_flight_.load() > 0) {
        if (!flush_cv_.wait_until(lk, deadline, [this] {
                return queue_.empty() && in_flight_.load() == 0;
            })) {
            return false;  // timed out
        }
    }
    return true;
}

FeedbackBusStats FeedbackBus::stats() const {
    FeedbackBusStats s{};
    s.published           = published_.load(std::memory_order_relaxed);
    s.delivered           = delivered_.load(std::memory_order_relaxed);
    s.dropped_queue_full  = dropped_queue_full_.load(std::memory_order_relaxed);
    s.delivery_errors     = delivery_errors_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        s.queue_size = queue_.size();
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        s.subscriber_count = subscribers_.size();
    }
    return s;
}

void FeedbackBus::reconfigure(FeedbackBusConfig cfg) {
    std::lock_guard<std::mutex> lk(queue_mutex_);
    config_ = std::move(cfg);
    // Trim queue if new max is smaller.
    while (queue_.size() > config_.max_queue_size) {
        queue_.pop_front();
        dropped_queue_full_.fetch_add(1, std::memory_order_relaxed);
    }
}

FeedbackBus& FeedbackBus::instance() {
    static FeedbackBus bus;
    return bus;
}

void FeedbackBus::dispatcherLoop() {
    while (true) {
        std::vector<FeedbackEvent> batch;
        {
            std::unique_lock<std::mutex> lk(queue_mutex_);
            queue_cv_.wait(lk, [this] {
                return !queue_.empty() || stopping_.load();
            });
            while (!queue_.empty()) {
                batch.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
            // CRITICAL: bump in_flight_ BEFORE releasing queue_mutex_, so
            // that a concurrent flush() cannot observe the (queue empty +
            // in_flight == 0) state when events have actually been picked
            // up but not yet delivered. Without this, coverage-instrumented
            // builds (which widen the window between pop and fetch_add)
            // race flush() out with undelivered events.
            in_flight_.fetch_add(batch.size(), std::memory_order_relaxed);
        }

        for (const auto& ev : batch) {
            deliver(ev);
            in_flight_.fetch_sub(1, std::memory_order_relaxed);
        }
        {
            // Hold queue_mutex_ briefly while signalling flush_cv_ to avoid
            // a lost-wakeup where flush() evaluates its predicate right
            // between fetch_sub and notify_all.
            std::lock_guard<std::mutex> lk(queue_mutex_);
            (void)lk;
        }
        flush_cv_.notify_all();

        if (stopping_.load()) {
            std::unique_lock<std::mutex> lk(queue_mutex_);
            if (queue_.empty()) break;
        }
    }
}

void FeedbackBus::deliver(const FeedbackEvent& event) {
    // Snapshot subscribers under mutex_, then release before invoking callbacks
    // (lock-outside-I/O pattern — see docs/LOCK_ORDERING.md).
    std::vector<SubscriberEntry> snapshot;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        snapshot.reserve(subscribers_.size());
        for (const auto& [id, entry] : subscribers_) {
            (void)id;
            if (matchesFilter(event.topic, entry.topic_filter)) {
                snapshot.push_back(entry);
            }
        }
    }
    for (const auto& entry : snapshot) {
        try {
            entry.cb(event);
            delivered_.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& e) {
            delivery_errors_.fetch_add(1, std::memory_order_relaxed);
            spdlog::warn("FeedbackBus subscriber threw: {}", e.what());
        } catch (...) {
            delivery_errors_.fetch_add(1, std::memory_order_relaxed);
            spdlog::warn("FeedbackBus subscriber threw unknown exception");
        }
    }
}

bool FeedbackBus::matchesFilter(const std::string& topic,
                                 const std::string& filter) const {
    if (filter.empty()) return true;
    if (filter.size() > topic.size()) return false;
    return topic.compare(0, filter.size(), filter) == 0;
}

} // namespace aegisgate
