// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 6.
//
// End-to-end integration smoke for the v1 self-healing pipeline:
//
//   FeedbackBus + LogRingbufferSink → FeedbackEventHistory + spdlog
//                       │
//                       ▼
//                 RcaSignals
//                       │
//                       ▼
//                RootCauseAnalyzer.analyze()  →  hypothesis list
//                       │
//                       ▼
//                RunbookEngine.evaluate()      →  matches
//                       │
//                       ▼
//                buildProposal()               →  ApprovalProposal
//                       │
//                       ▼
//                RecoveryApplier.apply()       →  MLRouter mutated
//                                                 + AuditLogger entry
//
// Validates SR2 (audit), SR7 (RCA + runbook double signal), SR-NEW2 (PII
// mask), and the cooldown / approval_required flags propagate properly.

#include "aegisgate/feedback_event.h"
#include "common/clock.h"
#include "gateway/ml_router.h"
#include "guardrail/audit.h"
#include "guardrail/inbound/pii_filter.h"
#include "observe/feedback_bus.h"
#include "observe/recovery/feedback_event_history.h"
#include "observe/recovery/log_ringbuffer_sink.h"
#include "observe/recovery/recovery_applier.h"
#include "observe/recovery/root_cause_analyzer.h"
#include "observe/recovery/runbook_engine.h"
#include "observe/recovery/signal_snapshot.h"
#include "server/budget_guard_stage.h"

#include <gtest/gtest.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

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
                ("rca_int_" + std::to_string(rand()) + ".yaml");
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

