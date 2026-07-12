#include <gtest/gtest.h>
#include "observe/alerting.h"
#include "observe/metrics.h"
#include "core/context.h"

using namespace aegisgate;

// C1 (REV20260702-C1): parseMetricSelector splits a rule metric string into base
// name + label filter.
TEST(MetricSelectorTest, NoBracesNameOnly) {
    auto sel = parseMetricSelector("requests_total");
    EXPECT_EQ(sel.name, "requests_total");
    EXPECT_TRUE(sel.labels.labels.empty());
}

TEST(MetricSelectorTest, SingleQuotedLabel) {
    auto sel = parseMetricSelector("requests_total{status=\"rejected\"}");
    EXPECT_EQ(sel.name, "requests_total");
    ASSERT_EQ(sel.labels.labels.size(), 1u);
    EXPECT_EQ(sel.labels.labels[0].first, "status");
    EXPECT_EQ(sel.labels.labels[0].second, "rejected");
}

TEST(MetricSelectorTest, MultipleLabelsMixedWhitespaceAndQuotes) {
    auto sel = parseMetricSelector("requests_total{ model=\"gpt-4\", status = ok }");
    EXPECT_EQ(sel.name, "requests_total");
    ASSERT_EQ(sel.labels.labels.size(), 2u);
    EXPECT_EQ(sel.labels.labels[0].first, "model");
    EXPECT_EQ(sel.labels.labels[0].second, "gpt-4");
    EXPECT_EQ(sel.labels.labels[1].first, "status");
    EXPECT_EQ(sel.labels.labels[1].second, "ok");
}

class AlertManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto eg = FeatureGate::createUnlocked(Edition::Enterprise);
        enterprise_gate_ = std::make_unique<FeatureGate>(std::move(eg));
        community_gate_ = std::make_unique<FeatureGate>(Edition::Community);
    }
    std::unique_ptr<FeatureGate> enterprise_gate_;
    std::unique_ptr<FeatureGate> community_gate_;
};

TEST_F(AlertManagerTest, FiresAlertWhenThresholdExceeded) {
    AlertManager mgr(*enterprise_gate_);
    AlertRule rule;
    rule.id = "high_cost";
    rule.description = "Cost exceeds $10";
    rule.severity = AlertSeverity::Warning;
    rule.metric_name = "cost";
    rule.threshold = 10.0;
    mgr.addRule(rule);

    mgr.checkValue("cost", 15.0);
    ASSERT_EQ(mgr.firedAlerts().size(), 1u);
    EXPECT_EQ(mgr.firedAlerts()[0].rule_id, "high_cost");
    EXPECT_EQ(mgr.firedAlerts()[0].current_value, 15.0);
}

TEST_F(AlertManagerTest, DoesNotFireBelowThreshold) {
    AlertManager mgr(*enterprise_gate_);
    AlertRule rule;
    rule.id = "high_cost";
    rule.metric_name = "cost";
    rule.threshold = 10.0;
    mgr.addRule(rule);

    mgr.checkValue("cost", 5.0);
    EXPECT_EQ(mgr.firedAlerts().size(), 0u);
}

TEST_F(AlertManagerTest, SkipsInCommunityEdition) {
    AlertManager mgr(*community_gate_);
    AlertRule rule;
    rule.id = "high_cost";
    rule.metric_name = "cost";
    rule.threshold = 10.0;
    mgr.addRule(rule);

    mgr.checkValue("cost", 100.0);
    EXPECT_EQ(mgr.firedAlerts().size(), 0u);
}

TEST_F(AlertManagerTest, ChannelReceivesAlert) {
    AlertManager mgr(*enterprise_gate_);
    std::vector<Alert> received;
    mgr.setChannel([&received](const Alert& a) {
        received.push_back(a);
    });

    AlertRule rule;
    rule.id = "error_rate";
    rule.description = "Error rate too high";
    rule.severity = AlertSeverity::Critical;
    rule.metric_name = "error_rate";
    rule.threshold = 0.1;
    mgr.addRule(rule);

    mgr.checkValue("error_rate", 0.25);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].severity, AlertSeverity::Critical);
}

TEST_F(AlertManagerTest, MultipleRulesEvaluated) {
    AlertManager mgr(*enterprise_gate_);

    AlertRule r1;
    r1.id = "cost_warn"; r1.metric_name = "cost"; r1.threshold = 5.0;
    r1.severity = AlertSeverity::Warning;
    AlertRule r2;
    r2.id = "cost_crit"; r2.metric_name = "cost"; r2.threshold = 20.0;
    r2.severity = AlertSeverity::Critical;
    mgr.addRule(r1);
    mgr.addRule(r2);

    mgr.checkValue("cost", 25.0);
    EXPECT_EQ(mgr.firedAlerts().size(), 2u);
}

