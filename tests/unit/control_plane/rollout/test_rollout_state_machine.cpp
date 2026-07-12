// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic A.2 RolloutRecord POCO +
// Epic B.3 RolloutStateMachine tests.
//
// This TU pairs POCO round-trip tests (Epic A.2, this commit) with the
// state-machine transition table tests that follow in Epic B.3. Kept in one
// binary so the pure C++ layer has a single gtest handle.

#include "control_plane/rollout/rollout_record.h"
#include "control_plane/rollout/rollout_state_machine.h"
#include <gtest/gtest.h>

namespace {

using aegisgate::AutoPausePolicy;
using aegisgate::ObservationPolicy;
using aegisgate::PauseReason;
using aegisgate::pauseReasonFromString;
using aegisgate::pauseReasonToString;
using aegisgate::RolloutAction;
using aegisgate::RolloutQuery;
using aegisgate::RolloutRecord;
using aegisgate::RolloutSpec;
using aegisgate::RolloutStageEvent;
using aegisgate::RolloutStageRecord;
using aegisgate::RolloutStatus;
using aegisgate::rolloutActionToString;
using aegisgate::rolloutStatusFromString;
using aegisgate::rolloutStatusToString;
using aegisgate::ScopeSelector;
using aegisgate::attemptRolloutTransition;
using aegisgate::RolloutTransitionInput;

// ---------------------------------------------------------------------------
// Epic A.2 — POCO shape + enum string conversions (5 tests)
// ---------------------------------------------------------------------------

TEST(RolloutRecord, StatusStringRoundtripCoversAllStates) {
    EXPECT_STREQ(rolloutStatusToString(RolloutStatus::PENDING),     "PENDING");
    EXPECT_STREQ(rolloutStatusToString(RolloutStatus::PROGRESSING), "PROGRESSING");
    EXPECT_STREQ(rolloutStatusToString(RolloutStatus::PAUSED),      "PAUSED");
    EXPECT_STREQ(rolloutStatusToString(RolloutStatus::COMPLETED),   "COMPLETED");
    EXPECT_STREQ(rolloutStatusToString(RolloutStatus::FAILED),      "FAILED");
    EXPECT_STREQ(rolloutStatusToString(RolloutStatus::ABORTED),     "ABORTED");

    EXPECT_EQ(rolloutStatusFromString("PENDING").value(),     RolloutStatus::PENDING);
    EXPECT_EQ(rolloutStatusFromString("PROGRESSING").value(), RolloutStatus::PROGRESSING);
    EXPECT_EQ(rolloutStatusFromString("PAUSED").value(),      RolloutStatus::PAUSED);
    EXPECT_EQ(rolloutStatusFromString("COMPLETED").value(),   RolloutStatus::COMPLETED);
    EXPECT_EQ(rolloutStatusFromString("FAILED").value(),      RolloutStatus::FAILED);
    EXPECT_EQ(rolloutStatusFromString("ABORTED").value(),     RolloutStatus::ABORTED);

    EXPECT_FALSE(rolloutStatusFromString("XYZ").has_value());
    EXPECT_FALSE(rolloutStatusFromString("").has_value());
    EXPECT_FALSE(rolloutStatusFromString("pending").has_value());  // case-sensitive
}

TEST(RolloutRecord, PauseReasonStringRoundtrip) {
    EXPECT_STREQ(pauseReasonToString(PauseReason::UNSPECIFIED),   "UNSPECIFIED");
    EXPECT_STREQ(pauseReasonToString(PauseReason::MANUAL),        "MANUAL");
    EXPECT_STREQ(pauseReasonToString(PauseReason::ERROR_RATE),    "ERROR_RATE");
    EXPECT_STREQ(pauseReasonToString(PauseReason::LATENCY_RATIO), "LATENCY_RATIO");
    EXPECT_STREQ(pauseReasonToString(PauseReason::AUTO_ROLLBACK), "AUTO_ROLLBACK");

    EXPECT_EQ(pauseReasonFromString("MANUAL").value(),        PauseReason::MANUAL);
    EXPECT_EQ(pauseReasonFromString("ERROR_RATE").value(),    PauseReason::ERROR_RATE);
    EXPECT_EQ(pauseReasonFromString("LATENCY_RATIO").value(), PauseReason::LATENCY_RATIO);
    EXPECT_EQ(pauseReasonFromString("AUTO_ROLLBACK").value(), PauseReason::AUTO_ROLLBACK);
    EXPECT_FALSE(pauseReasonFromString("unknown").has_value());
}

TEST(RolloutRecord, StageRecordAndSelectorHaveZeroedDefaults) {
    RolloutStageRecord s;
    EXPECT_TRUE(s.name.empty());
    EXPECT_TRUE(s.scope.tenant_globs.empty());
    EXPECT_TRUE(s.scope.regions.empty());
    EXPECT_EQ(s.scope.percentage, 0);
    EXPECT_EQ(s.observation.min_duration_seconds, 0);
    EXPECT_EQ(s.observation.min_sample_count, 0);
    EXPECT_DOUBLE_EQ(s.auto_pause.error_rate_gt, 0.0);
    EXPECT_DOUBLE_EQ(s.auto_pause.p99_latency_ratio_gt, 0.0);
    EXPECT_DOUBLE_EQ(s.auto_pause.absolute_error_rate_gt, 0.0);
    EXPECT_DOUBLE_EQ(s.auto_pause.absolute_p99_latency_ms_gt, 0.0);
}

TEST(RolloutRecord, SpecAndRecordDefaultsMatchSpec) {
    RolloutSpec spec;
    EXPECT_TRUE(spec.target_version_id.empty());
    EXPECT_TRUE(spec.stages.empty());
    EXPECT_EQ(spec.sticky_key, "tenant_id");             // design default
    EXPECT_TRUE(spec.auto_rollback_on_pause);             // design default true
    EXPECT_EQ(spec.auto_rollback_grace_seconds, 1800);    // design default 30min

    RolloutRecord rec;
    EXPECT_TRUE(rec.rollout_id.empty());
    EXPECT_EQ(rec.status, RolloutStatus::PENDING);
    EXPECT_EQ(rec.current_stage_index, 0);
    EXPECT_EQ(rec.started_at, 0);
    EXPECT_EQ(rec.stage_started_at, 0);
    EXPECT_EQ(rec.paused_at, 0);
    EXPECT_EQ(rec.pause_reason, PauseReason::UNSPECIFIED);
    EXPECT_EQ(rec.completed_at, 0);
}

TEST(RolloutRecord, StageEventAndQueryDefaults) {
    RolloutStageEvent ev;
    EXPECT_TRUE(ev.event_id.empty());
    EXPECT_TRUE(ev.rollout_id.empty());
    EXPECT_EQ(ev.stage_index, 0);
    EXPECT_TRUE(ev.event_type.empty());
    EXPECT_EQ(ev.at_millis, 0);

    RolloutQuery q;
    EXPECT_TRUE(q.statuses.empty());
    EXPECT_EQ(q.limit, 50);
    EXPECT_TRUE(q.page_token.empty());
}

// ---------------------------------------------------------------------------
// Epic B.3 — transition table tests (15+ combinations).
// Each transition is asserted both by legal outcome and by symmetric
// illegal-from-other-states coverage so the full matrix stays honest.
// ---------------------------------------------------------------------------

namespace {

RolloutTransitionInput tin(RolloutStatus s, RolloutAction a, bool last = false) {
    RolloutTransitionInput in;
    in.from = s;
    in.action = a;
    in.is_last_stage = last;
    return in;
}

}  // (anonymous within namespace{})

// --- Legal transitions ----------------------------------------------------

TEST(RolloutStateMachine, PendingStartMovesToProgressing) {
    auto out = attemptRolloutTransition(tin(RolloutStatus::PENDING, RolloutAction::Start));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, RolloutStatus::PROGRESSING);
}

