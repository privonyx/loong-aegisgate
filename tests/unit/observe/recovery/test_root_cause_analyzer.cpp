// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 3.
//
// RootCauseAnalyzer tests (14, RunbookEngine-rca integration tests live in
// test_runbook_engine.cpp — 3 more for C2.5).
//
// Coverage map → creative-phase11.4-rca-design.md §4.4:
//   Schema loading (4)
//   C2.1 3 condition kind parsing (3)
//   C2.2 scoreRule pure function (3)
//   C2.3 evidence PII mask defence-in-depth (1)
//   C2.4 min_matched_conditions boundary (1)
//   Hypothesis ranking + threshold filtering (2)

#include "aegisgate/feedback_event.h"
#include "guardrail/inbound/pii_filter.h"
#include "observe/recovery/log_ringbuffer_sink.h"
#include "observe/recovery/root_cause_analyzer.h"
#include "observe/recovery/signal_snapshot.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace aegisgate;

namespace {

namespace fs = std::filesystem;

class TempYaml {
public:
    explicit TempYaml(const std::string& body) {
        path_ = fs::temp_directory_path() /
                ("rca_rules_test_" + std::to_string(rand()) + ".yaml");
        std::ofstream(path_) << body;
    }
    ~TempYaml() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    std::string path() const { return path_.string(); }

private:
    fs::path path_;
};

RcaSignals makeSignals(double p99 = 100.0,
                         double error_rate = 0.0,
                         std::vector<FeedbackEvent> events = {},
                         std::vector<LogRingbufferSink::Entry> logs = {}) {
    RcaSignals s;
    s.metrics_now.p99_latency_ms = p99;
    s.metrics_now.error_rate = error_rate;
    s.metrics_now.sample_count = 1000;
    s.recent_feedback_events = std::move(events);
    s.recent_log_entries = std::move(logs);
    return s;
}

FeedbackEvent makeFeedbackEvent(FeedbackEventType t,
                                  const std::string& topic) {
    FeedbackEvent e;
    e.type = t;
    e.topic = topic;
    e.timestamp = std::chrono::system_clock::now();
    return e;
}

LogRingbufferSink::Entry makeLogEntry(const std::string& msg) {
    LogRingbufferSink::Entry e;
    e.ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    e.lvl = spdlog::level::warn;
    e.msg_masked = msg;
    return e;
}

} // namespace

// --- 1: schema — load valid rule -------------------------------------------

TEST(RootCauseAnalyzerTest, LoadValidRulesSucceeds) {
    TempYaml y(R"(
rules:
  - id: "rca_simple"
    category: "internal"
    summary: "test"
    min_matched_conditions: 1
    score_when_all_matched: 0.9
    score_per_condition: 0.5
    conditions:
      - kind: "metric_threshold"
        signal: "p99_latency_ms"
        op: ">"
        value: 1000
        evidence_label: "p99 ${current}"
)");
    RootCauseAnalyzer rca(std::make_shared<PIIFilter>());
    EXPECT_TRUE(rca.reloadRules(y.path()));
    EXPECT_EQ(rca.health().first, 1u);
}

// --- 2: schema — empty file rejected ---------------------------------------

TEST(RootCauseAnalyzerTest, LoadEmptyFileFails) {
    TempYaml y("");
    RootCauseAnalyzer rca(std::make_shared<PIIFilter>());
    EXPECT_FALSE(rca.reloadRules(y.path()));
}

// --- 3: schema — missing id field rejected ---------------------------------

TEST(RootCauseAnalyzerTest, LoadRulesMissingIdRejects) {
    TempYaml y(R"(
rules:
  - category: "internal"
    summary: "no id"
    conditions:
      - kind: "metric_threshold"
        signal: "p99_latency_ms"
        op: ">"
        value: 1000
)");
    RootCauseAnalyzer rca(std::make_shared<PIIFilter>());
    EXPECT_FALSE(rca.reloadRules(y.path()));
}

// --- 4: schema — invalid path traversal rejected (SR-NEW1) -----------------

