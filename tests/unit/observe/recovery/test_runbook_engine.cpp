// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 5.
//
// RunbookEngine tests (13).

#include "aegisgate/feedback_event.h"
#include "common/clock.h"
#include "guardrail/inbound/pii_filter.h"
#include "observe/recovery/log_ringbuffer_sink.h"
#include "observe/recovery/root_cause_analyzer.h"
#include "observe/recovery/runbook_engine.h"
#include "observe/recovery/signal_snapshot.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>

using namespace aegisgate;

namespace {

namespace fs = std::filesystem;

class TempYamlDir {
public:
    TempYamlDir() {
        path_ = fs::temp_directory_path() /
                ("rb_test_" + std::to_string(rand()));
        fs::create_directories(path_);
    }
    ~TempYamlDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    fs::path path() const { return path_; }
    void put(const std::string& name, const std::string& body) {
        std::ofstream(path_ / name) << body;
    }

private:
    fs::path path_;
};

RcaSignals makeSignals(double p99 = 100, double err = 0,
                         std::vector<FeedbackEvent> events = {}) {
    RcaSignals s;
    s.metrics_now.p99_latency_ms = p99;
    s.metrics_now.error_rate = err;
    s.metrics_now.sample_count = 1000;
    s.recent_feedback_events = std::move(events);
    return s;
}

FeedbackEvent makeEvent(FeedbackEventType t, const std::string& topic) {
    FeedbackEvent e;
    e.type = t;
    e.topic = topic;
    e.timestamp = std::chrono::system_clock::now();
    return e;
}

} // namespace

// --- 1: schema valid -------------------------------------------------------

TEST(RunbookEngineTest, RunbookSchemaValidLoadsSucceeds) {
    TempYamlDir d;
    d.put("rb_a.yaml", R"(
id: "rb_a"
description: "test"
triggers:
  - kind: "metric"
    signal: "p99_latency_ms"
    op: ">"
    value: 1000
    hold_seconds: 60
actions:
  - action: "audit_only"
    payload:
      note: "test"
approval_required: true
cooldown_seconds: 300
)");
    common::FakeClock clock(0);
    RunbookEngine eng(&clock);
    EXPECT_TRUE(eng.reloadRunbooks(d.path().string()));
    EXPECT_EQ(eng.runbookCount(), 1u);
}

// --- 2: schema invalid -----------------------------------------------------

TEST(RunbookEngineTest, RunbookSchemaInvalidRejected) {
    TempYamlDir d;
    d.put("rb_bad.yaml", R"(
no_id_field: true
)");
    common::FakeClock clock(0);
    RunbookEngine eng(&clock);
    EXPECT_FALSE(eng.reloadRunbooks(d.path().string()));
}

// --- 3: SR-NEW1 path traversal rejected ------------------------------------

TEST(RunbookEngineTest, PathTraversalRejected) {
    common::FakeClock clock(0);
    RunbookEngine eng(&clock);
    EXPECT_FALSE(eng.reloadRunbooks("../../etc"));
}

// --- 4: metric trigger ----------------------------------------------------

TEST(RunbookEngineTest, TriggerMetricMatches) {
    TempYamlDir d;
    d.put("rb_metric.yaml", R"(
id: "rb_metric"
triggers:
  - kind: "metric"
    signal: "p99_latency_ms"
    op: ">"
    value: 1000
actions:
  - action: "audit_only"
    payload: {}
)");
    common::FakeClock clock(0);
    RunbookEngine eng(&clock);
    ASSERT_TRUE(eng.reloadRunbooks(d.path().string()));

    auto out = eng.evaluate(makeSignals(/*p99=*/1500), {});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].runbook_id, "rb_metric");
}

// --- 5: feedback trigger ---------------------------------------------------

