#include <gtest/gtest.h>

#include "core/config.h"
#include "core/pipeline_assembler.h"

#include <chrono>

using namespace aegisgate;

namespace {

void loadObservabilityConfig(Config& config, bool anomaly_enabled,
                             bool optimizer_enabled,
                             double quality_alert_threshold = 0.42) {
    const std::string yaml = std::string(R"(
observability:
  anomaly_detection:
    enabled: )") + (anomaly_enabled ? "true" : "false") + R"(
    z_score_threshold: 0.5
    window_size: 4
  quality_monitoring:
    enabled: false
    alert_threshold: )" + std::to_string(quality_alert_threshold) + R"(
  cost_optimization:
    enabled: )" + (optimizer_enabled ? "true" : "false") + R"(
)";
    EXPECT_TRUE(config.loadFromString(yaml));
}

RequestContext costedContext(const std::string& id) {
    RequestContext ctx;
    ctx.request_id = id;
    ctx.tenant_id = "tenant-obs";
    ctx.app_id = "app-obs";
    ctx.target_model = "gpt-4o";
    ctx.quality_score = 0.9;
    ctx.token_usage.prompt_tokens = 1000;
    ctx.token_usage.completion_tokens = 1000;
    ctx.start_time = std::chrono::steady_clock::now();
    return ctx;
}

} // namespace

TEST(ObservabilityWiringTest, AssemblerInjectsObservabilityConfig) {
    Config config;
    loadObservabilityConfig(config, /*anomaly_enabled=*/true,
                            /*optimizer_enabled=*/true);
    auto ap = PipelineAssembler::assemble(config);

    ASSERT_NE(ap.anomaly_detector, nullptr);
    ASSERT_NE(ap.cost_optimizer, nullptr);

    const auto& anomaly_cfg = ap.anomaly_detector->anomalyConfig();
    EXPECT_TRUE(anomaly_cfg.enabled);
    EXPECT_DOUBLE_EQ(anomaly_cfg.z_score_threshold, 0.5);
    EXPECT_EQ(anomaly_cfg.window_size, 4u);
    EXPECT_TRUE(ap.cost_optimizer->costOptimizerConfig().enabled);
}

TEST(ObservabilityWiringTest, OutboundFeedsCostOptimizerWhenEnabled) {
    Config config;
    loadObservabilityConfig(config, /*anomaly_enabled=*/true,
                            /*optimizer_enabled=*/true);
    auto ap = PipelineAssembler::assemble(config);

    auto ctx = costedContext("obs-enabled");
    EXPECT_EQ(ap.outbound.execute(ctx), PipelineResult::Success);

    auto profiles = ap.cost_optimizer->getProfiles();
    ASSERT_EQ(profiles.size(), 1u);
    EXPECT_EQ(profiles[0].model, "gpt-4o");
    EXPECT_EQ(profiles[0].request_count, 1);
}

TEST(ObservabilityWiringTest, OutboundDoesNotFeedCostOptimizerWhenDisabled) {
    Config config;
    loadObservabilityConfig(config, /*anomaly_enabled=*/true,
                            /*optimizer_enabled=*/false);
    auto ap = PipelineAssembler::assemble(config);

    auto ctx = costedContext("obs-disabled");
    EXPECT_EQ(ap.outbound.execute(ctx), PipelineResult::Success);

    EXPECT_TRUE(ap.cost_optimizer->getProfiles().empty());
}

TEST(ObservabilityWiringTest, AssemblerInjectsQualityMonitoringThreshold) {
    Config config;
    loadObservabilityConfig(config, /*anomaly_enabled=*/false,
                            /*optimizer_enabled=*/false,
                            /*quality_alert_threshold=*/0.17);

    auto ap = PipelineAssembler::assemble(config);

    ASSERT_NE(ap.quality_monitor, nullptr);
    EXPECT_DOUBLE_EQ(ap.quality_monitor->qualityMonitorConfig().alert_threshold,
                     0.17);
}

TEST(ObservabilityWiringTest, BaselineTelemetryStillFlowsWhenFlagsDisabled) {
    Config config;
    loadObservabilityConfig(config, /*anomaly_enabled=*/false,
                            /*optimizer_enabled=*/false);

    auto ap = PipelineAssembler::assemble(config);

    auto ctx = costedContext("obs-baseline");
    ctx.accumulated_response =
        "This answer is complete enough for the quality scorer to record.";
    EXPECT_EQ(ap.outbound.execute(ctx), PipelineResult::Success);

    auto trend = ap.quality_monitor->getTrend("gpt-4o");
    ASSERT_TRUE(trend.has_value());
    EXPECT_EQ(trend->sample_count, 1u);
    EXPECT_EQ(ap.cost_attribution->size(), 1u);
}