TEST(RootCauseAnalyzerTest, LoadPathTraversalRejected) {
    RootCauseAnalyzer rca(std::make_shared<PIIFilter>());
    EXPECT_FALSE(rca.reloadRules("../../etc/passwd"));
}

// --- 5: C2.1 condition kind = metric_threshold ------------------------------

TEST(RootCauseAnalyzerTest, MetricThresholdConditionMatches) {
    TempYaml y(R"(
rules:
  - id: "rca_metric"
    category: "internal"
    summary: "metric trigger"
    min_matched_conditions: 1
    score_when_all_matched: 0.9
    score_per_condition: 0.9
    conditions:
      - kind: "metric_threshold"
        signal: "p99_latency_ms"
        op: ">"
        value: 1000
        evidence_label: "p99 ${current} > ${expected}"
)");
    RootCauseAnalyzer rca(std::make_shared<PIIFilter>());
    ASSERT_TRUE(rca.reloadRules(y.path()));

    auto out = rca.analyze(makeSignals(/*p99=*/1500));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].rule_id, "rca_metric");
    EXPECT_NEAR(out[0].score, 0.9, 1e-9);
    ASSERT_EQ(out[0].evidence.size(), 1u);
    EXPECT_NE(out[0].evidence[0].label.find("1500"), std::string::npos);
}

// --- 6: C2.1 condition kind = feedback_event_count -------------------------

TEST(RootCauseAnalyzerTest, FeedbackEventCountConditionMatches) {
    TempYaml y(R"(
rules:
  - id: "rca_feedback"
    category: "upstream"
    summary: "feedback trigger"
    min_matched_conditions: 1
    score_when_all_matched: 0.8
    score_per_condition: 0.8
    conditions:
      - kind: "feedback_event_count"
        event_type: "OpsIncident"
        topic_match: ""
        window_seconds: 300
        count_threshold: 3
        evidence_label: "ops_incident=${count}"
)");
    RootCauseAnalyzer rca(std::make_shared<PIIFilter>());
    ASSERT_TRUE(rca.reloadRules(y.path()));

    std::vector<FeedbackEvent> events;
    for (int i = 0; i < 4; ++i) {
        events.push_back(makeFeedbackEvent(FeedbackEventType::OpsIncident,
                                             "ops.incident"));
    }
    auto out = rca.analyze(makeSignals(100, 0, std::move(events)));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].rule_id, "rca_feedback");
    EXPECT_NE(out[0].evidence[0].label.find("4"), std::string::npos);
}

// --- 7: C2.1 condition kind = log_pattern_count ----------------------------

TEST(RootCauseAnalyzerTest, LogPatternCountConditionMatches) {
    TempYaml y(R"(
rules:
  - id: "rca_log"
    category: "upstream"
    summary: "log trigger"
    min_matched_conditions: 1
    score_when_all_matched: 0.7
    score_per_condition: 0.7
    conditions:
      - kind: "log_pattern_count"
        pattern: "5\\d\\d Bad Gateway"
        window_seconds: 300
        count_threshold: 2
        evidence_label: "log_5xx=${count}"
)");
    RootCauseAnalyzer rca(std::make_shared<PIIFilter>());
    ASSERT_TRUE(rca.reloadRules(y.path()));

    std::vector<LogRingbufferSink::Entry> logs;
    logs.push_back(makeLogEntry("upstream returned 502 Bad Gateway"));
    logs.push_back(makeLogEntry("503 Bad Gateway from openai"));
    logs.push_back(makeLogEntry("normal info message"));
    auto out = rca.analyze(makeSignals(100, 0, {}, std::move(logs)));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].rule_id, "rca_log");
}

// --- 8: C2.2 scoreRule pure function ---------------------------------------

