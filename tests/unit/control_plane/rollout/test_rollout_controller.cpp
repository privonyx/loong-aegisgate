// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic B.6.
//
// Integration-style unit tests: MemoryPersistentStore + real ConfigServiceCore
// + real RolloutAuditBridge + FakeClock + FakeMetricsProvider.
//
// Coverage map (matches plan §B.6):
//   B.6.a manual lifecycle create/start/get/list     — 6 tests
//   B.6.b promote / pause / resume / abort           — 10 tests
//   B.6.c evaluateStage (observation + auto-pause)   — 10 tests
//   B.6.d evaluateAutoRollback + onTick              — 8 tests
//   Error-code discipline / quota / SR17 flag / tick exception isolation.

#include "common/clock.h"
#include "control_plane/config_service_core.h"
#include "control_plane/rollout/rollout_audit_bridge.h"
#include "control_plane/rollout/rollout_controller.h"
#include "control_plane/rollout/rollout_metrics_provider.h"
#include "guardrail/audit.h"
#include "storage/memory_persistent_store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

namespace aegisgate {
namespace {

// --- Fake metrics provider -------------------------------------------------

class FakeMetricsProvider : public RolloutMetricsProvider {
public:
    VersionMetrics forVersion(std::string_view version_id,
                               std::chrono::seconds /*window*/) override {
        const std::string key{version_id};
        auto it = table_.find(key);
        if (it == table_.end()) return VersionMetrics{};
        return it->second;
    }

    void set(const std::string& v, VersionMetrics m) { table_[v] = m; }
    void clear() { table_.clear(); }

private:
    std::unordered_map<std::string, VersionMetrics> table_;
};

// --- Test fixture ----------------------------------------------------------

class RolloutControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryPersistentStore>();
        ASSERT_TRUE(store_->initialize());
        audit_logger_ = std::make_unique<AuditLogger>();
        audit_logger_->setPersistentStore(store_.get());

        clock_ = std::make_unique<common::FakeClock>(1'700'000'000'000LL);

        ConfigServiceCore::Deps cdeps;
        cdeps.store = store_.get();
        cdeps.audit = audit_logger_.get();
        cdeps.clock = [this]() { return clock_->wallClockMillis(); };
        cdeps.validator = [](const std::string&) {
            return std::vector<Config::ValidationIssue>{};
        };
        config_core_ = std::make_unique<ConfigServiceCore>(std::move(cdeps));

        bridge_ = std::make_unique<RolloutAuditBridge>(audit_logger_.get());
        metrics_ = std::make_unique<FakeMetricsProvider>();

        RolloutController::Deps d;
        d.store = store_.get();
        d.config_core = config_core_.get();
        d.metrics = metrics_.get();
        d.audit = bridge_.get();
        d.clock = clock_.get();
        d.pause_cooldown_ms = 5 * 60 * 1000;
        d.system_user_id = "system.autorollback";
        controller_ = std::make_unique<RolloutController>(std::move(d));
    }

    // Creates + approves a config version, returning its version_id.
    std::string createApprovedVersion(const std::string& yaml = "v: 1\n") {
        auto sub = config_core_->submit(yaml, "alice", "", false);
        EXPECT_EQ(sub.error_code, "");
        auto app = config_core_->approve(sub.record.version_id, "bob", "");
        EXPECT_EQ(app.error_code, "");
        return sub.record.version_id;
    }

    std::string createApproveActivate(const std::string& yaml) {
        auto id = createApprovedVersion(yaml);
        auto act = config_core_->activate(id, "carol");
        EXPECT_EQ(act.error_code, "");
        return id;
    }

    // Minimal 2-stage spec targeting `target`.
    static RolloutSpec twoStageSpec(const std::string& target,
                                      int percentA = 10, int percentB = 100,
                                      int min_duration_s = 30,
                                      int min_samples = 10) {
        RolloutSpec s;
        s.target_version_id = target;
        RolloutStageRecord a;
        a.name = "canary";
        a.scope.percentage = percentA;
        a.observation.min_duration_seconds = min_duration_s;
        a.observation.min_sample_count = min_samples;
        a.auto_pause.absolute_error_rate_gt = 0.05;
        a.auto_pause.absolute_p99_latency_ms_gt = 500.0;
        s.stages.push_back(a);
        RolloutStageRecord b = a;
        b.name = "full";
        b.scope.percentage = percentB;
        s.stages.push_back(b);
        return s;
    }

