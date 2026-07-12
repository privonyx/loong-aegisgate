#include "guardrail/feedback/guard_feedback_rate_limiter.h"

namespace aegisgate::guard {

GuardFeedbackRateLimiter::GuardFeedbackRateLimiter(
    GuardFeedbackRateLimitConfig cfg)
    : cfg_(cfg) {}

void GuardFeedbackRateLimiter::rollWindow(
    WindowedCounter& c, std::chrono::steady_clock::time_point now) const {
    if (now - c.window_start >= kWindow) {
        c.window_start = now;
        c.count = 0;
    }
}

GuardFeedbackRateLimitDecision GuardFeedbackRateLimiter::checkAndConsume(
    const std::string& tenant_id, const std::string& reviewer_user_id) {
    std::lock_guard lock(mu_);
    auto now = std::chrono::steady_clock::now();

    // Global cap first — if we are saturated, refuse even before bucketing.
    rollWindow(global_, now);
    if (global_.count >= cfg_.global_per_min) {
        return {false, "global_quota_exceeded"};
    }

    auto& tenant_bucket = per_tenant_[tenant_id];
    if (tenant_bucket.window_start.time_since_epoch().count() == 0) {
        tenant_bucket.window_start = now;
    }
    rollWindow(tenant_bucket, now);
    if (tenant_bucket.count >= cfg_.per_tenant_per_min) {
        return {false, "tenant_quota_exceeded"};
    }

    auto& reviewer_bucket = per_reviewer_[reviewer_user_id];
    if (reviewer_bucket.window_start.time_since_epoch().count() == 0) {
        reviewer_bucket.window_start = now;
    }
    rollWindow(reviewer_bucket, now);
    if (reviewer_bucket.count >= cfg_.per_reviewer_per_min) {
        return {false, "reviewer_quota_exceeded"};
    }

    ++global_.count;
    ++tenant_bucket.count;
    ++reviewer_bucket.count;
    return {true, {}};
}

}  // namespace aegisgate::guard
