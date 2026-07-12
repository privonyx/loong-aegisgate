#pragma once

// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 1 task 1.3.
//
// LogRingbufferSink — bounded, FIFO in-memory spdlog sink that feeds
// RootCauseAnalyzer with recent log lines. Differences from spdlog's
// builtin ringbuffer_sink:
//
//   1. SR-NEW2: each entry is passed through PIIFilter::mask() BEFORE it
//      is enqueued. The raw bytes never sit in the buffer, even briefly.
//      (Defence-in-depth: RootCauseAnalyzer also masks again on output.)
//   2. Returns structured Entry instead of raw spdlog::log_msg, so RCA can
//      filter by level / logger name without coupling to spdlog internals.
//   3. Exposes snapshotSince(timestamp) so RCA can ask only for log lines
//      newer than its previous evaluation window.
//
// Lock layering: deque_mu_ is Layer 3 (same layer as FeedbackBus + the
// AuditLogger queue). PIIFilter::mask() is called WHILE deque_mu_ is held
// — PIIFilter uses its own Layer 1 patterns_mutex_ and never calls back
// into Layer 3.

#include "guardrail/inbound/pii_filter.h"

#include <spdlog/sinks/base_sink.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aegisgate {

class LogRingbufferSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    struct Entry {
        std::int64_t              ts_ms = 0;     // wall clock ms since epoch
        spdlog::level::level_enum lvl   = spdlog::level::info;
        std::string               logger;        // logger name (post-mask: none)
        std::string               msg_masked;    // payload AFTER PIIFilter
    };

    LogRingbufferSink(std::shared_ptr<PIIFilter> pii_filter,
                       std::size_t capacity);

    // Snapshot entries strictly newer than `since_ms` (wall-clock ms).
    std::vector<Entry> snapshotSince(std::int64_t since_ms) const;

    // Full snapshot. Used by /admin debug API (must be SuperAdmin-gated
    // at the HTTP layer per SR-NEW2 detection (c)).
    std::vector<Entry> dumpAll() const;

    std::size_t size() const;
    std::size_t capacity() const { return capacity_; }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override {}

private:
    std::shared_ptr<PIIFilter>     pii_filter_;
    std::size_t                    capacity_;
    mutable std::mutex             deque_mu_;   // Layer 3
    std::deque<Entry>              entries_;
};

} // namespace aegisgate
