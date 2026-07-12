#pragma once

// Phase 11.1 TASK-20260523-01 — Epic R2.3 GuardFeedbackAnomalyDetector.
//
// SR-NEW1 third layer (D4=C "+ anomaly detector"). The rate limiter (layer 2)
// only caps raw event count; this detector cares about *content distribution*:
// a single reviewer flagging >N false_positive within a rolling window is a
// strong poisoning signal even when they are under quota. We keep the model
// deliberately simple (count-by-label sliding window) so the false-positive
// rate of the detector itself is tunable per deployment.
//
// Production wiring sits between the rate limiter and the sink in
// GuardAdminController::postFeedback. A flagged feedback DOES NOT enter the
// sink; instead the controller records an `feedback_anomaly_flag` audit row
// and returns HTTP 202 (or 4xx, deployment-dependent) so trainer corpora stay
// clean.
//
// Thread-safe via std::mutex.

#include "guardrail/feedback/guard_feedback_payload.h"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace aegisgate::guard {

struct GuardFeedbackAnomalyConfig {
    // Maximum number of false_positive labels from a single reviewer within the
    // window before they are flagged. Default is sized to allow legitimate
    // bulk-correction by a senior reviewer while still catching obvious abuse.
    std::size_t reviewer_fp_threshold = 50;
    std::chrono::seconds window{std::chrono::hours{1}};
};

struct GuardFeedbackAnomalyDecision {
    bool is_anomalous = false;
    std::string reason;     // e.g. "reviewer_fp_burst"
    std::size_t observed = 0;
    std::size_t threshold = 0;
};

class GuardFeedbackAnomalyDetector {
public:
    explicit GuardFeedbackAnomalyDetector(GuardFeedbackAnomalyConfig cfg = {});

    // Inspect and record a feedback event. The decision is computed AFTER
    // recording so callers see the count that includes the current event.
    GuardFeedbackAnomalyDecision inspect(const std::string& reviewer_user_id,
                                          GuardFeedbackLabel label);

    const GuardFeedbackAnomalyConfig& config() const { return cfg_; }

private:
    struct ReviewerWindow {
        std::chrono::steady_clock::time_point window_start;
        std::size_t fp_count = 0;
    };

    void rollWindow(ReviewerWindow& w,
                    std::chrono::steady_clock::time_point now) const;

    GuardFeedbackAnomalyConfig cfg_;
    std::mutex mu_;
    std::unordered_map<std::string, ReviewerWindow> reviewers_;
};

}  // namespace aegisgate::guard
