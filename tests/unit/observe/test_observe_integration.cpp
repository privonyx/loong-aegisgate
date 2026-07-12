#include <gtest/gtest.h>
#include "core/pipeline.h"
#include "observe/request_logger.h"
#include "observe/metrics.h"
#include "observe/cost_tracker.h"
#include "observe/alerting.h"
#include "core/feature_gate.h"

using namespace aegisgate;

class ObserveIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        MetricsRegistry::instance().resetAll();
    }
};

TEST_F(ObserveIntegrationTest, FullObservabilityPipeline) {
    Pipeline pipeline;

    auto logger = std::make_unique<RequestLogger>();
    auto* logger_ptr = logger.get();
    auto cost = std::make_unique<CostTracker>();
    auto* cost_ptr = cost.get();
    cost->setPricing("gpt-4", 0.03, 0.06);

    pipeline.addStage(std::move(cost));
    pipeline.addStage(std::move(logger));

    RequestContext ctx;
    ctx.request_id = "integ-obs-001";
    ctx.tenant_id = "tenant-a";
    ctx.chat_request.model = "gpt-4";
    ctx.token_usage = {1000, 500, 1500};
    ctx.start_time = std::chrono::steady_clock::now();

    EXPECT_EQ(pipeline.execute(ctx), PipelineResult::Success);

    ASSERT_EQ(logger_ptr->logs().size(), 1u);
    EXPECT_EQ(logger_ptr->logs()[0]["request_id"], "integ-obs-001");

    ASSERT_EQ(cost_ptr->records().size(), 1u);
    EXPECT_NEAR(cost_ptr->records()[0].total_cost, 0.06, 0.001);
}

TEST_F(ObserveIntegrationTest, MetricsTrackRequestCounts) {
    auto& reg = MetricsRegistry::instance();

    LabelSet l1{{{"model", "gpt-4"}, {"status", "ok"}}, "", false};
    LabelSet l2{{{"model", "claude-3"}, {"status", "ok"}}, "", false};

    reg.requestsTotal().inc(l1);
    reg.requestsTotal().inc(l1);
    reg.requestsTotal().inc(l2);

    EXPECT_DOUBLE_EQ(reg.requestsTotal().get(l1), 2.0);
    EXPECT_DOUBLE_EQ(reg.requestsTotal().get(l2), 1.0);
}

TEST_F(ObserveIntegrationTest, MetricsTrackTokens) {
    auto& reg = MetricsRegistry::instance();

    LabelSet input{{{"model", "gpt-4"}, {"type", "input"}}, "", false};
    LabelSet output{{{"model", "gpt-4"}, {"type", "output"}}, "", false};

    reg.tokensTotal().inc(input, 1000);
    reg.tokensTotal().inc(output, 500);

    EXPECT_DOUBLE_EQ(reg.tokensTotal().get(input), 1000.0);
    EXPECT_DOUBLE_EQ(reg.tokensTotal().get(output), 500.0);
}

TEST_F(ObserveIntegrationTest, MetricsTrackGuardrailBlocks) {
    auto& reg = MetricsRegistry::instance();

    LabelSet inj{{{"type", "injection"}, {"direction", "inbound"}}, "", false};
    reg.guardrailBlocksTotal().inc(inj);
    reg.guardrailBlocksTotal().inc(inj);

    EXPECT_DOUBLE_EQ(reg.guardrailBlocksTotal().get(inj), 2.0);
}

TEST_F(ObserveIntegrationTest, HistogramTracksLatency) {
    auto& reg = MetricsRegistry::instance();

    reg.requestDuration().observe(0.05);
    reg.requestDuration().observe(0.12);
    reg.requestDuration().observe(1.5);

    auto text = reg.requestDuration().expose();
    EXPECT_NE(text.find("aegisgate_request_duration_seconds_bucket"), std::string::npos);
    EXPECT_NE(text.find("_count"), std::string::npos);
}

TEST_F(ObserveIntegrationTest, GaugeTracksActiveConnections) {
    auto& reg = MetricsRegistry::instance();

    reg.activeConnections().inc();
    reg.activeConnections().inc();
    EXPECT_DOUBLE_EQ(reg.activeConnections().get(), 2.0);
    reg.activeConnections().dec();
    EXPECT_DOUBLE_EQ(reg.activeConnections().get(), 1.0);
}

TEST_F(ObserveIntegrationTest, ExposeAllProducesFullOutput) {
    auto& reg = MetricsRegistry::instance();
    reg.requestsTotal().inc({}, 10);
    reg.activeConnections().set(5);
    reg.requestDuration().observe(0.1);

    auto text = reg.exposeAll();
    EXPECT_NE(text.find("aegisgate_requests_total"), std::string::npos);
    EXPECT_NE(text.find("aegisgate_active_connections"), std::string::npos);
    EXPECT_NE(text.find("aegisgate_request_duration_seconds"), std::string::npos);
    EXPECT_NE(text.find("aegisgate_tokens_total"), std::string::npos);
    EXPECT_NE(text.find("aegisgate_guardrail_blocks_total"), std::string::npos);
}

TEST_F(ObserveIntegrationTest, AlertIntegrationWithCost) {
    auto gate = FeatureGate::createUnlocked(Edition::Enterprise);
    AlertManager alerts(gate);

    AlertRule rule;
    rule.id = "high_cost";
    rule.metric_name = "total_cost";
    rule.threshold = 0.1;
    rule.severity = AlertSeverity::Warning;
    alerts.addRule(rule);

    CostTracker tracker;
    tracker.setPricing("gpt-4", 0.03, 0.06);

    CostRecord rec;
    rec.model = "gpt-4"; rec.input_tokens = 5000; rec.output_tokens = 3000;
    rec.total_cost = 0.33;
    rec.tenant_id = "t1";
    tracker.record(rec);

    auto summary = tracker.totalSummary();
    alerts.checkAndAlert("total_cost", summary.total_cost);

    ASSERT_EQ(alerts.firedAlerts().size(), 1u);
    EXPECT_EQ(alerts.firedAlerts()[0].rule_id, "high_cost");
}