TEST(RootCauseAnalyzerTest, ScoreRulePureFunctionEnumerated) {
    Rule r;
    r.min_matched_conditions = 2;
    r.score_per_condition    = 0.3;
    r.score_when_all_matched = 0.9;

    // matched < min → score 0
    EXPECT_DOUBLE_EQ(scoreRule(r, /*matched_count=*/0), 0.0);
    EXPECT_DOUBLE_EQ(scoreRule(r, 1), 0.0);

    // matched == min → score = per * count = 0.6
    EXPECT_DOUBLE_EQ(scoreRule(r, 2), 0.6);

    // matched > min, not yet capped → 0.9 (capped by score_when_all_matched)
    EXPECT_DOUBLE_EQ(scoreRule(r, 3), 0.9);

    // matched far > → still capped at 0.9
    EXPECT_DOUBLE_EQ(scoreRule(r, 5), 0.9);
}

// --- 9: C2.2 OUTPUT_SCORE_THRESHOLD filter ---------------------------------

TEST(RootCauseAnalyzerTest, OutputScoreThresholdFiltersLowScores) {
    TempYaml y(R"(
rules:
  - id: "rca_low_signal"
    category: "internal"
    summary: "low score rule"
    min_matched_conditions: 1
    score_when_all_matched: 0.2
    score_per_condition: 0.2
    conditions:
      - kind: "metric_threshold"
        signal: "p99_latency_ms"
        op: ">"
        value: 100
        evidence_label: "low"
)");
    RootCauseAnalyzer rca(std::make_shared<PIIFilter>());
    ASSERT_TRUE(rca.reloadRules(y.path()));
    // Even though the rule matches, its score 0.2 is below the global
    // 0.3 threshold → no hypothesis emitted.
    auto out = rca.analyze(makeSignals(/*p99=*/200));
    EXPECT_TRUE(out.empty());
}

// --- 10: C2.2 multi-rule descending order ----------------------------------

TEST(RootCauseAnalyzerTest, AnalyzeReturnsHypothesesSortedByScoreDesc) {
    TempYaml y(R"(
rules:
  - id: "rca_lower"
    category: "internal"
    summary: "lower"
    min_matched_conditions: 1
    score_when_all_matched: 0.5
    score_per_condition: 0.5
    conditions:
      - kind: "metric_threshold"
        signal: "p99_latency_ms"
        op: ">"
        value: 1000
        evidence_label: "p99 ${current}"
  - id: "rca_higher"
    category: "internal"
    summary: "higher"
    min_matched_conditions: 1
    score_when_all_matched: 0.9
    score_per_condition: 0.9
    conditions:
      - kind: "metric_threshold"
        signal: "p99_latency_ms"
        op: ">"
        value: 1000
        evidence_label: "p99 ${current}"
)");
    RootCauseAnalyzer rca(std::make_shared<PIIFilter>());
    ASSERT_TRUE(rca.reloadRules(y.path()));
    auto out = rca.analyze(makeSignals(/*p99=*/1500));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].rule_id, "rca_higher");
    EXPECT_EQ(out[1].rule_id, "rca_lower");
}

// --- 11: C2.3 evidence PII mask defence-in-depth (M_RCA_evidence_mask) -----

TEST(RootCauseAnalyzerTest, EvidencePiiMaskDefenceInDepth) {
    TempYaml y(R"(
rules:
  - id: "rca_log"
    category: "internal"
    summary: "log w/ pii"
    min_matched_conditions: 1
    score_when_all_matched: 0.9
    score_per_condition: 0.9
    conditions:
      - kind: "log_pattern_count"
        pattern: "auth failed"
        window_seconds: 300
        count_threshold: 1
        evidence_label: "auth_failed=${count}"
)");
    RootCauseAnalyzer rca(std::make_shared<PIIFilter>());
    ASSERT_TRUE(rca.reloadRules(y.path()));

    // Hand-craft a (deliberately) un-masked log entry to simulate a
    // misbehaving sink. RCA must mask again before producing evidence.
    std::vector<LogRingbufferSink::Entry> logs;
    logs.push_back(makeLogEntry(
        "auth failed for user alice@example.com from 1.2.3.4"));
    auto out = rca.analyze(makeSignals(100, 0, {}, std::move(logs)));
    ASSERT_EQ(out.size(), 1u);
    for (const auto& ev : out[0].evidence) {
        EXPECT_EQ(ev.label.find("alice@example.com"), std::string::npos);
        EXPECT_EQ(ev.current_value.find("alice@example.com"),
                  std::string::npos);
    }
}

