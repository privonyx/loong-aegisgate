// Phase 9.3 Epic 3 Task 3.1 — W3 state machine transition matrix.
//
// Matches design §5.2:
//
//   PENDING    + APPROVE     -> APPROVED
//   PENDING    + REJECT      -> REJECTED
//   APPROVED   + ACTIVATE    -> ACTIVE
//   APPROVED   + REJECT      -> REJECTED
//   ACTIVE     + ROLLBACK_TO -> ACTIVE      (idempotent self-activation)
//   SUPERSEDED + ROLLBACK_TO -> ACTIVE      (R2 exemption)
//
// Everything else is illegal and must yield nullopt / false.

#include "control_plane/state_machine.h"
#include <gtest/gtest.h>

namespace aegisgate {
namespace {

// --- Legal transitions ---

TEST(StateMachineLegal, PendingApproveYieldsApproved) {
    auto out = StateMachine::next(ConfigStatus::PENDING, ConfigAction::APPROVE);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, ConfigStatus::APPROVED);
    EXPECT_TRUE(StateMachine::canTransition(
        ConfigStatus::PENDING, ConfigAction::APPROVE));
}

TEST(StateMachineLegal, PendingRejectYieldsRejected) {
    auto out = StateMachine::next(ConfigStatus::PENDING, ConfigAction::REJECT);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, ConfigStatus::REJECTED);
}

TEST(StateMachineLegal, ApprovedActivateYieldsActive) {
    auto out = StateMachine::next(ConfigStatus::APPROVED, ConfigAction::ACTIVATE);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, ConfigStatus::ACTIVE);
}

TEST(StateMachineLegal, ApprovedRejectYieldsRejected) {
    auto out = StateMachine::next(ConfigStatus::APPROVED, ConfigAction::REJECT);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, ConfigStatus::REJECTED);
}

TEST(StateMachineLegal, ActiveRollbackToIdempotentActive) {
    // Rolling back to the currently-active bundle is a no-op success, so the
    // state machine allows ACTIVE + ROLLBACK_TO -> ACTIVE. The service layer
    // short-circuits the underlying activateConfig call separately.
    auto out = StateMachine::next(
        ConfigStatus::ACTIVE, ConfigAction::ROLLBACK_TO);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, ConfigStatus::ACTIVE);
}

TEST(StateMachineLegal, SupersededRollbackToYieldsActive) {
    // R2 exemption — rolling a previously-active bundle back into service.
    auto out = StateMachine::next(
        ConfigStatus::SUPERSEDED, ConfigAction::ROLLBACK_TO);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, ConfigStatus::ACTIVE);
}

// --- Illegal transitions (a representative 10+ from the §5.2 matrix) ---

TEST(StateMachineIllegal, PendingActivate) {
    EXPECT_FALSE(StateMachine::next(
        ConfigStatus::PENDING, ConfigAction::ACTIVATE).has_value());
    EXPECT_FALSE(StateMachine::canTransition(
        ConfigStatus::PENDING, ConfigAction::ACTIVATE));
}

TEST(StateMachineIllegal, PendingRollback) {
    EXPECT_FALSE(StateMachine::next(
        ConfigStatus::PENDING, ConfigAction::ROLLBACK_TO).has_value());
}

TEST(StateMachineIllegal, ApprovedApprove) {
    // Double-approval is meaningless and must be rejected.
    EXPECT_FALSE(StateMachine::next(
        ConfigStatus::APPROVED, ConfigAction::APPROVE).has_value());
}

TEST(StateMachineIllegal, ApprovedRollback) {
    // Never-activated APPROVED should not go through rollback; service layer
    // directs the caller to ActivateVersion instead.
    EXPECT_FALSE(StateMachine::next(
        ConfigStatus::APPROVED, ConfigAction::ROLLBACK_TO).has_value());
}

TEST(StateMachineIllegal, ActiveApprove) {
    EXPECT_FALSE(StateMachine::next(
        ConfigStatus::ACTIVE, ConfigAction::APPROVE).has_value());
}

TEST(StateMachineIllegal, ActiveReject) {
    EXPECT_FALSE(StateMachine::next(
        ConfigStatus::ACTIVE, ConfigAction::REJECT).has_value());
}

TEST(StateMachineIllegal, ActiveActivate) {
    // ACTIVE + ACTIVATE is not meaningful; use ROLLBACK_TO for idempotency.
    EXPECT_FALSE(StateMachine::next(
        ConfigStatus::ACTIVE, ConfigAction::ACTIVATE).has_value());
}

TEST(StateMachineIllegal, RejectedIsTerminal) {
    for (auto a : {ConfigAction::APPROVE, ConfigAction::REJECT,
                   ConfigAction::ACTIVATE, ConfigAction::ROLLBACK_TO}) {
        EXPECT_FALSE(StateMachine::next(ConfigStatus::REJECTED, a).has_value())
            << "REJECTED must be terminal for action "
            << static_cast<int>(a);
    }
}

TEST(StateMachineIllegal, SupersededApprove) {
    EXPECT_FALSE(StateMachine::next(
        ConfigStatus::SUPERSEDED, ConfigAction::APPROVE).has_value());
}

TEST(StateMachineIllegal, SupersededReject) {
    EXPECT_FALSE(StateMachine::next(
        ConfigStatus::SUPERSEDED, ConfigAction::REJECT).has_value());
}

TEST(StateMachineIllegal, SupersededActivate) {
    // Forcing ACTIVATE on a SUPERSEDED bundle is forbidden; must go through
    // ROLLBACK_TO so the audit event carries the correct semantics.
    EXPECT_FALSE(StateMachine::next(
        ConfigStatus::SUPERSEDED, ConfigAction::ACTIVATE).has_value());
}

} // namespace
} // namespace aegisgate