TEST(RunbookEngineTest, TriggerFeedbackEventCountMatches) {
    TempYamlDir d;
    d.put("rb_fb.yaml", R"(
id: "rb_fb"
triggers:
  - kind: "feedback_event_count"
    event_type: "OpsIncident"
    window_seconds: 300
    count_threshold: 2
actions:
  - action: "audit_only"
    payload: {}
)");
    common::FakeClock clock(0);
    RunbookEngine eng(&clock);
    ASSERT_TRUE(eng.reloadRunbooks(d.path().string()));

    std::vector<FeedbackEvent> events;
    events.push_back(makeEvent(FeedbackEventType::OpsIncident, "ops.incident"));
    events.push_back(makeEvent(FeedbackEventType::OpsIncident, "ops.incident"));
    auto out = eng.evaluate(makeSignals(100, 0, events), {});
    ASSERT_EQ(out.size(), 1u);
}

// --- 6: rca_required matches when score sufficient (C2.5) ------------------

TEST(RunbookEngineTest, TriggerRcaRequiredMatchesWhenScoreSufficient) {
    TempYamlDir d;
    d.put("rb_rca.yaml", R"(
id: "rb_rca"
triggers:
  - kind: "rca_required"
    rca_rule_id: "rca_high_p99_internal"
    rca_min_score: 0.6
actions:
  - action: "audit_only"
    payload: {}
)");
    common::FakeClock clock(0);
    RunbookEngine eng(&clock);
    ASSERT_TRUE(eng.reloadRunbooks(d.path().string()));

    std::vector<RootCauseHypothesis> rca_results;
    RootCauseHypothesis h;
    h.rule_id = "rca_high_p99_internal";
    h.score = 0.85;
    rca_results.push_back(h);

    auto out = eng.evaluate(makeSignals(), rca_results);
    ASSERT_EQ(out.size(), 1u);
}

// --- 7: rca_required rejects when score insufficient -----------------------

TEST(RunbookEngineTest, TriggerRcaRequiredRejectsWhenScoreInsufficient) {
    TempYamlDir d;
    d.put("rb_rca2.yaml", R"(
id: "rb_rca2"
triggers:
  - kind: "rca_required"
    rca_rule_id: "rca_high_p99_internal"
    rca_min_score: 0.8
actions:
  - action: "audit_only"
    payload: {}
)");
    common::FakeClock clock(0);
    RunbookEngine eng(&clock);
    ASSERT_TRUE(eng.reloadRunbooks(d.path().string()));

    std::vector<RootCauseHypothesis> rca_results;
    RootCauseHypothesis h;
    h.rule_id = "rca_high_p99_internal";
    h.score = 0.5;  // below 0.8 threshold
    rca_results.push_back(h);

    EXPECT_TRUE(eng.evaluate(makeSignals(), rca_results).empty());
}

// --- 8: evaluate returns triggered ids -------------------------------------

TEST(RunbookEngineTest, EvaluateReturnsMultipleTriggered) {
    TempYamlDir d;
    d.put("rb_one.yaml", R"(
id: "rb_one"
triggers:
  - kind: "metric"
    signal: "p99_latency_ms"
    op: ">"
    value: 1000
actions:
  - action: "audit_only"
    payload: {}
)");
    d.put("rb_two.yaml", R"(
id: "rb_two"
triggers:
  - kind: "metric"
    signal: "error_rate"
    op: ">"
    value: 0.05
actions:
  - action: "audit_only"
    payload: {}
)");
    common::FakeClock clock(0);
    RunbookEngine eng(&clock);
    ASSERT_TRUE(eng.reloadRunbooks(d.path().string()));

    auto out = eng.evaluate(makeSignals(1500, 0.1), {});
    EXPECT_EQ(out.size(), 2u);
}

// --- 9: cooldown skips recently triggered (M3 mutation target) -------------

TEST(RunbookEngineTest, CooldownSkipsRecentlyTriggeredRunbook) {
    TempYamlDir d;
    d.put("rb_cool.yaml", R"(
id: "rb_cool"
triggers:
  - kind: "metric"
    signal: "p99_latency_ms"
    op: ">"
    value: 1000
actions:
  - action: "audit_only"
    payload: {}
cooldown_seconds: 300
)");
    common::FakeClock clock(0);
    RunbookEngine eng(&clock);
    ASSERT_TRUE(eng.reloadRunbooks(d.path().string()));

    auto out1 = eng.evaluate(makeSignals(1500), {});
    ASSERT_EQ(out1.size(), 1u);
    eng.markTriggered("rb_cool", clock.nowMillis());

    // Within cooldown window — must skip.
    clock.advance(std::chrono::minutes(2));
    auto out2 = eng.evaluate(makeSignals(1500), {});
    EXPECT_TRUE(out2.empty()) << "M3 mutation target: cooldown gate";
}