TEST(RolloutStateMachine, PendingAbortMovesToAborted) {
    auto out = attemptRolloutTransition(tin(RolloutStatus::PENDING, RolloutAction::Abort));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, RolloutStatus::ABORTED);
}

TEST(RolloutStateMachine, ProgressingPromoteNonLastStaysProgressing) {
    auto out = attemptRolloutTransition(
        tin(RolloutStatus::PROGRESSING, RolloutAction::Promote, /*last=*/false));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, RolloutStatus::PROGRESSING);
}

TEST(RolloutStateMachine, ProgressingPromoteLastCompletes) {
    auto out = attemptRolloutTransition(
        tin(RolloutStatus::PROGRESSING, RolloutAction::Promote, /*last=*/true));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, RolloutStatus::COMPLETED);
}

TEST(RolloutStateMachine, ProgressingPauseManualBecomesPaused) {
    auto out = attemptRolloutTransition(
        tin(RolloutStatus::PROGRESSING, RolloutAction::PauseManual));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, RolloutStatus::PAUSED);
}

TEST(RolloutStateMachine, ProgressingPauseAutoBecomesPaused) {
    auto out = attemptRolloutTransition(
        tin(RolloutStatus::PROGRESSING, RolloutAction::PauseAuto));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, RolloutStatus::PAUSED);
}

