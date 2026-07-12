#include <gtest/gtest.h>
#include "guardrail/inbound/guard_decision.h"
#include <cmath>

using namespace aegisgate::guard_detail;

// --- softmax ---

TEST(GuardDecisionTest, SoftmaxSumsToOne) {
    auto p = softmax({2.0f, 1.0f, 0.1f});
    float sum = 0.0f;
    for (float v : p) sum += v;
    EXPECT_NEAR(sum, 1.0f, 1e-5f);
    // monotonic: larger logit -> larger prob
    EXPECT_GT(p[0], p[1]);
    EXPECT_GT(p[1], p[2]);
}

TEST(GuardDecisionTest, SoftmaxNumericStability) {
    // Large logits must not overflow (subtract-max stabilization).
    auto p = softmax({1000.0f, 999.0f});
    float sum = 0.0f;
    for (float v : p) { EXPECT_FALSE(std::isnan(v)); EXPECT_FALSE(std::isinf(v)); sum += v; }
    EXPECT_NEAR(sum, 1.0f, 1e-5f);
    EXPECT_GT(p[0], p[1]);
}

TEST(GuardDecisionTest, SoftmaxEmptyReturnsEmpty) {
    auto p = softmax({});
    EXPECT_TRUE(p.empty());
}

// --- evaluate: binary [safe=0, injection=1] ---

TEST(GuardDecisionTest, UnsafeWhenInjectionAboveThreshold) {
    // logits favor injection (index 1); P(injection) ~ 0.88 >= 0.5
    auto d = evaluate({0.0f, 2.0f}, /*safe_index=*/0, /*threshold=*/0.5f);
    EXPECT_TRUE(d.unsafe);
    EXPECT_EQ(d.label_index, 1u);
    EXPECT_GE(d.score, 0.5f);
}

TEST(GuardDecisionTest, SafeWhenSafeDominant) {
    auto d = evaluate({3.0f, 0.0f}, 0, 0.5f);
    EXPECT_FALSE(d.unsafe);
    EXPECT_EQ(d.label_index, 0u);
}

TEST(GuardDecisionTest, SafeWhenInjectionBelowThreshold) {
    // P(injection) ~ 0.62; with a high threshold 0.8 -> still considered safe
    auto d = evaluate({0.0f, 0.5f}, 0, 0.8f);
    EXPECT_FALSE(d.unsafe);
}

TEST(GuardDecisionTest, ThresholdSensitivityLowerIsMoreAggressive) {
    std::vector<float> logits = {0.0f, 0.5f};  // P(injection) ~ 0.62
    EXPECT_TRUE(evaluate(logits, 0, 0.3f).unsafe);   // low threshold -> flag
    EXPECT_FALSE(evaluate(logits, 0, 0.8f).unsafe);  // high threshold -> pass
}

// --- evaluate: multi-class (safe + several unsafe labels) ---

TEST(GuardDecisionTest, MultiClassPicksHighestUnsafe) {
    // [safe, violence, hate]; hate dominates
    auto d = evaluate({0.0f, 1.0f, 3.0f}, 0, 0.5f);
    EXPECT_TRUE(d.unsafe);
    EXPECT_EQ(d.label_index, 2u);
}