    void advanceMs(std::int64_t ms) {
        clock_->advance(std::chrono::milliseconds(ms));
    }

    std::unique_ptr<MemoryPersistentStore> store_;
    std::unique_ptr<AuditLogger>           audit_logger_;
    std::unique_ptr<common::FakeClock>     clock_;
    std::unique_ptr<ConfigServiceCore>     config_core_;
    std::unique_ptr<RolloutAuditBridge>    bridge_;
    std::unique_ptr<FakeMetricsProvider>   metrics_;
    std::unique_ptr<RolloutController>     controller_;
};

// =========================================================================
// B.6.a — create / get / list / start
// =========================================================================

TEST_F(RolloutControllerTest, CreateRolloutHappyPath) {
    auto vid = createApprovedVersion();
    auto spec = twoStageSpec(vid);
    auto r = controller_->createRollout(spec, "alice");
    EXPECT_EQ(r.error_code, "");
    EXPECT_FALSE(r.record.rollout_id.empty());
    EXPECT_EQ(r.record.status, RolloutStatus::PENDING);
    EXPECT_EQ(r.record.target_version_id, vid);
    EXPECT_EQ(r.record.creator, "alice");
    EXPECT_EQ(r.record.current_stage_index, 0);
}

TEST_F(RolloutControllerTest, CreateRejectsEmptyTarget) {
    RolloutSpec s;
    RolloutStageRecord a; a.scope.percentage = 100;
    s.stages.push_back(a);
    auto r = controller_->createRollout(s, "alice");
    EXPECT_EQ(r.error_code, "INVALID_ARGUMENT");
}

TEST_F(RolloutControllerTest, CreateRejectsNoStages) {
    auto vid = createApprovedVersion();
    RolloutSpec s; s.target_version_id = vid;
    auto r = controller_->createRollout(s, "alice");
    EXPECT_EQ(r.error_code, "INVALID_ARGUMENT");
}

TEST_F(RolloutControllerTest, CreateRejectsUnknownVersion) {
    auto s = twoStageSpec("01VERDOESNOTEXIST000000000");
    auto r = controller_->createRollout(s, "alice");
    EXPECT_EQ(r.error_code, "NOT_FOUND");
}

TEST_F(RolloutControllerTest, CreateRejectsNonApprovedVersion) {
    auto sub = config_core_->submit("v: 1\n", "alice", "", false);
    ASSERT_EQ(sub.error_code, "");
    // PENDING, not approved
    auto s = twoStageSpec(sub.record.version_id);
    auto r = controller_->createRollout(s, "alice");
    EXPECT_EQ(r.error_code, "FAILED_PRECONDITION");
}

TEST_F(RolloutControllerTest, CreateRejectsDuplicateActiveRollout) {
    auto vid = createApprovedVersion();
    auto spec = twoStageSpec(vid);
    ASSERT_EQ(controller_->createRollout(spec, "alice").error_code, "");
    auto dup = controller_->createRollout(spec, "alice");
    EXPECT_EQ(dup.error_code, "ALREADY_EXISTS");
}

TEST_F(RolloutControllerTest, CreateRespectsTenantQuota) {
    RolloutController::Deps d;
    d.store = store_.get();
    d.config_core = config_core_.get();
    d.metrics = metrics_.get();
    d.audit = bridge_.get();
    d.clock = clock_.get();
    d.check_quota = [](const std::string& t) { return t != "blocked"; };
    RolloutController capped(std::move(d));

    auto vid = createApprovedVersion();
    auto s = twoStageSpec(vid);
    auto r = capped.createRollout(s, "blocked");
    EXPECT_EQ(r.error_code, "RESOURCE_EXHAUSTED");
}

TEST_F(RolloutControllerTest, StartRolloutMovesToProgressing) {
    auto vid = createApproveActivate("v: 1\n");  // active = vid
    auto newvid = createApprovedVersion("v: 2\n");
    auto spec = twoStageSpec(newvid);
    auto c = controller_->createRollout(spec, "alice");
    ASSERT_EQ(c.error_code, "");

    advanceMs(1000);
    auto s = controller_->startRollout(c.record.rollout_id, "alice");
    EXPECT_EQ(s.error_code, "");
    EXPECT_EQ(s.record.status, RolloutStatus::PROGRESSING);
    EXPECT_EQ(s.record.previous_active_version_id, vid);
    EXPECT_GT(s.record.started_at, 0);
    EXPECT_EQ(s.record.stage_started_at, s.record.started_at);
}

