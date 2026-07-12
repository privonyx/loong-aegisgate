#include <gtest/gtest.h>
#include "observe/alerting.h"
#include "observe/metrics.h"
#include "core/context.h"
#include "core/config.h"
#include <chrono>

using namespace aegisgate;

class AlertIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        MetricsRegistry::instance().resetAll();
        auto eg = FeatureGate::createUnlocked(Edition::Enterprise);
        enterprise_gate_ = std::make_unique<FeatureGate>(std::move(eg));
    }

    void TearDown() override { MetricsRegistry::instance().resetAll(); }

    std::unique_ptr<FeatureGate> enterprise_gate_;
};

TEST_F(AlertIntegrationTest, AlertManagerIsAPipelineStage) {
    AlertManager manager(*enterprise_gate_);
    RequestContext ctx;
    auto result = manager.process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
}

TEST_F(AlertIntegrationTest, AlertManagerChecksRulesOnProcess) {
    AlertManager manager(*enterprise_gate_);
    AlertRule rule;
    rule.id = "test";
    rule.description = "guardrail blocks over threshold";
    rule.metric_name = "guardrail_blocks_total";
    rule.threshold = 0.5;
    manager.addRule(rule);

    MetricsRegistry::instance().guardrailBlocksTotal().inc({}, 1.0);

    RequestContext ctx;
    auto result = manager.process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    ASSERT_EQ(manager.firedAlerts().size(), 1u);
    EXPECT_EQ(manager.firedAlerts()[0].rule_id, "test");
}

TEST_F(AlertIntegrationTest, AlertManagerProcessChunkReturnsContinue) {
    AlertManager manager(*enterprise_gate_);
    RequestContext ctx;
    auto r = manager.processChunk(ctx, "chunk");
    EXPECT_EQ(r, StageResult::Continue);
}

// --- TASK-20260701-02: config-driven alerting + per-rule cooldown ---

TEST(ParseAlertSeverityTest, MapsStrings) {
    EXPECT_EQ(parseAlertSeverity("info"), AlertSeverity::Info);
    EXPECT_EQ(parseAlertSeverity("warning"), AlertSeverity::Warning);
    EXPECT_EQ(parseAlertSeverity("warn"), AlertSeverity::Warning);
    EXPECT_EQ(parseAlertSeverity("critical"), AlertSeverity::Critical);
    EXPECT_EQ(parseAlertSeverity("crit"), AlertSeverity::Critical);
    // Unknown / empty defaults to Warning.
    EXPECT_EQ(parseAlertSeverity(""), AlertSeverity::Warning);
    EXPECT_EQ(parseAlertSeverity("bogus"), AlertSeverity::Warning);
    // Case-insensitive.
    EXPECT_EQ(parseAlertSeverity("CRITICAL"), AlertSeverity::Critical);
}

// End-to-end contract that locks the P0-G bug: rules loaded from config
// (via the same glue the assembler uses) must actually drive alerting.
TEST_F(AlertIntegrationTest, ConfigRulesDriveAlerting) {
    Config config;
    ASSERT_TRUE(config.loadFromString(R"(
edition: community
alerting:
  rules:
    - id: too_many_requests
      metric: requests_total
      threshold: 1
      severity: critical
)"));

    AlertManager manager(*enterprise_gate_);
    for (const auto& rc : config.alertRules()) {
        AlertRule rule;
        rule.id = rc.id;
        rule.description = rc.description;
        rule.metric_name = rc.metric_name;
        rule.threshold = rc.threshold;
        rule.enabled = rc.enabled;
        rule.severity = parseAlertSeverity(rc.severity);
        rule.cooldown_seconds = rc.cooldown_seconds;
        manager.addRule(rule);
    }
    ASSERT_EQ(manager.ruleCount(), 1u);

    MetricsRegistry::instance().requestsTotal().inc({}, 5.0);

    RequestContext ctx;
    ASSERT_EQ(manager.process(ctx), StageResult::Continue);
    ASSERT_EQ(manager.firedAlerts().size(), 1u);
    EXPECT_EQ(manager.firedAlerts()[0].rule_id, "too_many_requests");
    EXPECT_EQ(manager.firedAlerts()[0].severity, AlertSeverity::Critical);
}

TEST_F(AlertIntegrationTest, CooldownSuppressesRepeatFire) {
    AlertManager manager(*enterprise_gate_);
    // Freeze the clock so both checks land inside the cooldown window.
    auto t0 = std::chrono::steady_clock::now();
    manager.setClockForTest([t0] { return t0; });

    AlertRule rule;
    rule.id = "cooling";
    rule.metric_name = "requests_total";
    rule.threshold = 1.0;
    rule.cooldown_seconds = 60;
    manager.addRule(rule);

    manager.checkValue("requests_total", 5.0);
    manager.checkValue("requests_total", 5.0);  // within cooldown -> suppressed

    EXPECT_EQ(manager.firedAlerts().size(), 1u);
}

TEST_F(AlertIntegrationTest, CooldownExpiryRefires) {
    AlertManager manager(*enterprise_gate_);
    auto t0 = std::chrono::steady_clock::now();
    auto now = std::make_shared<std::chrono::steady_clock::time_point>(t0);
    manager.setClockForTest([now] { return *now; });

    AlertRule rule;
    rule.id = "cooling";
    rule.metric_name = "requests_total";
    rule.threshold = 1.0;
    rule.cooldown_seconds = 60;
    manager.addRule(rule);

    manager.checkValue("requests_total", 5.0);
    *now = t0 + std::chrono::seconds(61);  // advance past cooldown
    manager.checkValue("requests_total", 5.0);

    EXPECT_EQ(manager.firedAlerts().size(), 2u);
}
