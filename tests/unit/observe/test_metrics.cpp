#include <gtest/gtest.h>
#include "observe/metrics.h"
#include "gateway/circuit_breaker.h"

using namespace aegisgate;

class CounterTest : public ::testing::Test {
protected:
    Counter counter_{"test_counter", "A test counter"};
};

TEST_F(CounterTest, StartsAtZero) {
    EXPECT_DOUBLE_EQ(counter_.get(), 0.0);
}

TEST_F(CounterTest, Increments) {
    counter_.inc();
    EXPECT_DOUBLE_EQ(counter_.get(), 1.0);
    counter_.inc({}, 5.0);
    EXPECT_DOUBLE_EQ(counter_.get(), 6.0);
}

TEST_F(CounterTest, IncrementsWithLabels) {
    LabelSet l1{{{"model", "gpt-4"}, {"status", "ok"}}, "", false};
    LabelSet l2{{{"model", "claude-3"}, {"status", "ok"}}, "", false};

    counter_.inc(l1, 3.0);
    counter_.inc(l2, 7.0);
    counter_.inc(l1, 2.0);

    EXPECT_DOUBLE_EQ(counter_.get(l1), 5.0);
    EXPECT_DOUBLE_EQ(counter_.get(l2), 7.0);
}

TEST_F(CounterTest, ExposesPrometheusFormat) {
    counter_.inc({}, 42.0);
    auto text = counter_.expose();
    EXPECT_NE(text.find("# HELP test_counter"), std::string::npos);
    EXPECT_NE(text.find("# TYPE test_counter counter"), std::string::npos);
    EXPECT_NE(text.find("test_counter 42"), std::string::npos);
}

TEST_F(CounterTest, ExposesLabelsInPrometheus) {
    LabelSet l{{{"model", "gpt-4"}}, "", false};
    counter_.inc(l, 10.0);
    auto text = counter_.expose();
    EXPECT_NE(text.find("model=\"gpt-4\""), std::string::npos);
}

TEST_F(CounterTest, ResetClearsAll) {
    counter_.inc({}, 100.0);
    counter_.reset();
    EXPECT_DOUBLE_EQ(counter_.get(), 0.0);
}

// C1 (REV20260702-C1): getSum with empty filter must total all label buckets,
// so alerting on a labeled metric reads the real sum instead of the empty
// bucket (0).
TEST_F(CounterTest, GetSumEmptyFilterSumsAllLabels) {
    LabelSet l1{{{"model", "gpt-4"}, {"status", "ok"}}, "", false};
    LabelSet l2{{{"model", "claude-3"}, {"status", "rejected"}}, "", false};
    counter_.inc(l1, 3.0);
    counter_.inc(l2, 7.0);
    counter_.inc({}, 1.0);
    EXPECT_DOUBLE_EQ(counter_.getSum({}), 11.0);
}

// C1: getSum with a label filter sums only buckets whose labels are a superset.
TEST_F(CounterTest, GetSumSubsetMatchesLabelFilter) {
    LabelSet l1{{{"model", "gpt-4"}, {"status", "rejected"}}, "", false};
    LabelSet l2{{{"model", "claude-3"}, {"status", "rejected"}}, "", false};
    LabelSet l3{{{"model", "gpt-4"}, {"status", "ok"}}, "", false};
    counter_.inc(l1, 3.0);
    counter_.inc(l2, 4.0);
    counter_.inc(l3, 5.0);
    LabelSet rejected{{{"status", "rejected"}}, "", false};
    EXPECT_DOUBLE_EQ(counter_.getSum(rejected), 7.0);
    LabelSet nomatch{{{"status", "timeout"}}, "", false};
    EXPECT_DOUBLE_EQ(counter_.getSum(nomatch), 0.0);
}

class HistogramTest : public ::testing::Test {
protected:
    Histogram hist_{"test_histogram", "A test histogram",
                    {0.01, 0.05, 0.1, 0.5, 1.0}};
};

