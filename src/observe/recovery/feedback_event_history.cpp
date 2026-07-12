// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 1 task 1.4.

#include "observe/recovery/feedback_event_history.h"

#include <chrono>

namespace aegisgate {

FeedbackEventHistory::FeedbackEventHistory(FeedbackBus& bus,
                                             std::size_t capacity)
    : bus_(&bus), capacity_(capacity == 0 ? 1 : capacity) {
    subscription_id_ = bus_->subscribe(
        [this](const FeedbackEvent& ev) { onEvent(ev); },
        /*topic_filter=*/"");
}

FeedbackEventHistory::~FeedbackEventHistory() {
    if (bus_ && subscription_id_ != 0) {
        bus_->unsubscribe(subscription_id_);
    }
}

void FeedbackEventHistory::onEvent(const FeedbackEvent& ev) {
    std::lock_guard<std::mutex> g(mu_);
    if (entries_.size() >= capacity_) {
        entries_.pop_front();
    }
    entries_.push_back(ev);
}

std::vector<FeedbackEvent>
FeedbackEventHistory::snapshotSince(std::int64_t since_ms,
                                      const std::string& topic_filter) const {
    std::vector<FeedbackEvent> out;
    std::lock_guard<std::mutex> g(mu_);
    out.reserve(entries_.size());
    for (const auto& e : entries_) {
        const auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               e.timestamp.time_since_epoch())
                               .count();
        if (ts_ms <= since_ms) continue;
        if (!topic_filter.empty() && e.topic != topic_filter) continue;
        out.push_back(e);
    }
    return out;
}

std::vector<FeedbackEvent>
FeedbackEventHistory::snapshotByType(FeedbackEventType type,
                                       std::size_t limit) const {
    std::vector<FeedbackEvent> out;
    std::lock_guard<std::mutex> g(mu_);
    out.reserve(entries_.size());
    for (const auto& e : entries_) {
        if (e.type == type) out.push_back(e);
    }
    if (limit > 0 && out.size() > limit) {
        out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(limit));
    }
    return out;
}

std::size_t FeedbackEventHistory::size() const {
    std::lock_guard<std::mutex> g(mu_);
    return entries_.size();
}

} // namespace aegisgate
