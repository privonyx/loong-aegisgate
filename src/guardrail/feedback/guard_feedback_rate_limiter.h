#pragma once

// Phase 11.1 TASK-20260523-01 — Epic 2.3 GuardFeedbackRateLimiter.
//
// D4=C anti-poisoning layer in front of GuardFeedbackSink. Enforces three
// independent token buckets sized in events-per-minute:
//   * per-tenant   — caps one customer flooding the trainer
//   * per-reviewer — caps one compromised account flooding the trainer
//   * global       — circuit-breaker on aggregate spam
//
// Quotas use a simple sliding-window counter (one minute) because the
// existing gateway RateLimiter is concerned with HTTP routes; this one is
// at the feedback domain and the simpler model keeps SR-NEW2 reasoning
// explicit. Thread-safe via std::mutex.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace aegisgate::guard {

struct GuardFeedbackRateLimitConfig {
    std::size_t per_tenant_per_min = 100;
    std::size_t per_reviewer_per_min = 30;
    std::size_t global_per_min = 10000;
};

struct GuardFeedbackRateLimitDecision {
    bool allowed = false;
    std::string reject_reason;  // tenant_/reviewer_/global_quota_exceeded
};

class GuardFeedbackRateLimiter {
public:
    explicit GuardFeedbackRateLimiter(GuardFeedbackRateLimitConfig cfg);

    GuardFeedbackRateLimitDecision checkAndConsume(
        const std::string& tenant_id, const std::string& reviewer_user_id);

private:
    struct WindowedCounter {
        std::chrono::steady_clock::time_point window_start;
        std::size_t count = 0;
    };

    static constexpr std::chrono::seconds kWindow{60};

    void rollWindow(WindowedCounter& c,
                    std::chrono::steady_clock::time_point now) const;

    GuardFeedbackRateLimitConfig cfg_;
    std::mutex mu_;
    std::unordered_map<std::string, WindowedCounter> per_tenant_;
    std::unordered_map<std::string, WindowedCounter> per_reviewer_;
    WindowedCounter global_;
};

}  // namespace aegisgate::guard
