#include "guardrail/feedback/guard_feedback_anomaly_detector.h"

namespace aegisgate::guard {

GuardFeedbackAnomalyDetector::GuardFeedbackAnomalyDetector(
    GuardFeedbackAnomalyConfig cfg)
    : cfg_(cfg) {}

void GuardFeedbackAnomalyDetector::rollWindow(
    ReviewerWindow& w, std::chrono::steady_clock::time_point now) const {
    if (w.window_start.time_since_epoch().count() == 0 ||
        now - w.window_start >= cfg_.window) {
        w.window_start = now;
        w.fp_count = 0;
    }
}

GuardFeedbackAnomalyDecision GuardFeedbackAnomalyDetector::inspect(
    const std::string& reviewer_user_id, GuardFeedbackLabel label) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mu_);
    auto& w = reviewers_[reviewer_user_id];
    rollWindow(w, now);
    if (label == GuardFeedbackLabel::FalsePositive) {
        ++w.fp_count;
    }
    GuardFeedbackAnomalyDecision d;
    d.threshold = cfg_.reviewer_fp_threshold;
    d.observed = w.fp_count;
    if (w.fp_count > cfg_.reviewer_fp_threshold) {
        d.is_anomalous = true;
        d.reason = "reviewer_fp_burst";
    }
    return d;
}

}  // namespace aegisgate::guard