TEST_F(HistogramTest, ObservesValues) {
    hist_.observe(0.02);
    hist_.observe(0.07);
    hist_.observe(0.3);
    hist_.observe(2.0);

    auto text = hist_.expose();
    EXPECT_NE(text.find("test_histogram_bucket"), std::string::npos);
    EXPECT_NE(text.find("le=\"+Inf\""), std::string::npos);
    EXPECT_NE(text.find("test_histogram_sum"), std::string::npos);
    EXPECT_NE(text.find("test_histogram_count"), std::string::npos);
}

TEST_F(HistogramTest, BucketCountsAreCorrect) {
    hist_.observe(0.005);
    hist_.observe(0.03);
    hist_.observe(0.08);
    hist_.observe(0.3);
    hist_.observe(5.0);

    auto text = hist_.expose();
    // le=0.01: 1 (0.005), le=0.05: 2 (0.005, 0.03), le=0.1: 3, le=0.5: 4, le=1.0: 4, +Inf: 5
    EXPECT_NE(text.find("_count"), std::string::npos);
}

class GaugeTest : public ::testing::Test {
protected:
    Gauge gauge_{"test_gauge", "A test gauge"};
};

TEST_F(GaugeTest, StartsAtZero) {
    EXPECT_DOUBLE_EQ(gauge_.get(), 0.0);
}

TEST_F(GaugeTest, SetsValue) {
    gauge_.set(42.0);
    EXPECT_DOUBLE_EQ(gauge_.get(), 42.0);
}

TEST_F(GaugeTest, IncAndDec) {
    gauge_.inc();
    gauge_.inc();
    EXPECT_DOUBLE_EQ(gauge_.get(), 2.0);
    gauge_.dec();
    EXPECT_DOUBLE_EQ(gauge_.get(), 1.0);
}

TEST_F(GaugeTest, ExposesPrometheusFormat) {
    gauge_.set(7.5);
    auto text = gauge_.expose();
    EXPECT_NE(text.find("# TYPE test_gauge gauge"), std::string::npos);
    EXPECT_NE(text.find("test_gauge 7.5"), std::string::npos);
}

// C1: Gauge getSum totals all label buckets (empty filter).
TEST_F(GaugeTest, GetSumSumsAllLabels) {
    LabelSet l1{{{"node", "a"}}, "", false};
    LabelSet l2{{{"node", "b"}}, "", false};
    gauge_.set(2.0, l1);
    gauge_.set(3.0, l2);
    EXPECT_DOUBLE_EQ(gauge_.getSum({}), 5.0);
}

class MetricsRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        MetricsRegistry::instance().resetAll();
    }
};

TEST_F(MetricsRegistryTest, SingletonExists) {
    auto& reg = MetricsRegistry::instance();
    EXPECT_EQ(reg.requestsTotal().name(), "aegisgate_requests_total");
    EXPECT_EQ(reg.tokensTotal().name(), "aegisgate_tokens_total");
    EXPECT_EQ(reg.guardrailBlocksTotal().name(), "aegisgate_guardrail_blocks_total");
}

TEST_F(MetricsRegistryTest, ExposeAllProducesValidOutput) {
    auto& reg = MetricsRegistry::instance();
    reg.requestsTotal().inc({}, 5.0);
    reg.activeConnections().set(3.0);

    auto text = reg.exposeAll();
    EXPECT_NE(text.find("aegisgate_requests_total"), std::string::npos);
    EXPECT_NE(text.find("aegisgate_active_connections"), std::string::npos);
    EXPECT_NE(text.find("aegisgate_request_duration_seconds"), std::string::npos);
    EXPECT_NE(text.find("aegisgate_upstream_duration_seconds"), std::string::npos);
    // P1-B: modality_rate_limited_total was registered but omitted from
    // exposeAll(), so it never reached Prometheus scrapes.
    EXPECT_NE(text.find("aegisgate_modality_rate_limited_total"), std::string::npos);
}

TEST_F(MetricsRegistryTest, UpstreamDurationRegistered) {
    auto& reg = MetricsRegistry::instance();
    EXPECT_EQ(reg.upstreamDuration().name(), "aegisgate_upstream_duration_seconds");
    reg.upstreamDuration().observe(0.25);
    auto text = reg.upstreamDuration().expose();
    EXPECT_NE(text.find("aegisgate_upstream_duration_seconds_bucket"), std::string::npos);
    EXPECT_NE(text.find("aegisgate_upstream_duration_seconds_count"), std::string::npos);
}