// --- 10: approval_required propagates to proposal (M7 mutation target) -----

TEST(RunbookEngineTest, ApprovalRequiredFlagPropagatesToProposal) {
    TempYamlDir d;
    d.put("rb_appr.yaml", R"(
id: "rb_appr"
triggers:
  - kind: "metric"
    signal: "p99_latency_ms"
    op: ">"
    value: 1000
actions:
  - action: "override_quality_tier"
    payload:
      tenant_id: "tenant-A"
      to_quality_tier: "economy"
approval_required: true
)");
    common::FakeClock clock(0);
    RunbookEngine eng(&clock);
    ASSERT_TRUE(eng.reloadRunbooks(d.path().string()));
    auto matches = eng.evaluate(makeSignals(1500), {});
    ASSERT_EQ(matches.size(), 1u);
    auto p = eng.buildProposal(matches[0]);
    EXPECT_TRUE(p.payload.value("approval_required", false))
        << "M7 mutation target: approval_required flag must be honoured";
}

// --- 11: approval_not_required => not flagged ------------------------------

TEST(RunbookEngineTest, ApprovalNotRequiredFlagFalse) {
    TempYamlDir d;
    d.put("rb_no_appr.yaml", R"(
id: "rb_no_appr"
triggers:
  - kind: "metric"
    signal: "p99_latency_ms"
    op: ">"
    value: 1000
actions:
  - action: "audit_only"
    payload: {}
approval_required: false
)");
    common::FakeClock clock(0);
    RunbookEngine eng(&clock);
    ASSERT_TRUE(eng.reloadRunbooks(d.path().string()));
    auto matches = eng.evaluate(makeSignals(1500), {});
    ASSERT_EQ(matches.size(), 1u);
    auto p = eng.buildProposal(matches[0]);
    EXPECT_FALSE(p.payload.value("approval_required", true));
}

// --- 12: buildProposal carries action payload ------------------------------

TEST(RunbookEngineTest, BuildProposalAttachesPayload) {
    TempYamlDir d;
    d.put("rb_payload.yaml", R"(
id: "rb_payload"
triggers:
  - kind: "metric"
    signal: "p99_latency_ms"
    op: ">"
    value: 1000
actions:
  - action: "override_quality_tier"
    payload:
      tenant_id: "tenant-Z"
      to_quality_tier: "economy"
)");
    common::FakeClock clock(0);
    RunbookEngine eng(&clock);
    ASSERT_TRUE(eng.reloadRunbooks(d.path().string()));
    auto matches = eng.evaluate(makeSignals(1500), {});
    ASSERT_EQ(matches.size(), 1u);
    auto p = eng.buildProposal(matches[0]);
    ASSERT_TRUE(p.payload.contains("actions"));
    ASSERT_EQ(p.payload["actions"].size(), 1u);
    EXPECT_EQ(p.payload["actions"][0].value("action", std::string{}),
              "override_quality_tier");
    EXPECT_EQ(p.payload["actions"][0]["payload"].value("tenant_id",
                                                          std::string{}),
              "tenant-Z");
}

// --- 13: shipped 5 runbooks load -------------------------------------------

TEST(RunbookEngineTest, ShippedRunbooksLoad) {
    const fs::path repo_dir = fs::path(__FILE__).parent_path() /
                                ".." / ".." / ".." / ".." /
                                "config" / "runbooks";
    if (!fs::exists(repo_dir)) GTEST_SKIP() << repo_dir.string();
    common::FakeClock clock(0);
    RunbookEngine eng(&clock);
    ASSERT_TRUE(eng.reloadRunbooks(repo_dir.string()));
    EXPECT_GE(eng.runbookCount(), 5u);
}