TEST_F(RolloutControllerTest, StartTwiceFails) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    auto again = controller_->startRollout(c.record.rollout_id, "alice");
    EXPECT_EQ(again.error_code, "FAILED_PRECONDITION");
}

TEST_F(RolloutControllerTest, GetRolloutReturnsStoredRecord) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    auto got = controller_->getRollout(c.record.rollout_id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->rollout_id, c.record.rollout_id);
    EXPECT_EQ(got->status, RolloutStatus::PENDING);
}

TEST_F(RolloutControllerTest, ListRolloutsFiltersByStatus) {
    auto v1 = createApprovedVersion("v: 1\n");
    auto v2 = createApprovedVersion("v: 2\n");
    auto c1 = controller_->createRollout(twoStageSpec(v1), "alice");
    ASSERT_EQ(c1.error_code, "");
    auto c2 = controller_->createRollout(twoStageSpec(v2), "alice");
    ASSERT_EQ(c2.error_code, "");
    // Start only c2.
    ASSERT_EQ(controller_->startRollout(c2.record.rollout_id, "alice").error_code, "");

    RolloutQuery q;
    q.statuses = {RolloutStatus::PROGRESSING};
    auto list = controller_->listRollouts(q);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].rollout_id, c2.record.rollout_id);

    q.statuses = {RolloutStatus::PENDING};
    list = controller_->listRollouts(q);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].rollout_id, c1.record.rollout_id);
}

// =========================================================================
// B.6.b — promote / pause / resume / abort
// =========================================================================

TEST_F(RolloutControllerTest, PromoteAdvancesToNextStage) {
    auto vid = createApprovedVersion();
    auto spec = twoStageSpec(vid);
    auto c = controller_->createRollout(spec, "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    advanceMs(60'000);
    auto p = controller_->promoteStage(c.record.rollout_id, "alice");
    EXPECT_EQ(p.error_code, "");
    EXPECT_EQ(p.record.status, RolloutStatus::PROGRESSING);
    EXPECT_EQ(p.record.current_stage_index, 1);
    EXPECT_EQ(p.record.stage_started_at, clock_->wallClockMillis());
}

TEST_F(RolloutControllerTest, PromoteLastStageCompletesAndActivatesTarget) {
    auto prev = createApproveActivate("v: 1\n");
    auto target = createApprovedVersion("v: 2\n");
    auto c = controller_->createRollout(twoStageSpec(target), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    // Advance through stage 0.
    ASSERT_EQ(controller_->promoteStage(c.record.rollout_id, "alice").error_code, "");
    // Promote from last stage → COMPLETED + activate target.
    auto p = controller_->promoteStage(c.record.rollout_id, "alice");
    EXPECT_EQ(p.error_code, "");
    EXPECT_EQ(p.record.status, RolloutStatus::COMPLETED);

    auto active = store_->getActiveConfig();
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->version_id, target);
    (void)prev;
}

TEST_F(RolloutControllerTest, PromoteFromPendingRejected) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    auto p = controller_->promoteStage(c.record.rollout_id, "alice");
    EXPECT_EQ(p.error_code, "FAILED_PRECONDITION");
}

TEST_F(RolloutControllerTest, PauseRolloutManualSetsPauseReason) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    auto pr = controller_->pauseRollout(c.record.rollout_id, "alice", "manual investigation");
    EXPECT_EQ(pr.error_code, "");
    EXPECT_EQ(pr.record.status, RolloutStatus::PAUSED);
    EXPECT_EQ(pr.record.pause_reason, PauseReason::MANUAL);
    EXPECT_EQ(pr.record.pause_detail, "manual investigation");
    EXPECT_GT(pr.record.paused_at, 0);
}

TEST_F(RolloutControllerTest, PauseFromPendingRejected) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    auto pr = controller_->pauseRollout(c.record.rollout_id, "alice", "nope");
    EXPECT_EQ(pr.error_code, "FAILED_PRECONDITION");
}