TEST_F(MetricsRegistryTest, ResetClearsAllMetrics) {
    auto& reg = MetricsRegistry::instance();
    reg.requestsTotal().inc({}, 100.0);
    reg.resetAll();
    EXPECT_DOUBLE_EQ(reg.requestsTotal().get(), 0.0);
}

TEST(LabelSetTest, KeyFormat) {
    LabelSet l{{{"model", "gpt-4"}, {"status", "ok"}}, "", false};
    EXPECT_EQ(l.key(), "model=gpt-4,status=ok");
}

TEST(LabelSetTest, PrometheusFormat) {
    LabelSet l{{{"model", "gpt-4"}}, "", false};
    EXPECT_EQ(l.prometheus(), "{model=\"gpt-4\"}");
}

TEST(LabelSetTest, PrometheusEscapesSpecialCharsInValues) {
    LabelSet l{{{"x", "a\\b\"c\nd"}}, "", false};
    EXPECT_EQ(l.prometheus(), "{x=\"a\\\\b\\\"c\\nd\"}");
}

TEST(LabelSetTest, EmptyLabels) {
    LabelSet l;
    EXPECT_EQ(l.key(), "");
    EXPECT_EQ(l.prometheus(), "");
}

TEST(LabelSetTest, KeyReturnsSameReference) {
    LabelSet ls{{{"model", "gpt-4"}, {"provider", "openai"}}, "", false};
    const std::string& ref1 = ls.key();
    const std::string& ref2 = ls.key();
    EXPECT_EQ(&ref1, &ref2);
    EXPECT_EQ(ref1, "model=gpt-4,provider=openai");
}

TEST(LabelSetTest, EmptyLabelsCachedKey) {
    LabelSet ls;
    const std::string& ref = ls.key();
    EXPECT_TRUE(ref.empty());
    EXPECT_EQ(&ref, &ls.key());
}

// TASK-20260708-02 / REV20260707-C1 Epic 2 — rate_limiter degraded counter
// increments each time the assembler falls back from Redis to in-memory
// rate limiting because Redis is unhealthy / init failed. Enables ops
// alerting on silent degradation of "cluster quotas" claim.
TEST_F(MetricsRegistryTest, RateLimiterDegradedTotalStartsAtZero) {
    auto& counter = MetricsRegistry::instance().rateLimiterDegradedTotal();
    counter.reset();
    EXPECT_DOUBLE_EQ(counter.get(), 0.0);
}

TEST_F(MetricsRegistryTest, RateLimiterDegradedTotalIncrements) {
    auto& counter = MetricsRegistry::instance().rateLimiterDegradedTotal();
    counter.reset();
    counter.inc();
    counter.inc();
    EXPECT_DOUBLE_EQ(counter.get(), 2.0);
}

TEST_F(MetricsRegistryTest, RateLimiterDegradedTotalInExposeAll) {
    auto& counter = MetricsRegistry::instance().rateLimiterDegradedTotal();
    counter.reset();
    counter.inc();
    std::string output = MetricsRegistry::instance().exposeAll();
    EXPECT_NE(output.find("aegisgate_rate_limiter_degraded_total"),
              std::string::npos);
}

TEST_F(MetricsRegistryTest, CircuitBreakerStateInMetricsOutput) {
    auto& gauge = MetricsRegistry::instance().circuitBreakerState();

    CircuitConfig cfg;
    cfg.failure_threshold = 2;
    CircuitBreaker breaker(cfg);

    breaker.recordFailure("gpt-4");
    breaker.recordFailure("gpt-4");
    ASSERT_EQ(breaker.state("gpt-4"), CircuitState::Open);

    gauge.reset();
    breaker.exportMetrics(gauge);

    std::string output = MetricsRegistry::instance().exposeAll();
    EXPECT_NE(output.find("aegisgate_circuit_breaker_state"), std::string::npos);
    EXPECT_NE(output.find("model=\"gpt-4\""), std::string::npos);
}
