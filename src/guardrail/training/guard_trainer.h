#pragma once

// Phase 11.1 TASK-20260523-01 — Epic 3 GuardTrainer (collector).
//
// D2=A decision: the in-process component is a feedback collector that
// buffers canonical-form training rows and flushes to JSONL for an offline
// trainer sidecar to consume. The actual model fit lives outside this
// codebase (v2 留作业 in plan §6).
//
// SR2 reuse: comments are masked again before they hit disk so a misbehaving
// upstream cannot silently leak PII into the training corpus.

#include "guardrail/feedback/guard_feedback_payload.h"

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace aegisgate::guard {

// Canonical feature row produced from a feedback payload. Schema is stable;
// reading code MUST tolerate unknown extra keys.
using TrainingFeatures = std::map<std::string, std::string>;

TrainingFeatures extractTrainingFeatures(const GuardFeedbackPayload& payload);

// Strip / re-mask any obvious PII patterns (email at minimum) even if the
// upstream pipeline already redacted. Defense-in-depth for SR2.
std::string sanitizeFreeText(std::string in);

class GuardTrainer {
public:
    GuardTrainer() = default;
    explicit GuardTrainer(std::size_t max_buffer_rows);

    void captureFromPayload(const GuardFeedbackPayload& payload,
                             const std::string& tenant_id);

    std::size_t bufferedRows() const;

    // Append one JSON-line per buffered row to `out_path`. Returns false
    // when the path is unwritable.
    bool snapshotJsonl(const std::string& out_path) const;

    // Discard buffered rows (e.g. after a successful snapshot upload).
    void clear();

private:
    mutable std::mutex mu_;
    std::size_t max_buffer_rows_ = 100000;
    std::vector<TrainingFeatures> rows_;
};

}  // namespace aegisgate::guard