TEST(RolloutStateMachine, ProgressingAbortBecomesAborted) {
    auto out = attemptRolloutTransition(
        tin(RolloutStatus::PROGRESSING, RolloutAction::Abort));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, RolloutStatus::ABORTED);
}

TEST(RolloutStateMachine, ProgressingFailBecomesFailed) {
    auto out = attemptRolloutTransition(
        tin(RolloutStatus::PROGRESSING, RolloutAction::Fail));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, RolloutStatus::FAILED);
}

TEST(RolloutStateMachine, PausedResumeReturnsToProgressing) {
    auto out = attemptRolloutTransition(
        tin(RolloutStatus::PAUSED, RolloutAction::Resume));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, RolloutStatus::PROGRESSING);
}

TEST(RolloutStateMachine, PausedAutoRollbackBecomesFailed) {
    auto out = attemptRolloutTransition(
        tin(RolloutStatus::PAUSED, RolloutAction::AutoRollback));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, RolloutStatus::FAILED);
}

TEST(RolloutStateMachine, PausedAbortBecomesAborted) {
    auto out = attemptRolloutTransition(
        tin(RolloutStatus::PAUSED, RolloutAction::Abort));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, RolloutStatus::ABORTED);
}

TEST(RolloutStateMachine, PausedFailBecomesFailed) {
    auto out = attemptRolloutTransition(
        tin(RolloutStatus::PAUSED, RolloutAction::Fail));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, RolloutStatus::FAILED);
}

// --- Illegal transitions --------------------------------------------------

TEST(RolloutStateMachine, PendingPromoteRejected) {
    EXPECT_FALSE(attemptRolloutTransition(
        tin(RolloutStatus::PENDING, RolloutAction::Promote)).has_value());
}

TEST(RolloutStateMachine, PendingResumePauseRejected) {
    EXPECT_FALSE(attemptRolloutTransition(
        tin(RolloutStatus::PENDING, RolloutAction::Resume)).has_value());
    EXPECT_FALSE(attemptRolloutTransition(
        tin(RolloutStatus::PENDING, RolloutAction::PauseManual)).has_value());
    EXPECT_FALSE(attemptRolloutTransition(
        tin(RolloutStatus::PENDING, RolloutAction::PauseAuto)).has_value());
    EXPECT_FALSE(attemptRolloutTransition(
        tin(RolloutStatus::PENDING, RolloutAction::AutoRollback)).has_value());
    EXPECT_FALSE(attemptRolloutTransition(
        tin(RolloutStatus::PENDING, RolloutAction::Fail)).has_value());
}