TEST_F(AlertManagerTest, OnlyMatchingMetricFires) {
    AlertManager mgr(*enterprise_gate_);

    AlertRule r1;
    r1.id = "cost_alert"; r1.metric_name = "cost"; r1.threshold = 10.0;
    AlertRule r2;
    r2.id = "latency_alert"; r2.metric_name = "latency"; r2.threshold = 5.0;
    mgr.addRule(r1);
    mgr.addRule(r2);

    mgr.checkValue("cost", 15.0);
    ASSERT_EQ(mgr.firedAlerts().size(), 1u);
    EXPECT_EQ(mgr.firedAlerts()[0].rule_id, "cost_alert");
}

TEST_F(AlertManagerTest, ClearAlerts) {
    AlertManager mgr(*enterprise_gate_);
    AlertRule rule;
    rule.id = "test"; rule.metric_name = "x"; rule.threshold = 0;
    mgr.addRule(rule);

    mgr.checkValue("x", 1.0);
    EXPECT_EQ(mgr.firedAlerts().size(), 1u);
    mgr.clearAlerts();
    EXPECT_EQ(mgr.firedAlerts().size(), 0u);
}

TEST_F(AlertManagerTest, AlertHasTimestamp) {
    AlertManager mgr(*enterprise_gate_);
    AlertRule rule;
    rule.id = "ts_test"; rule.metric_name = "m"; rule.threshold = 0;
    mgr.addRule(rule);

    mgr.checkValue("m", 1.0);
    ASSERT_EQ(mgr.firedAlerts().size(), 1u);
    EXPECT_FALSE(mgr.firedAlerts()[0].timestamp.empty());
    EXPECT_NE(mgr.firedAlerts()[0].timestamp.find("T"), std::string::npos);
}

TEST_F(AlertManagerTest, RuleCountWorks) {
    AlertManager mgr(*enterprise_gate_);
    EXPECT_EQ(mgr.ruleCount(), 0u);
    AlertRule r; r.id = "r1"; r.metric_name = "m"; r.threshold = 1;
    mgr.addRule(r);
    EXPECT_EQ(mgr.ruleCount(), 1u);
}

// C1 regression: production increments requests_total with labels (model/status)
// so the empty-label bucket stays 0. process() must sum all buckets, otherwise a
// rule on requests_total never fires.
TEST_F(AlertManagerTest, ProcessFiresOnLabeledCounter) {
    auto& reg = MetricsRegistry::instance();
    reg.resetAll();
    reg.requestsTotal().inc({{{"model", "gpt-4"}, {"status", "ok"}}, "", false}, 10.0);

    AlertManager mgr(*enterprise_gate_);
    AlertRule rule;
    rule.id = "req_high";
    rule.metric_name = "requests_total";
    rule.threshold = 5.0;
    mgr.addRule(rule);

    RequestContext ctx;
    mgr.process(ctx);
    ASSERT_EQ(mgr.firedAlerts().size(), 1u);
    EXPECT_DOUBLE_EQ(mgr.firedAlerts()[0].current_value, 10.0);
}

// C1: a selector rule sums only the matching label subset.
TEST_F(AlertManagerTest, ProcessSelectorFiltersBySubset) {
    auto& reg = MetricsRegistry::instance();
    reg.resetAll();
    reg.requestsTotal().inc({{{"model", "gpt-4"}, {"status", "rejected"}}, "", false}, 3.0);
    reg.requestsTotal().inc({{{"model", "gpt-4"}, {"status", "ok"}}, "", false}, 8.0);

    AlertManager mgr(*enterprise_gate_);
    AlertRule rule;
    rule.id = "rejected_high";
    rule.metric_name = "requests_total{status=\"rejected\"}";
    rule.threshold = 5.0;
    mgr.addRule(rule);

    RequestContext ctx;
    mgr.process(ctx);
    EXPECT_EQ(mgr.firedAlerts().size(), 0u);  // rejected sum 3 < 5

    reg.requestsTotal().inc({{{"model", "claude"}, {"status", "rejected"}}, "", false}, 4.0);
    mgr.process(ctx);
    ASSERT_EQ(mgr.firedAlerts().size(), 1u);  // rejected sum 7 >= 5
    EXPECT_DOUBLE_EQ(mgr.firedAlerts()[0].current_value, 7.0);
}
