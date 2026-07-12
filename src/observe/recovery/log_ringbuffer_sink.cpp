// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 1 task 1.3.

#include "observe/recovery/log_ringbuffer_sink.h"

#include <spdlog/details/log_msg.h>

#include <chrono>

namespace aegisgate {

LogRingbufferSink::LogRingbufferSink(std::shared_ptr<PIIFilter> pii_filter,
                                       std::size_t capacity)
    : pii_filter_(std::move(pii_filter)),
      capacity_(capacity == 0 ? 1 : capacity) {}

void LogRingbufferSink::sink_it_(const spdlog::details::log_msg& msg) {
    Entry e;
    e.ts_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   msg.time.time_since_epoch())
                   .count();
    e.lvl    = msg.level;
    e.logger.assign(msg.logger_name.data(), msg.logger_name.size());

    // SR-NEW2: mask BEFORE enqueue. PIIFilter handles its own locking.
    std::string raw(msg.payload.data(), msg.payload.size());
    if (pii_filter_) {
        e.msg_masked = pii_filter_->mask(raw);
    } else {
        e.msg_masked = std::move(raw);
    }

    std::lock_guard<std::mutex> g(deque_mu_);
    if (entries_.size() >= capacity_) {
        entries_.pop_front();
    }
    entries_.emplace_back(std::move(e));
}

std::vector<LogRingbufferSink::Entry>
LogRingbufferSink::snapshotSince(std::int64_t since_ms) const {
    std::vector<Entry> out;
    std::lock_guard<std::mutex> g(deque_mu_);
    out.reserve(entries_.size());
    for (const auto& e : entries_) {
        if (e.ts_ms > since_ms) out.push_back(e);
    }
    return out;
}

std::vector<LogRingbufferSink::Entry>
LogRingbufferSink::dumpAll() const {
    std::lock_guard<std::mutex> g(deque_mu_);
    return {entries_.begin(), entries_.end()};
}

std::size_t LogRingbufferSink::size() const {
    std::lock_guard<std::mutex> g(deque_mu_);
    return entries_.size();
}

} // namespace aegisgate