class TempYamlDir {
public:
    TempYamlDir() {
        path_ = fs::temp_directory_path() /
                ("rb_int_" + std::to_string(rand()));
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

} // namespace

// --- 1: end-to-end flow ----------------------------------------------------

TEST(SelfHealingIntegrationTest, EndToEndDownsizeRunbookAppliesSuccessfully) {
    auto pii = std::make_shared<PIIFilter>();

    // RCA rule: high p99 ≥ 1500 OR ops_incident ≥ 2.
    TempYaml rca_yaml(R"(
rules:
  - id: "rca_high_p99_int"
    category: "internal"
    summary: "high p99"
    min_matched_conditions: 1
    score_when_all_matched: 0.9
    score_per_condition: 0.9
    conditions:
      - kind: "metric_threshold"
        signal: "p99_latency_ms"
        op: ">"
        value: 1500
        evidence_label: "p99=${current}"
      - kind: "feedback_event_count"
        event_type: "OpsIncident"
        topic_match: ""
        window_seconds: 600
        count_threshold: 2
        evidence_label: "ops=${count}"
)");
    RootCauseAnalyzer rca(pii);
    ASSERT_TRUE(rca.reloadRules(rca_yaml.path()));

    // Runbook: rca_required match → override_quality_tier (auto-approve
    // when low risk).
    TempYamlDir rb_dir;
    rb_dir.put("rb_int.yaml", R"(
id: "rb_int_downgrade"
description: "integration smoke runbook"
triggers:
  - kind: "rca_required"
    rca_rule_id: "rca_high_p99_int"
    rca_min_score: 0.7
actions:
  - action: "override_quality_tier"
    payload:
      tenant_id: "tenant-int"
      to_quality_tier: "economy"
      from_quality_tier: "standard"
      estimated_savings_usd_24h: 5.0
      affected_requests_per_hour: 100
approval_required: false
cooldown_seconds: 300
)");
    common::FakeClock clock(0);
    RunbookEngine eng(&clock);
    ASSERT_TRUE(eng.reloadRunbooks(rb_dir.path().string()));

    // Wire applier with shared MLRouter + AuditLogger + BudgetGuard.
    auto router = std::make_shared<MLRouter>();
    auto audit  = std::make_shared<AuditLogger>();
    BudgetGuardConfig bgcfg; bgcfg.enabled = true; bgcfg.per_tenant_24h_usd = 100;
    auto budget = std::make_shared<BudgetGuardStage>(nullptr, router, bgcfg);
    autonomy::RecoveryApplier applier({router, budget, audit});

    // Drive signals: high p99 → RCA hypothesis → runbook match.
    RcaSignals s;
    s.metrics_now.p99_latency_ms = 2000.0;
    s.metrics_now.error_rate = 0.0;
    s.metrics_now.sample_count = 1000;

    auto hyps = rca.analyze(s);
    ASSERT_GE(hyps.size(), 1u);
    EXPECT_GE(hyps[0].score, 0.7);

    auto matches = eng.evaluate(s, hyps);
    ASSERT_EQ(matches.size(), 1u) << "rb_int_downgrade should match";
    EXPECT_EQ(matches[0].runbook_id, "rb_int_downgrade");

    auto proposal = eng.buildProposal(matches[0]);
    EXPECT_FALSE(proposal.payload.value("approval_required", true));
    EXPECT_EQ(proposal.subject, "rb_int_downgrade");
    EXPECT_EQ(proposal.payload.value("action", std::string{}),
              "override_quality_tier");

    // SR2 — applier writes audit + mutates router.
    auto r = applier.apply(proposal, /*dry_run=*/false);
    ASSERT_TRUE(r.success) << r.error_code << ": " << r.error_message;
    EXPECT_EQ(router->getQualityTierOverride("tenant-int").value_or(""),
                "economy");
    auto entries = audit->entries();
    bool found_apply = false;
    for (const auto& e : entries) {
        if (e.action.rfind("auto_recovery.apply.", 0) == 0) {
            found_apply = true; break;
        }
    }
    EXPECT_TRUE(found_apply) << "SR2: audit chain must record apply event";

    // mark cooldown — second evaluate must skip even on identical signals.
    eng.markTriggered(matches[0].runbook_id, clock.nowMillis());
    auto matches2 = eng.evaluate(s, hyps);
    EXPECT_TRUE(matches2.empty()) << "cooldown must skip second hit";
}

// --- 2: PII mask defence-in-depth across the full pipeline (SR-NEW2) -------

TEST(SelfHealingIntegrationTest, PiiMaskedAcrossLogSinkAndRcaEvidence) {
    auto pii = std::make_shared<PIIFilter>();
    auto sink = std::make_shared<LogRingbufferSink>(pii, /*capacity=*/100);
    auto logger = std::make_shared<spdlog::logger>("int_test", sink);
    logger->set_level(spdlog::level::trace);

    // Log carrying email — must end up masked in the ringbuffer.
    logger->warn("auth failed for alice@example.com from 1.2.3.4");
    auto raw = sink->dumpAll();
    ASSERT_FALSE(raw.empty());
    EXPECT_EQ(raw[0].msg_masked.find("alice@example.com"), std::string::npos);

    // RCA evidence pulls from the (already masked) ringbuffer; defence-in-
    // depth: verify a mishaped raw entry is also masked at evidence time.
    TempYaml y(R"(
rules:
  - id: "rca_auth_fail"
    category: "internal"
    summary: "auth failed log"
    min_matched_conditions: 1
    score_when_all_matched: 0.9
    score_per_condition: 0.9
    conditions:
      - kind: "log_pattern_count"
        pattern: "auth failed"
        window_seconds: 600
        count_threshold: 1
        evidence_label: "auth=${count}"
)");
    RootCauseAnalyzer rca(pii);
    ASSERT_TRUE(rca.reloadRules(y.path()));

    // Inject a mishaped (unmasked) entry to simulate misbehaving sink.
    LogRingbufferSink::Entry tampered;
    tampered.ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    tampered.lvl = spdlog::level::warn;
    tampered.msg_masked = "auth failed for bob@evil.com";
    RcaSignals s;
    s.recent_log_entries.push_back(tampered);

    auto hyps = rca.analyze(s);
    ASSERT_EQ(hyps.size(), 1u);
    for (const auto& ev : hyps[0].evidence) {
        EXPECT_EQ(ev.label.find("bob@evil.com"), std::string::npos);
        EXPECT_EQ(ev.current_value.find("bob@evil.com"), std::string::npos);
    }
}
