#include <gtest/gtest.h>
#include "observe/anomaly_detector.h"
#include "observe/metrics.h"

using namespace aegisgate;

class AnomalyDetectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        AnomalyDetectorConfig cfg;
        cfg.z_score_threshold = 3.0;
        cfg.window_size = 100;
        cfg.enabled = true;
        detector_.setConfig(cfg);
        detector_.clear();
    }
    AnomalyDetector detector_;
};

TEST_F(AnomalyDetectorTest, NormalValues) {
    for (int i = 0; i < 50; i++) {
        detector_.recordMetric(AnomalyType::RateSpike, 100.0 + (i % 3));
    }
    auto events = detector_.recentEvents();
    EXPECT_TRUE(events.empty());
}

TEST_F(AnomalyDetectorTest, DetectSpike) {
    for (int i = 0; i < 50; i++) {
        detector_.recordMetric(AnomalyType::LatencySpike, 10.0);
    }
    detector_.recordMetric(AnomalyType::LatencySpike, 100.0);

    auto events = detector_.recentEvents();
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().type, AnomalyType::LatencySpike);
    EXPECT_GT(events.back().z_score, 3.0);
}

// P2-#5: detecting an anomaly must advance anomaly_events_total (was always 0).
TEST_F(AnomalyDetectorTest, DetectedAnomalyIncrementsMetric) {
    MetricsRegistry::instance().anomalyEventsTotal().reset();
    for (int i = 0; i < 50; i++) {
        detector_.recordMetric(AnomalyType::LatencySpike, 10.0);
    }
    EXPECT_DOUBLE_EQ(MetricsRegistry::instance().anomalyEventsTotal().get(), 0.0);
    detector_.recordMetric(AnomalyType::LatencySpike, 100.0);
    EXPECT_GT(MetricsRegistry::instance().anomalyEventsTotal().get(), 0.0);
}

TEST_F(AnomalyDetectorTest, WindowMaintenance) {
    AnomalyDetectorConfig cfg;
    cfg.window_size = 5;
    cfg.enabled = true;
    cfg.z_score_threshold = 3.0;
    AnomalyDetector small_det(cfg);

    for (int i = 0; i < 20; i++) {
        small_det.recordMetric(AnomalyType::ErrorSpike, 10.0);
    }

    auto anomalies = small_det.checkAnomalies();
    EXPECT_TRUE(anomalies.empty());
}

TEST_F(AnomalyDetectorTest, InsufficientData) {
    detector_.recordMetric(AnomalyType::CostSpike, 1.0);
    detector_.recordMetric(AnomalyType::CostSpike, 1000.0);

    auto events = detector_.recentEvents();
    EXPECT_TRUE(events.empty());
}

TEST_F(AnomalyDetectorTest, RecentEvents) {
    for (int i = 0; i < 50; i++) {
        detector_.recordMetric(AnomalyType::RateSpike, 5.0);
    }
    detector_.recordMetric(AnomalyType::RateSpike, 500.0);
    detector_.recordMetric(AnomalyType::RateSpike, 5.0);

    for (int i = 0; i < 50; i++) {
        detector_.recordMetric(AnomalyType::LatencySpike, 20.0);
    }
    detector_.recordMetric(AnomalyType::LatencySpike, 2000.0);

    auto events = detector_.recentEvents(5);
    EXPECT_LE(events.size(), 5u);
}

TEST_F(AnomalyDetectorTest, ConfigUpdate) {
    AnomalyDetectorConfig new_cfg;
    new_cfg.z_score_threshold = 2.0;
    new_cfg.window_size = 50;
    new_cfg.enabled = false;
    detector_.setConfig(new_cfg);

    EXPECT_EQ(detector_.anomalyConfig().z_score_threshold, 2.0);
    EXPECT_EQ(detector_.anomalyConfig().window_size, 50u);
    EXPECT_FALSE(detector_.anomalyConfig().enabled);
}

TEST_F(AnomalyDetectorTest, ClearResetsState) {
    for (int i = 0; i < 50; i++) {
        detector_.recordMetric(AnomalyType::RateSpike, 10.0);
    }
    detector_.recordMetric(AnomalyType::RateSpike, 1000.0);

    detector_.clear();
    auto events = detector_.recentEvents();
    EXPECT_TRUE(events.empty());

    auto anomalies = detector_.checkAnomalies();
    EXPECT_TRUE(anomalies.empty());
}

TEST_F(AnomalyDetectorTest, CheckAnomaliesReturnsCurrentState) {
    for (int i = 0; i < 50; i++) {
        detector_.recordMetric(AnomalyType::ErrorSpike, 10.0);
    }
    auto anomalies = detector_.checkAnomalies();
    EXPECT_TRUE(anomalies.empty());

    detector_.recordMetric(AnomalyType::ErrorSpike, 500.0);
    anomalies = detector_.checkAnomalies();
    ASSERT_FALSE(anomalies.empty());
    EXPECT_EQ(anomalies[0].type, AnomalyType::ErrorSpike);
}
