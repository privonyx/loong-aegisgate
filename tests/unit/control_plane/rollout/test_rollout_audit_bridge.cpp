// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic B.5.
//
// RolloutAuditBridge writes one AuditEntry per lifecycle action into the
// shared control-plane AuditLogger chain. Invariants exercised here:
//   * Each record* method produces exactly one entry with the correct action.
//   * All entries land on stage_name="control_plane", tenant_id="system".
//   * AuditLogger::verifyChain() passes across the full 11-action timeline.
//   * SR14 no-leak: RolloutSpec.yaml-like sensitive fields never appear.
//   * nullptr AuditLogger → every method is a no-op; no crash.

#include "control_plane/rollout/rollout_audit_bridge.h"
#include "guardrail/audit.h"
#include "storage/memory_persistent_store.h"

#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <string>

namespace aegisgate {
namespace {

class RolloutAuditBridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryPersistentStore>();
        ASSERT_TRUE(store_->initialize());
        audit_ = std::make_unique<AuditLogger>();
        audit_->setPersistentStore(store_.get());
        bridge_ = std::make_unique<RolloutAuditBridge>(audit_.get());
    }

    void flush() {
        ASSERT_TRUE(audit_->flush(std::chrono::seconds{1}));
    }

    RolloutRecord makeRecord() const {
        RolloutRecord r;
        r.rollout_id = "01RL0000000000000000000001";
        r.target_version_id = "01VER00000000000000000NEW";
        r.previous_active_version_id = "01VER00000000000000000OLD";
        r.creator = "alice";
        r.status = RolloutStatus::PROGRESSING;
        r.current_stage_index = 1;
        r.started_at = 1'700'000'000'000LL;
        r.stage_started_at = 1'700'000'100'000LL;
        r.paused_at = 1'700'000'200'000LL;
        r.pause_reason = PauseReason::ERROR_RATE;
        r.completed_at = 1'700'000'300'000LL;
        return r;
    }

    std::unique_ptr<MemoryPersistentStore> store_;
    std::unique_ptr<AuditLogger>           audit_;
    std::unique_ptr<RolloutAuditBridge>    bridge_;
};

// --- Each of 11 actions emits exactly one entry with the correct action. --

TEST_F(RolloutAuditBridgeTest, RecordCreatedEmitsOneEntry) {
    bridge_->recordCreated(makeRecord(), "alice");
    flush();
    auto es = audit_->entries();
    ASSERT_EQ(es.size(), 1u);
    EXPECT_EQ(es[0].action,     "rollout.created");
    EXPECT_EQ(es[0].stage_name, "control_plane");
    EXPECT_EQ(es[0].tenant_id,  "system");
    EXPECT_NE(es[0].detail.find("01RL0000000000000000000001"), std::string::npos);
    EXPECT_NE(es[0].detail.find("\"actor\":\"alice\""), std::string::npos);
}

TEST_F(RolloutAuditBridgeTest, RecordStartedCarriesPreviousActive) {
    bridge_->recordStarted(makeRecord(), "alice");
    flush();
    auto es = audit_->entries();
    ASSERT_EQ(es.size(), 1u);
    EXPECT_EQ(es[0].action, "rollout.started");
    EXPECT_NE(es[0].detail.find("previous_active_version_id"), std::string::npos);
    EXPECT_NE(es[0].detail.find("01VER00000000000000000OLD"), std::string::npos);
    EXPECT_NE(es[0].detail.find("started_at"), std::string::npos);
}

TEST_F(RolloutAuditBridgeTest, RecordStagePromotedCarriesIndices) {
    bridge_->recordStagePromoted(makeRecord(), /*from=*/1, /*to=*/2, "bob");
    flush();
    auto es = audit_->entries();
    ASSERT_EQ(es.size(), 1u);
    EXPECT_EQ(es[0].action, "rollout.stage_promoted");
    EXPECT_NE(es[0].detail.find("\"from_stage_index\":1"), std::string::npos);
    EXPECT_NE(es[0].detail.find("\"to_stage_index\":2"), std::string::npos);
    EXPECT_NE(es[0].detail.find("\"actor\":\"bob\""), std::string::npos);
}

TEST_F(RolloutAuditBridgeTest, RecordPausedManualCarriesActorAndComment) {
    bridge_->recordPausedManual(makeRecord(), "carol", "needs review");
    flush();
    auto es = audit_->entries();
    ASSERT_EQ(es.size(), 1u);
    EXPECT_EQ(es[0].action, "rollout.paused_manual");
    EXPECT_NE(es[0].detail.find("needs review"), std::string::npos);
    EXPECT_NE(es[0].detail.find("\"reason\":\"MANUAL\""), std::string::npos);
}

TEST_F(RolloutAuditBridgeTest, RecordPausedAutoCarriesReasonAndNoYaml) {
    // Build a record whose RolloutSpec.yaml-analog is populated to prove
    // no such surface leaks. (We simulate by stuffing a long detail string;
    // the bridge must only write the supplied scalar, not any record body.)
    RolloutRecord r = makeRecord();
    bridge_->recordPausedAuto(r, PauseReason::ERROR_RATE,
                               "error_rate=0.15 > 0.05");
    flush();
    auto es = audit_->entries();
    ASSERT_EQ(es.size(), 1u);
    EXPECT_EQ(es[0].action, "rollout.paused_auto");
    EXPECT_NE(es[0].detail.find("ERROR_RATE"), std::string::npos);
    EXPECT_NE(es[0].detail.find("error_rate=0.15"), std::string::npos);
    // SR14: no RolloutSpec schema fields bleed through. We don't carry a
    // yaml payload in the POCO, but we still guard against accidentally
    // logging the spec by key-name reconnaissance.
    EXPECT_EQ(es[0].detail.find("stages\":["), std::string::npos);
    EXPECT_EQ(es[0].detail.find("auto_pause"), std::string::npos);
}