// --- 12: C2.4 min_matched_conditions boundary (= mode) ---------------------

TEST(RootCauseAnalyzerTest, MinMatchedConditionsBoundary) {
    TempYaml y(R"(
rules:
  - id: "rca_strict"
    category: "internal"
    summary: "must match BOTH"
    min_matched_conditions: 2
    score_when_all_matched: 0.9
    score_per_condition: 0.5
    conditions:
      - kind: "metric_threshold"
        signal: "p99_latency_ms"
        op: ">"
        value: 1000
        evidence_label: "p99"
      - kind: "metric_threshold"
        signal: "error_rate"
        op: ">"
        value: 0.05
        evidence_label: "err"
)");
    RootCauseAnalyzer rca(std::make_shared<PIIFilter>());
    ASSERT_TRUE(rca.reloadRules(y.path()));

    // Only metric matches → 1 < min_matched = 2 → no hypothesis.
    EXPECT_TRUE(rca.analyze(makeSignals(1500, 0.0)).empty());

    // Both match → hypothesis emitted with score = 0.9.
    auto out = rca.analyze(makeSignals(1500, 0.10));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_NEAR(out[0].score, 0.9, 1e-9);
}

// --- 13: findHypothesis helper ---------------------------------------------

TEST(RootCauseAnalyzerTest, FindHypothesisHelperByRuleId) {
    TempYaml y(R"(
rules:
  - id: "rca_a"
    category: "internal"
    summary: "a"
    min_matched_conditions: 1
    score_when_all_matched: 0.9
    score_per_condition: 0.9
    conditions:
      - kind: "metric_threshold"
        signal: "p99_latency_ms"
        op: ">"
        value: 1000
        evidence_label: "p99"
)");
    RootCauseAnalyzer rca(std::make_shared<PIIFilter>());
    ASSERT_TRUE(rca.reloadRules(y.path()));
    auto out = rca.analyze(makeSignals(1500));
    auto h = rca.findHypothesis(out, "rca_a");
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(h->rule_id, "rca_a");
    EXPECT_FALSE(rca.findHypothesis(out, "rca_unknown").has_value());
}

// --- 15: ship rca_rules_v1.yaml loads cleanly -------------------------------

TEST(RootCauseAnalyzerTest, ShippedRcaRulesV1Loads) {
    const fs::path repo_yaml = fs::path(__FILE__).parent_path() /
                                 ".." / ".." / ".." / ".." /
                                 "config" / "rca_rules" / "rca_rules_v1.yaml";
    if (!fs::exists(repo_yaml)) GTEST_SKIP() << repo_yaml.string();
    RootCauseAnalyzer rca(std::make_shared<PIIFilter>());
    ASSERT_TRUE(rca.reloadRules(repo_yaml.string())) << repo_yaml.string();
    EXPECT_GE(rca.health().first, 5u);
}

// --- 14: reloadRules retains old on failure ---------------------------------

TEST(RootCauseAnalyzerTest, ReloadFailureRetainsOldRules) {
    TempYaml good(R"(
rules:
  - id: "rca_good"
    category: "internal"
    summary: "good"
    min_matched_conditions: 1
    score_when_all_matched: 0.9
    score_per_condition: 0.9
    conditions:
      - kind: "metric_threshold"
        signal: "p99_latency_ms"
        op: ">"
        value: 1000
        evidence_label: "p99"
)");
    TempYaml bad(R"(this is not valid yaml structure for rca rules: at all
)");
    RootCauseAnalyzer rca(std::make_shared<PIIFilter>());
    ASSERT_TRUE(rca.reloadRules(good.path()));
    EXPECT_EQ(rca.health().first, 1u);
    EXPECT_FALSE(rca.reloadRules(bad.path()));
    EXPECT_EQ(rca.health().first, 1u) << "old rules retained";
}
