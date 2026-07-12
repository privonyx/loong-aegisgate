#include "guardrail/feedback/guard_feedback_anomaly_detector.h"

#include <gtest/gtest.h>

namespace aegisgate::guard {
namespace {

class GuardFeedbackAnomalyDetectorTest : public ::testing::Test {};

TEST_F(GuardFeedbackAnomalyDetectorTest, AllowsTrafficUnderThreshold) {
    GuardFeedbackAnomalyConfig cfg;
    cfg.reviewer_fp_threshold = 5;
    GuardFeedbackAnomalyDetector det(cfg);
    for (int i = 0; i < 5; ++i) {
        auto d = det.inspect("reviewer_a", GuardFeedbackLabel::FalsePositive);
        EXPECT_FALSE(d.is_anomalous) << "iteration " << i;
    }
}

TEST_F(GuardFeedbackAnomalyDetectorTest, FlagsReviewerOnceFpBurstExceedsThreshold) {
    GuardFeedbackAnomalyConfig cfg;
    cfg.reviewer_fp_threshold = 3;
    GuardFeedbackAnomalyDetector det(cfg);
    for (int i = 0; i < 3; ++i) {
        ASSERT_FALSE(det.inspect("reviewer_a", GuardFeedbackLabel::FalsePositive)
                         .is_anomalous);
    }
    auto flagged = det.inspect("reviewer_a", GuardFeedbackLabel::FalsePositive);
    EXPECT_TRUE(flagged.is_anomalous);
    EXPECT_EQ(flagged.reason, "reviewer_fp_burst");
    EXPECT_EQ(flagged.threshold, 3u);
    EXPECT_GE(flagged.observed, 4u);
}

TEST_F(GuardFeedbackAnomalyDetectorTest, NonFalsePositiveLabelsDoNotIncrement) {
    GuardFeedbackAnomalyConfig cfg;
    cfg.reviewer_fp_threshold = 2;
    GuardFeedbackAnomalyDetector det(cfg);
    // 5 false_negatives must not trip the false-positive burst detector.
    for (int i = 0; i < 5; ++i) {
        auto d = det.inspect("reviewer_a", GuardFeedbackLabel::FalseNegative);
        EXPECT_FALSE(d.is_anomalous);
    }
}

TEST_F(GuardFeedbackAnomalyDetectorTest, IndependentReviewerCounters) {
    GuardFeedbackAnomalyConfig cfg;
    cfg.reviewer_fp_threshold = 2;
    GuardFeedbackAnomalyDetector det(cfg);
    // Reviewer A trips, reviewer B stays clean.
    det.inspect("reviewer_a", GuardFeedbackLabel::FalsePositive);
    det.inspect("reviewer_a", GuardFeedbackLabel::FalsePositive);
    auto a_trip = det.inspect("reviewer_a", GuardFeedbackLabel::FalsePositive);
    auto b_clean = det.inspect("reviewer_b", GuardFeedbackLabel::FalsePositive);
    EXPECT_TRUE(a_trip.is_anomalous);
    EXPECT_FALSE(b_clean.is_anomalous);
}

}  // namespace
}  // namespace aegisgate::guard