TEST_F(RolloutAuditBridgeTest, RecordResumedEmitsActor) {
    bridge_->recordResumed(makeRecord(), "dave");
    flush();
    auto es = audit_->entries();
    ASSERT_EQ(es.size(), 1u);
    EXPECT_EQ(es[0].action, "rollout.resumed");
    EXPECT_NE(es[0].detail.find("\"actor\":\"dave\""), std::string::npos);
}

TEST_F(RolloutAuditBridgeTest, RecordAutoRollbackTriggeredEmitsDetail) {
    bridge_->recordAutoRollbackTriggered(makeRecord(),
                                          "grace window expired at t=30min");
    flush();
    auto es = audit_->entries();
    ASSERT_EQ(es.size(), 1u);
    EXPECT_EQ(es[0].action, "rollout.auto_rollback_triggered");
    EXPECT_NE(es[0].detail.find("grace window"), std::string::npos);
}

TEST_F(RolloutAuditBridgeTest, RecordAutoRollbackCompletedEmitsNewActive) {
    bridge_->recordAutoRollbackCompleted(makeRecord(), "01VER00000000000000000OLD");
    flush();
    auto es = audit_->entries();
    ASSERT_EQ(es.size(), 1u);
    EXPECT_EQ(es[0].action, "rollout.auto_rollback_completed");
    EXPECT_NE(es[0].detail.find("new_active_version_id"), std::string::npos);
    EXPECT_NE(es[0].detail.find("01VER00000000000000000OLD"), std::string::npos);
}

TEST_F(RolloutAuditBridgeTest, RecordAbortedEmitsActor) {
    bridge_->recordAborted(makeRecord(), "eve");
    flush();
    auto es = audit_->entries();
    ASSERT_EQ(es.size(), 1u);
    EXPECT_EQ(es[0].action, "rollout.aborted");
    EXPECT_NE(es[0].detail.find("\"actor\":\"eve\""), std::string::npos);
}

TEST_F(RolloutAuditBridgeTest, RecordCompletedEmitsCompletionTimestamp) {
    bridge_->recordCompleted(makeRecord(), "01VER00000000000000000NEW");
    flush();
    auto es = audit_->entries();
    ASSERT_EQ(es.size(), 1u);
    EXPECT_EQ(es[0].action, "rollout.completed");
    EXPECT_NE(es[0].detail.find("new_active_version_id"), std::string::npos);
    EXPECT_NE(es[0].detail.find("completed_at"), std::string::npos);
}

TEST_F(RolloutAuditBridgeTest, RecordFailedEmitsReason) {
    bridge_->recordFailed(makeRecord(), "rollback_failed: target not found");
    flush();
    auto es = audit_->entries();
    ASSERT_EQ(es.size(), 1u);
    EXPECT_EQ(es[0].action, "rollout.failed");
    EXPECT_NE(es[0].detail.find("rollback_failed"), std::string::npos);
}

// --- Chain integrity across all 11 actions ---------------------------------

TEST_F(RolloutAuditBridgeTest, AllElevenActionsChainVerifies) {
    auto r = makeRecord();
    bridge_->recordCreated(r, "alice");
    bridge_->recordStarted(r, "alice");
    bridge_->recordStagePromoted(r, 0, 1, "alice");
    bridge_->recordPausedManual(r, "alice", "hold");
    bridge_->recordPausedAuto(r, PauseReason::ERROR_RATE, "rate spike");
    bridge_->recordResumed(r, "alice");
    bridge_->recordAutoRollbackTriggered(r, "grace expired");
    bridge_->recordAutoRollbackCompleted(r, "v-old");
    bridge_->recordAborted(r, "alice");
    bridge_->recordCompleted(r, "v-new");
    bridge_->recordFailed(r, "post-mortem failure");
    flush();

    auto es = audit_->entries();
    ASSERT_EQ(es.size(), 11u);
    EXPECT_TRUE(audit_->verifyChain());
    // chain_hash should strictly advance.
    for (size_t i = 1; i < es.size(); ++i) {
        EXPECT_NE(es[i].chain_hash, es[i - 1].chain_hash);
    }
}

// --- nullptr safety --------------------------------------------------------

TEST(RolloutAuditBridgeNullTest, NullAuditLoggerAllMethodsAreNoops) {
    RolloutAuditBridge b(nullptr);
    RolloutRecord r;
    r.rollout_id = "anything";
    // Must not crash. If any of these crash we'll segfault before EXPECT.
    b.recordCreated(r, "a");
    b.recordStarted(r, "a");
    b.recordStagePromoted(r, 0, 1, "a");
    b.recordPausedManual(r, "a", "c");
    b.recordPausedAuto(r, PauseReason::ERROR_RATE, "d");
    b.recordResumed(r, "a");
    b.recordAutoRollbackTriggered(r, "d");
    b.recordAutoRollbackCompleted(r, "v");
    b.recordAborted(r, "a");
    b.recordCompleted(r, "v");
    b.recordFailed(r, "x");
    SUCCEED();
}

} // namespace
} // namespace aegisgate