TEST_F(RolloutControllerTest, ResumeClearsPauseReasonButKeepsPausedAt) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    ASSERT_EQ(controller_->pauseRollout(c.record.rollout_id, "alice", "hmm").error_code, "");
    const auto paused_at_before = controller_->getRollout(c.record.rollout_id)->paused_at;

    advanceMs(30'000);
    auto rs = controller_->resumeRollout(c.record.rollout_id, "alice");
    EXPECT_EQ(rs.error_code, "");
    EXPECT_EQ(rs.record.status, RolloutStatus::PROGRESSING);
    EXPECT_EQ(rs.record.pause_reason, PauseReason::UNSPECIFIED);
    EXPECT_EQ(rs.record.paused_at, paused_at_before);  // retained as cooldown anchor
    EXPECT_EQ(rs.record.pause_detail, "");
}

TEST_F(RolloutControllerTest, ResumeFromProgressingRejected) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    auto rs = controller_->resumeRollout(c.record.rollout_id, "alice");
    EXPECT_EQ(rs.error_code, "FAILED_PRECONDITION");
}

TEST_F(RolloutControllerTest, AbortFromPendingSkipsRollback) {
    auto prev = createApproveActivate("v: 1\n");
    auto target = createApprovedVersion("v: 2\n");
    auto c = controller_->createRollout(twoStageSpec(target), "alice");
    ASSERT_EQ(c.error_code, "");
    auto a = controller_->abortRollout(c.record.rollout_id, "alice");
    EXPECT_EQ(a.error_code, "");
    EXPECT_EQ(a.record.status, RolloutStatus::ABORTED);

    auto active = store_->getActiveConfig();
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->version_id, prev);  // never switched
}

TEST_F(RolloutControllerTest, AbortProgressingRollsBackToPrevious) {
    auto prev = createApproveActivate("v: 1\n");
    auto target = createApprovedVersion("v: 2\n");
    auto c = controller_->createRollout(twoStageSpec(target), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    auto a = controller_->abortRollout(c.record.rollout_id, "alice");
    EXPECT_EQ(a.error_code, "");
    EXPECT_EQ(a.record.status, RolloutStatus::ABORTED);

    auto active = store_->getActiveConfig();
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->version_id, prev);
}

TEST_F(RolloutControllerTest, AbortTerminalRejected) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->abortRollout(c.record.rollout_id, "alice").error_code, "");
    auto again = controller_->abortRollout(c.record.rollout_id, "alice");
    EXPECT_EQ(again.error_code, "FAILED_PRECONDITION");
}

// =========================================================================
// B.6.c — evaluateStage (observation + auto-pause)
// =========================================================================

TEST_F(RolloutControllerTest, EvaluateStageNOPWhenNotProgressing) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    auto r = controller_->evaluateStage(c.record.rollout_id,
                                          clock_->nowMillis(),
                                          clock_->wallClockMillis());
    EXPECT_EQ(r.error_code, "");
    EXPECT_EQ(r.record.status, RolloutStatus::PENDING);  // unchanged
}

TEST_F(RolloutControllerTest, EvaluateStageBeforeMinDurationIsNOP) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    metrics_->set(vid, {1000, 0.5, 9999.0});  // wildly unhealthy but ignored

    advanceMs(5'000);  // below min_duration (30s)
    auto r = controller_->evaluateStage(c.record.rollout_id,
                                          clock_->nowMillis(),
                                          clock_->wallClockMillis());
    EXPECT_EQ(r.error_code, "");
    EXPECT_EQ(r.record.status, RolloutStatus::PROGRESSING);
}

TEST_F(RolloutControllerTest, EvaluateStageBelowMinSampleCountIsNOP) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    metrics_->set(vid, {5, 0.5, 9999.0});  // error rate high but only 5 samples

    advanceMs(60'000);
    auto r = controller_->evaluateStage(c.record.rollout_id,
                                          clock_->nowMillis(),
                                          clock_->wallClockMillis());
    EXPECT_EQ(r.record.status, RolloutStatus::PROGRESSING);
}