TEST(RolloutStateMachine, ProgressingStartAndResumeRejected) {
    EXPECT_FALSE(attemptRolloutTransition(
        tin(RolloutStatus::PROGRESSING, RolloutAction::Start)).has_value());
    EXPECT_FALSE(attemptRolloutTransition(
        tin(RolloutStatus::PROGRESSING, RolloutAction::Resume)).has_value());
    EXPECT_FALSE(attemptRolloutTransition(
        tin(RolloutStatus::PROGRESSING, RolloutAction::AutoRollback)).has_value());
}

TEST(RolloutStateMachine, PausedStartPromoteRejected) {
    EXPECT_FALSE(attemptRolloutTransition(
        tin(RolloutStatus::PAUSED, RolloutAction::Start)).has_value());
    EXPECT_FALSE(attemptRolloutTransition(
        tin(RolloutStatus::PAUSED, RolloutAction::Promote)).has_value());
    EXPECT_FALSE(attemptRolloutTransition(
        tin(RolloutStatus::PAUSED, RolloutAction::PauseManual)).has_value());
    EXPECT_FALSE(attemptRolloutTransition(
        tin(RolloutStatus::PAUSED, RolloutAction::PauseAuto)).has_value());
}

TEST(RolloutStateMachine, CompletedIsTerminal) {
    for (auto a : {RolloutAction::Start, RolloutAction::Promote,
                   RolloutAction::PauseManual, RolloutAction::PauseAuto,
                   RolloutAction::Resume, RolloutAction::AutoRollback,
                   RolloutAction::Abort, RolloutAction::Fail}) {
        EXPECT_FALSE(attemptRolloutTransition(
            tin(RolloutStatus::COMPLETED, a)).has_value())
            << "unexpected transition from COMPLETED with action " << rolloutActionToString(a);
    }
}

TEST(RolloutStateMachine, FailedIsTerminal) {
    for (auto a : {RolloutAction::Start, RolloutAction::Promote,
                   RolloutAction::PauseManual, RolloutAction::PauseAuto,
                   RolloutAction::Resume, RolloutAction::AutoRollback,
                   RolloutAction::Abort, RolloutAction::Fail}) {
        EXPECT_FALSE(attemptRolloutTransition(
            tin(RolloutStatus::FAILED, a)).has_value())
            << "unexpected transition from FAILED with action " << rolloutActionToString(a);
    }
}

TEST(RolloutStateMachine, AbortedIsTerminal) {
    for (auto a : {RolloutAction::Start, RolloutAction::Promote,
                   RolloutAction::PauseManual, RolloutAction::PauseAuto,
                   RolloutAction::Resume, RolloutAction::AutoRollback,
                   RolloutAction::Abort, RolloutAction::Fail}) {
        EXPECT_FALSE(attemptRolloutTransition(
            tin(RolloutStatus::ABORTED, a)).has_value())
            << "unexpected transition from ABORTED with action " << rolloutActionToString(a);
    }
}

// --- Action names ---------------------------------------------------------

TEST(RolloutStateMachine, ActionToStringCoversAllValues) {
    EXPECT_STREQ(rolloutActionToString(RolloutAction::Start),        "Start");
    EXPECT_STREQ(rolloutActionToString(RolloutAction::Promote),      "Promote");
    EXPECT_STREQ(rolloutActionToString(RolloutAction::PauseManual),  "PauseManual");
    EXPECT_STREQ(rolloutActionToString(RolloutAction::PauseAuto),    "PauseAuto");
    EXPECT_STREQ(rolloutActionToString(RolloutAction::Resume),       "Resume");
    EXPECT_STREQ(rolloutActionToString(RolloutAction::AutoRollback), "AutoRollback");
    EXPECT_STREQ(rolloutActionToString(RolloutAction::Abort),        "Abort");
    EXPECT_STREQ(rolloutActionToString(RolloutAction::Fail),         "Fail");
}

} // namespace
