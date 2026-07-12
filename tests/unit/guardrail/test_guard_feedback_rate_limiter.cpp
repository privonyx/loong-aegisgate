// Phase 11.1 TASK-20260523-01 — Epic 2.3 GuardFeedbackRateLimiter tests.
//
// D4=C "投毒防御" rate-limit layer that sits in front of GuardFeedbackSink.
// Two independent limits:
//   * per-tenant     (default 100/min)
//   * per-reviewer   (default 30/min)
//   * global         (default 10000/min) — circuit-breaker
//
// SR-NEW2 ensures the per-tenant + per-reviewer + global cap rejects bursts
// that would otherwise poison the trainer.

#include "guardrail/feedback/guard_feedback_rate_limiter.h"

#include <gtest/gtest.h>

using aegisgate::guard::GuardFeedbackRateLimiter;
using aegisgate::guard::GuardFeedbackRateLimitConfig;

TEST(GuardFeedbackRateLimiterTest, AllowsUnderTenantQuota) {
    GuardFeedbackRateLimitConfig cfg;
    cfg.per_tenant_per_min = 5;
    cfg.per_reviewer_per_min = 5;
    cfg.global_per_min = 1000;
    GuardFeedbackRateLimiter rl(cfg);

    for (int i = 0; i < 5; ++i) {
        auto r = rl.checkAndConsume("tenant-A", "user-1");
        EXPECT_TRUE(r.allowed) << "burst " << i;
    }
}

TEST(GuardFeedbackRateLimiterTest, RejectsAboveTenantQuota) {
    GuardFeedbackRateLimitConfig cfg;
    cfg.per_tenant_per_min = 3;
    cfg.per_reviewer_per_min = 100;
    cfg.global_per_min = 100;
    GuardFeedbackRateLimiter rl(cfg);

    EXPECT_TRUE(rl.checkAndConsume("tenant-A", "user-1").allowed);
    EXPECT_TRUE(rl.checkAndConsume("tenant-A", "user-2").allowed);
    EXPECT_TRUE(rl.checkAndConsume("tenant-A", "user-3").allowed);
    auto over = rl.checkAndConsume("tenant-A", "user-4");
    EXPECT_FALSE(over.allowed);
    EXPECT_EQ(over.reject_reason, "tenant_quota_exceeded");
}

TEST(GuardFeedbackRateLimiterTest, RejectsAboveReviewerQuota) {
    GuardFeedbackRateLimitConfig cfg;
    cfg.per_tenant_per_min = 100;
    cfg.per_reviewer_per_min = 2;
    cfg.global_per_min = 100;
    GuardFeedbackRateLimiter rl(cfg);

    EXPECT_TRUE(rl.checkAndConsume("tenant-A", "user-1").allowed);
    EXPECT_TRUE(rl.checkAndConsume("tenant-A", "user-1").allowed);
    auto over = rl.checkAndConsume("tenant-A", "user-1");
    EXPECT_FALSE(over.allowed);
    EXPECT_EQ(over.reject_reason, "reviewer_quota_exceeded");
}

TEST(GuardFeedbackRateLimiterTest, RejectsAboveGlobalQuota) {
    GuardFeedbackRateLimitConfig cfg;
    cfg.per_tenant_per_min = 100;
    cfg.per_reviewer_per_min = 100;
    cfg.global_per_min = 4;
    GuardFeedbackRateLimiter rl(cfg);

    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(rl.checkAndConsume("tenant-" + std::to_string(i),
                                        "u" + std::to_string(i)).allowed);
    }
    auto over = rl.checkAndConsume("tenant-9", "user-9");
    EXPECT_FALSE(over.allowed);
    EXPECT_EQ(over.reject_reason, "global_quota_exceeded");
}

TEST(GuardFeedbackRateLimiterTest, IndependentTenantsDoNotShareCounters) {
    GuardFeedbackRateLimitConfig cfg;
    cfg.per_tenant_per_min = 2;
    cfg.per_reviewer_per_min = 10;
    cfg.global_per_min = 100;
    GuardFeedbackRateLimiter rl(cfg);

    EXPECT_TRUE(rl.checkAndConsume("tenant-A", "u1").allowed);
    EXPECT_TRUE(rl.checkAndConsume("tenant-A", "u1").allowed);
    EXPECT_FALSE(rl.checkAndConsume("tenant-A", "u1").allowed);

    // tenant-B has its own bucket — still allowed.
    EXPECT_TRUE(rl.checkAndConsume("tenant-B", "u1").allowed);
}