TEST_F(RolloutControllerTest, EvaluateStagePausesOnAbsoluteErrorRate) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    metrics_->set(vid, {1000, 0.10, 50.0});  // > 0.05 absolute

    advanceMs(60'000);
    auto r = controller_->evaluateStage(c.record.rollout_id,
                                          clock_->nowMillis(),
                                          clock_->wallClockMillis());
    EXPECT_EQ(r.error_code, "");
    EXPECT_EQ(r.record.status, RolloutStatus::PAUSED);
    EXPECT_EQ(r.record.pause_reason, PauseReason::ERROR_RATE);
}

TEST_F(RolloutControllerTest, EvaluateStagePausesOnAbsoluteP99) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    metrics_->set(vid, {1000, 0.001, 800.0});  // p99 > 500 absolute

    advanceMs(60'000);
    auto r = controller_->evaluateStage(c.record.rollout_id,
                                          clock_->nowMillis(),
                                          clock_->wallClockMillis());
    EXPECT_EQ(r.record.status, RolloutStatus::PAUSED);
    EXPECT_EQ(r.record.pause_reason, PauseReason::LATENCY_RATIO);
}

TEST_F(RolloutControllerTest, EvaluateStageHealthyStaysProgressing) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    metrics_->set(vid, {1000, 0.001, 50.0});  // well under all thresholds

    advanceMs(60'000);
    auto r = controller_->evaluateStage(c.record.rollout_id,
                                          clock_->nowMillis(),
                                          clock_->wallClockMillis());
    EXPECT_EQ(r.record.status, RolloutStatus::PROGRESSING);
}

