// Phase 11.5 TASK-20260518-02 Epic 1.1 — ApprovalState / AutonomySource tests.

#include "observe/autonomy/approval_state.h"

#include <gtest/gtest.h>

using aegisgate::autonomy::ApprovalState;
using aegisgate::autonomy::AutonomySource;
using aegisgate::autonomy::approvalStateFromString;
using aegisgate::autonomy::autonomySourceFromString;
using aegisgate::autonomy::toString;

TEST(ApprovalStateTest, RoundtripCoversAllFiveStates) {
    constexpr ApprovalState all[] = {
        ApprovalState::PROPOSED, ApprovalState::APPROVED,
        ApprovalState::APPLIED,  ApprovalState::REJECTED,
        ApprovalState::ROLLED_BACK,
    };
    for (auto s : all) {
        auto round = approvalStateFromString(toString(s));
        ASSERT_TRUE(round.has_value()) << toString(s);
        EXPECT_EQ(*round, s);
    }
}

TEST(ApprovalStateTest, InvalidStringReturnsNullopt) {
    EXPECT_FALSE(approvalStateFromString("").has_value());
    EXPECT_FALSE(approvalStateFromString("proposed").has_value());      // case-sensitive
    EXPECT_FALSE(approvalStateFromString("UNKNOWN").has_value());
    EXPECT_FALSE(approvalStateFromString("ROLLEDBACK").has_value());    // typo guard
}

TEST(ApprovalStateTest, StringFormStableAcrossWire) {
    // These strings appear verbatim in autonomy_proposals.state column +
    // audit log entries. Renaming them is a wire-breaking change; the
    // assertions below pin the contract so any rename surfaces as test
    // failure in CI.
    EXPECT_EQ(toString(ApprovalState::PROPOSED),    "PROPOSED");
    EXPECT_EQ(toString(ApprovalState::APPROVED),    "APPROVED");
    EXPECT_EQ(toString(ApprovalState::APPLIED),     "APPLIED");
    EXPECT_EQ(toString(ApprovalState::REJECTED),    "REJECTED");
    EXPECT_EQ(toString(ApprovalState::ROLLED_BACK), "ROLLED_BACK");
}

TEST(AutonomySourceTest, RoundtripCoversAllFiveSources) {
    constexpr AutonomySource all[] = {
        AutonomySource::CostOptimizer, AutonomySource::AutoRecovery,
        AutonomySource::BanditRouter,  AutonomySource::AdaptiveGuard,
        AutonomySource::Workflow,
    };
    for (auto s : all) {
        auto round = autonomySourceFromString(toString(s));
        ASSERT_TRUE(round.has_value()) << toString(s);
        EXPECT_EQ(*round, s);
    }
}

TEST(AutonomySourceTest, InvalidStringReturnsNullopt) {
    EXPECT_FALSE(autonomySourceFromString("").has_value());
    EXPECT_FALSE(autonomySourceFromString("costoptimizer").has_value());
    EXPECT_FALSE(autonomySourceFromString("UNKNOWN").has_value());
    EXPECT_FALSE(autonomySourceFromString("Cost_Optimizer").has_value());
}

TEST(AutonomySourceTest, StringFormStableAcrossWire) {
    EXPECT_EQ(toString(AutonomySource::CostOptimizer), "CostOptimizer");
    EXPECT_EQ(toString(AutonomySource::AutoRecovery),  "AutoRecovery");
    EXPECT_EQ(toString(AutonomySource::BanditRouter),  "BanditRouter");
    EXPECT_EQ(toString(AutonomySource::AdaptiveGuard), "AdaptiveGuard");
    EXPECT_EQ(toString(AutonomySource::Workflow),      "Workflow");
}
