#pragma once

// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 1 task 1.4.
//
// FeedbackEventHistory — bounded FIFO buffer that subscribes to a
// FeedbackBus and lets the RootCauseAnalyzer / RunbookEngine query recent
// events by type and time. Owns its own subscription handle and detaches
// it in the destructor (RAII, matches AuditLogger subscribe / unsubscribe
// pattern).
//
// Why not just expose FeedbackBus to RCA directly? Because we want a
// process-wide ring of N most-recent events that survives between RCA
// passes; FeedbackBus itself does not retain delivered events.

#include "aegisgate/feedback_event.h"
#include "observe/feedback_bus.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace aegisgate {

class FeedbackEventHistory {
public:
    FeedbackEventHistory(FeedbackBus& bus, std::size_t capacity);
    ~FeedbackEventHistory();

    FeedbackEventHistory(const FeedbackEventHistory&) = delete;
    FeedbackEventHistory& operator=(const FeedbackEventHistory&) = delete;

    // Snapshot events newer than `since_ms` (wall-clock ms), optionally
    // filtered by exact topic match (empty = any topic).
    std::vector<FeedbackEvent> snapshotSince(
        std::int64_t since_ms,
        const std::string& topic_filter = std::string()) const;

    // Snapshot events of a specific type, returning the most recent
    // up-to-N entries in insertion order (oldest first).
    std::vector<FeedbackEvent> snapshotByType(FeedbackEventType type,
                                                std::size_t limit) const;

    std::size_t size() const;

private:
    void onEvent(const FeedbackEvent& ev);

    FeedbackBus*               bus_;
    std::size_t                subscription_id_ = 0;
    std::size_t                capacity_;
    mutable std::mutex         mu_;             // Layer 3
    std::deque<FeedbackEvent>  entries_;
};

} // namespace aegisgate