TEST_F(RolloutControllerTest, EvaluateStageRelativeErrorVsBaseline) {
    auto prev = createApproveActivate("v: 1\n");
    auto target = createApprovedVersion("v: 2\n");
    RolloutSpec s = twoStageSpec(target);
    s.stages[0].auto_pause.error_rate_gt = 0.02;             // relative
    s.stages[0].auto_pause.absolute_error_rate_gt = 0.99;    // effectively off
    s.stages[0].auto_pause.absolute_p99_latency_ms_gt = 9e9; // effectively off
    auto c = controller_->createRollout(s, "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");

    metrics_->set(prev,   {1000, 0.01, 50.0});
    metrics_->set(target, {1000, 0.05, 60.0});  // delta = 0.04 > 0.02 threshold

    advanceMs(60'000);
    auto r = controller_->evaluateStage(c.record.rollout_id,
                                          clock_->nowMillis(),
                                          clock_->wallClockMillis());
    EXPECT_EQ(r.record.status, RolloutStatus::PAUSED);
    EXPECT_EQ(r.record.pause_reason, PauseReason::ERROR_RATE);
}

TEST_F(RolloutControllerTest, EvaluateStageRelativeLatencyRatio) {
    auto prev = createApproveActivate("v: 1\n");
    auto target = createApprovedVersion("v: 2\n");
    RolloutSpec s = twoStageSpec(target);
    s.stages[0].auto_pause.p99_latency_ratio_gt = 1.5;        // target/baseline
    s.stages[0].auto_pause.absolute_error_rate_gt = 0.99;     // effectively off
    s.stages[0].auto_pause.absolute_p99_latency_ms_gt = 9e9;  // effectively off
    auto c = controller_->createRollout(s, "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");

    metrics_->set(prev,   {1000, 0.001, 100.0});
    metrics_->set(target, {1000, 0.001, 200.0});  // ratio = 2.0 > 1.5

    advanceMs(60'000);
    auto r = controller_->evaluateStage(c.record.rollout_id,
                                          clock_->nowMillis(),
                                          clock_->wallClockMillis());
    EXPECT_EQ(r.record.status, RolloutStatus::PAUSED);
    EXPECT_EQ(r.record.pause_reason, PauseReason::LATENCY_RATIO);
}

TEST_F(RolloutControllerTest, EvaluateStageCooldownSkipsRepeatedAutoPause) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    metrics_->set(vid, {1000, 0.10, 50.0});
    advanceMs(60'000);
    ASSERT_EQ(controller_->evaluateStage(c.record.rollout_id,
                                            clock_->nowMillis(),
                                            clock_->wallClockMillis()).record.status,
               RolloutStatus::PAUSED);
    ASSERT_EQ(controller_->resumeRollout(c.record.rollout_id, "alice").error_code, "");
    // Paused-at timestamp is retained; any evaluation inside the cooldown
    // window must NOT re-trigger auto-pause, even with breach.
    advanceMs(60'000);  // still < 5 min cooldown
    metrics_->set(vid, {1000, 0.10, 50.0});
    auto r = controller_->evaluateStage(c.record.rollout_id,
                                          clock_->nowMillis(),
                                          clock_->wallClockMillis());
    EXPECT_EQ(r.record.status, RolloutStatus::PROGRESSING);
}

TEST_F(RolloutControllerTest, EvaluateStageAfterCooldownPausesAgain) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    metrics_->set(vid, {1000, 0.10, 50.0});
    advanceMs(60'000);
    ASSERT_EQ(controller_->evaluateStage(c.record.rollout_id,
                                            clock_->nowMillis(),
                                            clock_->wallClockMillis()).record.status,
               RolloutStatus::PAUSED);
    ASSERT_EQ(controller_->resumeRollout(c.record.rollout_id, "alice").error_code, "");
    advanceMs(6 * 60 * 1000);  // beyond cooldown
    metrics_->set(vid, {1000, 0.10, 50.0});
    auto r = controller_->evaluateStage(c.record.rollout_id,
                                          clock_->nowMillis(),
                                          clock_->wallClockMillis());
    EXPECT_EQ(r.record.status, RolloutStatus::PAUSED);
}

// =========================================================================
// B.6.d — evaluateAutoRollback + onTick
// =========================================================================

TEST_F(RolloutControllerTest, EvaluateAutoRollbackNOPWhenNotPaused) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    auto r = controller_->evaluateAutoRollback(c.record.rollout_id,
                                                 clock_->nowMillis(),
                                                 clock_->wallClockMillis());
    EXPECT_EQ(r.record.status, RolloutStatus::PENDING);
}

TEST_F(RolloutControllerTest, EvaluateAutoRollbackBeforeGraceIsNOP) {
    auto prev = createApproveActivate("v: 1\n");
    auto target = createApprovedVersion("v: 2\n");
    RolloutSpec s = twoStageSpec(target);
    s.auto_rollback_grace_seconds = 1800;
    auto c = controller_->createRollout(s, "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    metrics_->set(target, {1000, 0.10, 50.0});
    advanceMs(60'000);
    ASSERT_EQ(controller_->evaluateStage(c.record.rollout_id,
                                            clock_->nowMillis(),
                                            clock_->wallClockMillis()).record.status,
               RolloutStatus::PAUSED);

    advanceMs(60'000);  // 1 min into 30 min grace
    auto r = controller_->evaluateAutoRollback(c.record.rollout_id,
                                                 clock_->nowMillis(),
                                                 clock_->wallClockMillis());
    EXPECT_EQ(r.record.status, RolloutStatus::PAUSED);
    (void)prev;
}

TEST_F(RolloutControllerTest, EvaluateAutoRollbackAfterGraceTriggers) {
    auto prev = createApproveActivate("v: 1\n");
    auto target = createApprovedVersion("v: 2\n");
    RolloutSpec s = twoStageSpec(target);
    s.auto_rollback_grace_seconds = 60;  // short for test
    auto c = controller_->createRollout(s, "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    metrics_->set(target, {1000, 0.10, 50.0});
    advanceMs(60'000);
    ASSERT_EQ(controller_->evaluateStage(c.record.rollout_id,
                                            clock_->nowMillis(),
                                            clock_->wallClockMillis()).record.status,
               RolloutStatus::PAUSED);

    advanceMs(120'000);  // beyond grace
    auto r = controller_->evaluateAutoRollback(c.record.rollout_id,
                                                 clock_->nowMillis(),
                                                 clock_->wallClockMillis());
    EXPECT_EQ(r.error_code, "");
    EXPECT_EQ(r.record.status, RolloutStatus::FAILED);
    EXPECT_EQ(r.record.pause_reason, PauseReason::AUTO_ROLLBACK);

    // Active version should have been rolled back to prev.
    auto active = store_->getActiveConfig();
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->version_id, prev);
}

TEST_F(RolloutControllerTest, EvaluateAutoRollbackRespectsSpecOptOut) {
    auto prev = createApproveActivate("v: 1\n");
    auto target = createApprovedVersion("v: 2\n");
    RolloutSpec s = twoStageSpec(target);
    s.auto_rollback_on_pause = false;
    s.auto_rollback_grace_seconds = 10;
    auto c = controller_->createRollout(s, "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    metrics_->set(target, {1000, 0.10, 50.0});
    advanceMs(60'000);
    ASSERT_EQ(controller_->evaluateStage(c.record.rollout_id,
                                            clock_->nowMillis(),
                                            clock_->wallClockMillis()).record.status,
               RolloutStatus::PAUSED);
    advanceMs(60'000);
    auto r = controller_->evaluateAutoRollback(c.record.rollout_id,
                                                 clock_->nowMillis(),
                                                 clock_->wallClockMillis());
    EXPECT_EQ(r.record.status, RolloutStatus::PAUSED);  // opt-out honored
    (void)prev;
}

TEST_F(RolloutControllerTest, EvaluateAutoRollbackRespectsSR17EnvSwitch) {
    auto prev = createApproveActivate("v: 1\n");
    auto target = createApprovedVersion("v: 2\n");

    RolloutController::Deps d;
    d.store = store_.get();
    d.config_core = config_core_.get();
    d.metrics = metrics_.get();
    d.audit = bridge_.get();
    d.clock = clock_.get();
    d.auto_rollback_enabled = []() { return false; };  // SR17 off
    RolloutController gated(std::move(d));

    RolloutSpec s = twoStageSpec(target);
    s.auto_rollback_grace_seconds = 10;
    auto c = gated.createRollout(s, "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(gated.startRollout(c.record.rollout_id, "alice").error_code, "");
    metrics_->set(target, {1000, 0.10, 50.0});
    advanceMs(60'000);
    ASSERT_EQ(gated.evaluateStage(c.record.rollout_id,
                                     clock_->nowMillis(),
                                     clock_->wallClockMillis()).record.status,
               RolloutStatus::PAUSED);
    advanceMs(60'000);
    auto r = gated.evaluateAutoRollback(c.record.rollout_id,
                                           clock_->nowMillis(),
                                           clock_->wallClockMillis());
    EXPECT_EQ(r.record.status, RolloutStatus::PAUSED);  // gated off
    (void)prev;
}

TEST_F(RolloutControllerTest, OnTickDrivesBothProgressingAndPaused) {
    auto prev = createApproveActivate("v: 1\n");
    auto target1 = createApprovedVersion("v: 2\n");
    auto target2 = createApprovedVersion("v: 3\n");

    // r1: PROGRESSING → will auto-pause
    auto c1 = controller_->createRollout(twoStageSpec(target1), "alice");
    ASSERT_EQ(c1.error_code, "");
    ASSERT_EQ(controller_->startRollout(c1.record.rollout_id, "alice").error_code, "");

    // r2: PAUSED manually → will auto-rollback after grace
    RolloutSpec s2 = twoStageSpec(target2);
    s2.auto_rollback_grace_seconds = 30;
    auto c2 = controller_->createRollout(s2, "alice");
    ASSERT_EQ(c2.error_code, "");
    ASSERT_EQ(controller_->startRollout(c2.record.rollout_id, "alice").error_code, "");
    ASSERT_EQ(controller_->pauseRollout(c2.record.rollout_id, "alice", "manual").error_code, "");

    metrics_->set(target1, {1000, 0.10, 50.0});
    advanceMs(60'000);

    controller_->onTick(clock_->nowMillis(), clock_->wallClockMillis());
    auto g1 = controller_->getRollout(c1.record.rollout_id);
    auto g2 = controller_->getRollout(c2.record.rollout_id);
    ASSERT_TRUE(g1 && g2);
    EXPECT_EQ(g1->status, RolloutStatus::PAUSED);  // evaluated by stage path
    EXPECT_EQ(g2->status, RolloutStatus::FAILED);  // grace expired in 60s
    (void)prev;
}

TEST_F(RolloutControllerTest, OnTickSkipsTerminalRollouts) {
    auto vid = createApprovedVersion();
    auto c = controller_->createRollout(twoStageSpec(vid), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->abortRollout(c.record.rollout_id, "alice").error_code, "");
    // Should run without touching terminal rollout.
    controller_->onTick(clock_->nowMillis(), clock_->wallClockMillis());
    auto got = controller_->getRollout(c.record.rollout_id);
    ASSERT_TRUE(got);
    EXPECT_EQ(got->status, RolloutStatus::ABORTED);
}

TEST_F(RolloutControllerTest, AuditChainVerifiesAfterFullLifecycle) {
    auto prev = createApproveActivate("v: 1\n");
    auto target = createApprovedVersion("v: 2\n");
    auto c = controller_->createRollout(twoStageSpec(target), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    advanceMs(60'000);
    ASSERT_EQ(controller_->promoteStage(c.record.rollout_id, "alice").error_code, "");
    advanceMs(60'000);
    ASSERT_EQ(controller_->promoteStage(c.record.rollout_id, "alice").error_code, "");

    ASSERT_TRUE(audit_logger_->flush(std::chrono::seconds{2}));
    EXPECT_TRUE(audit_logger_->verifyChain());
    (void)prev;
}

// =========================================================================
// TASK-20260703-02 Epic 3 / C7 — promote 原子性（D1=方案 C：activate-first +
// onTick 幂等对账）。根因：promote 先 activate（config→ACTIVE）再 updateRollout；
// 二者跨存储非原子。activate 成功而 updateRollout 失败 → config ACTIVE 但 rollout
// 永停 PROGRESSING。对账：onTick 检测 config 活跃版本 == target 且 rollout 未终态
// → 幂等补齐 COMPLETED。
// =========================================================================

TEST_F(RolloutControllerTest, OnTickReconcilesActivatedButProgressing) {
    auto prev = createApproveActivate("v: 1\n");
    auto target = createApprovedVersion("v: 2\n");
    auto c = controller_->createRollout(twoStageSpec(target), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    ASSERT_EQ(controller_->getRollout(c.record.rollout_id)->status,
              RolloutStatus::PROGRESSING);

    // 模拟 promote 的 activate 已成功、updateRollout 失败：config 活跃版本 = target，
    // 但 rollout 仍 PROGRESSING（C7 非原子缺口）。
    ASSERT_EQ(config_core_->activate(target, "carol").error_code, "");
    ASSERT_EQ(controller_->getRollout(c.record.rollout_id)->status,
              RolloutStatus::PROGRESSING);

    controller_->onTick(clock_->nowMillis(), clock_->wallClockMillis());
    auto got = controller_->getRollout(c.record.rollout_id);
    ASSERT_TRUE(got);
    EXPECT_EQ(got->status, RolloutStatus::COMPLETED);
    EXPECT_GT(got->completed_at, 0);
    (void)prev;
}

TEST_F(RolloutControllerTest, OnTickReconcileIsIdempotent) {
    auto prev = createApproveActivate("v: 1\n");
    auto target = createApprovedVersion("v: 2\n");
    auto c = controller_->createRollout(twoStageSpec(target), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");
    ASSERT_EQ(config_core_->activate(target, "carol").error_code, "");

    controller_->onTick(clock_->nowMillis(), clock_->wallClockMillis());
    auto first = controller_->getRollout(c.record.rollout_id);
    ASSERT_TRUE(first);
    ASSERT_EQ(first->status, RolloutStatus::COMPLETED);
    const auto completed_at1 = first->completed_at;

    // 再 tick：终态不再被 listRollouts 拾取 → 无副作用。
    controller_->onTick(clock_->nowMillis(), clock_->wallClockMillis());
    auto second = controller_->getRollout(c.record.rollout_id);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->status, RolloutStatus::COMPLETED);
    EXPECT_EQ(second->completed_at, completed_at1);
    (void)prev;
}

// 健康 PROGRESSING（config 活跃版本 != target）不得被对账误完成。
TEST_F(RolloutControllerTest, OnTickDoesNotReconcileHealthyProgressing) {
    auto prev = createApproveActivate("v: 1\n");
    auto target = createApprovedVersion("v: 2\n");
    auto c = controller_->createRollout(twoStageSpec(target), "alice");
    ASSERT_EQ(c.error_code, "");
    ASSERT_EQ(controller_->startRollout(c.record.rollout_id, "alice").error_code, "");

    // 未 activate target（config 仍 = prev）→ onTick 不得完成 rollout。
    controller_->onTick(clock_->nowMillis(), clock_->wallClockMillis());
    auto got = controller_->getRollout(c.record.rollout_id);
    ASSERT_TRUE(got);
    EXPECT_NE(got->status, RolloutStatus::COMPLETED);
    (void)prev;
}

}  // namespace
}  // namespace aegisgate
