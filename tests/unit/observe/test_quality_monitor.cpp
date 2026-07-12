#include <gtest/gtest.h>
#include "observe/quality_monitor.h"
#include <cmath>

using namespace aegisgate;

class QualityMonitorTest : public ::testing::Test {
protected:
    void SetUp() override {
        QualityMonitorConfig cfg;
        cfg.enabled = true;
        cfg.ema_alpha = 0.1;
        cfg.alert_threshold = 0.3;
        cfg.min_samples = 10;
        monitor_.setConfig(cfg);
        monitor_.clear();
    }
    QualityMonitor monitor_;
};

TEST_F(QualityMonitorTest, RecordAndGetTrend) {
    monitor_.recordQuality("gpt-4", 0.9);
    monitor_.recordQuality("gpt-4", 0.85);

    auto trend = monitor_.getTrend("gpt-4");
    ASSERT_TRUE(trend.has_value());
    EXPECT_EQ(trend->model, "gpt-4");
    EXPECT_EQ(trend->sample_count, 2u);
    EXPECT_GT(trend->current_ema, 0.0);
}

TEST_F(QualityMonitorTest, EMACalculation) {
    double alpha = 0.1;
    monitor_.recordQuality("model-a", 1.0);

    auto t1 = monitor_.getTrend("model-a");
    ASSERT_TRUE(t1.has_value());
    EXPECT_NEAR(t1->current_ema, 1.0, 1e-9);

    monitor_.recordQuality("model-a", 0.5);
    auto t2 = monitor_.getTrend("model-a");
    ASSERT_TRUE(t2.has_value());
    double expected = alpha * 0.5 + (1.0 - alpha) * 1.0;
    EXPECT_NEAR(t2->current_ema, expected, 1e-9);
}

TEST_F(QualityMonitorTest, AlertOnLowQuality) {
    for (int i = 0; i < 15; i++) {
        monitor_.recordQuality("bad-model", 0.1);
    }

    auto trend = monitor_.getTrend("bad-model");
    ASSERT_TRUE(trend.has_value());
    EXPECT_TRUE(trend->alert_triggered);
    EXPECT_LT(trend->current_ema, 0.3);
}

TEST_F(QualityMonitorTest, MultipleModels) {
    for (int i = 0; i < 5; i++) {
        monitor_.recordQuality("model-x", 0.9);
        monitor_.recordQuality("model-y", 0.4);
    }

    auto trends = monitor_.getTrends();
    EXPECT_EQ(trends.size(), 2u);

    auto tx = monitor_.getTrend("model-x");
    auto ty = monitor_.getTrend("model-y");
    ASSERT_TRUE(tx.has_value());
    ASSERT_TRUE(ty.has_value());
    EXPECT_GT(tx->current_ema, ty->current_ema);
}

TEST_F(QualityMonitorTest, InsufficientSamples) {
    for (int i = 0; i < 5; i++) {
        monitor_.recordQuality("new-model", 0.1);
    }

    auto trend = monitor_.getTrend("new-model");
    ASSERT_TRUE(trend.has_value());
    EXPECT_FALSE(trend->alert_triggered);
    EXPECT_EQ(trend->sample_count, 5u);
}

TEST_F(QualityMonitorTest, NonexistentModelReturnsNullopt) {
    auto trend = monitor_.getTrend("nonexistent");
    EXPECT_FALSE(trend.has_value());
}

TEST_F(QualityMonitorTest, ClearResetsState) {
    monitor_.recordQuality("gpt-4", 0.8);
    EXPECT_EQ(monitor_.getTrends().size(), 1u);

    monitor_.clear();
    EXPECT_TRUE(monitor_.getTrends().empty());
    EXPECT_FALSE(monitor_.getTrend("gpt-4").has_value());
}

TEST_F(QualityMonitorTest, SlopeTracksDirection) {
    monitor_.recordQuality("model-s", 0.8);
    monitor_.recordQuality("model-s", 0.9);

    auto trend = monitor_.getTrend("model-s");
    ASSERT_TRUE(trend.has_value());
    EXPECT_GT(trend->slope, 0.0);

    monitor_.recordQuality("model-s", 0.1);
    trend = monitor_.getTrend("model-s");
    ASSERT_TRUE(trend.has_value());
    EXPECT_LT(trend->slope, 0.0);
}

// === TASK-20260527-02 — quality_reason taxonomy (D5=A 3 buckets) ===
// quality_reason 是 MVP-5 case study 头条数字之一（spec §3.5 Row 4 Card 3）：
// 让 adopter 看到"质量没掉"的可解释证据。3 档枚举：
//   factuality / refusal / latency_degraded
// SR3：白名单外的 reason（如 "hallucination"）必须被静默丢弃，0 计数累加。

TEST_F(QualityMonitorTest, RecordsReasonTaxonomy) {
    // 各档 reason 互不影响，独立累加。
    monitor_.recordQuality("gpt-4", 0.8, "factuality");
    monitor_.recordQuality("gpt-4", 0.6, "factuality");
    monitor_.recordQuality("gpt-4", 0.7, "refusal");
    monitor_.recordQuality("gpt-4", 0.5, "latency_degraded");
    monitor_.recordQuality("gpt-4", 0.5, "latency_degraded");
    monitor_.recordQuality("gpt-4", 0.5, "latency_degraded");

    auto trend = monitor_.getTrend("gpt-4");
    ASSERT_TRUE(trend.has_value());
    EXPECT_EQ(trend->reason_factuality, 2u);
    EXPECT_EQ(trend->reason_refusal, 1u);
    EXPECT_EQ(trend->reason_latency_degraded, 3u);
}

TEST_F(QualityMonitorTest, RejectsUnknownReason) {
    // SR3 反向锚点：未在白名单内的 reason 字符串（如 "hallucination" /
    // "format_error" / 任意其他值）必须被静默丢弃，0 计数累加，但 EMA
    // 与 sample_count 仍按正常 recordQuality 流程累加。
    monitor_.recordQuality("gpt-4", 0.8, "hallucination");
    monitor_.recordQuality("gpt-4", 0.7, "format_error");
    monitor_.recordQuality("gpt-4", 0.6, "RANDOM_GIBBERISH");

    auto trend = monitor_.getTrend("gpt-4");
    ASSERT_TRUE(trend.has_value());
    EXPECT_EQ(trend->reason_factuality, 0u);
    EXPECT_EQ(trend->reason_refusal, 0u);
    EXPECT_EQ(trend->reason_latency_degraded, 0u);
    // 但 sample_count 仍然正常累加（recordQuality 主流程未被打断）。
    EXPECT_EQ(trend->sample_count, 3u);
}

TEST_F(QualityMonitorTest, TrendIncludesReasonCounts) {
    // 默认 reason="" 不增加任何 reason_* 计数（既有 recordQuality(model, score)
    // 调用站点不受影响，向后兼容）。
    monitor_.recordQuality("model-default", 0.9);
    monitor_.recordQuality("model-default", 0.8);

    auto trend = monitor_.getTrend("model-default");
    ASSERT_TRUE(trend.has_value());
    EXPECT_EQ(trend->reason_factuality, 0u);
    EXPECT_EQ(trend->reason_refusal, 0u);
    EXPECT_EQ(trend->reason_latency_degraded, 0u);
    EXPECT_EQ(trend->sample_count, 2u);
}
