// Phase 11.2 TASK-20260521-03 — BanditAutonomyApplier unit tests.
//
// Mirrors CostAutonomyApplier test layout. Anchors M5 (isLowRisk 4 rules)
// and M6 (autonomy env killswitch) mutation candidates.

#include <gtest/gtest.h>
#include "observe/autonomy/bandit_autonomy_applier.h"
#include "observe/autonomy/approval_proposal.h"
#include "gateway/bandit_router.h"
#include "gateway/router.h"

#include <cstdlib>

using namespace aegisgate;
using namespace aegisgate::autonomy;

namespace {

// Minimal Router stub so BanditRouter has a base to forward to in Shadow.
class NullBaseRouter : public Router {
public:
    std::string selectModel(RequestContext&,
                             const ConnectorRegistry&) override {
        return "stub";
    }
};

ApprovalProposal makeProposal(const std::string& action,
                                const std::string& strategy,
                                double canary_pct,
                                double win_rate,
                                int shadow_duration_min,
                                double cost_delta_pct) {
    ApprovalProposal p;
    p.id = "prop-1";
    p.source = AutonomySource::BanditRouter;
    p.subject = action + " " + strategy;
    p.payload = {
        {"action", action},
        {"strategy", strategy},
        {"canary_pct", canary_pct},
        {"shadow_metrics", {
            {"win_rate", win_rate},
            {"shadow_duration_min", shadow_duration_min},
            {"cost_delta_pct", cost_delta_pct}}}};
    p.payload_sha256 = computePayloadSha256(p.payload);
    return p;
}

}  // namespace

class BanditAutonomyApplierTest : public ::testing::Test {
protected:
    void SetUp() override {
        unsetenv("AEGISGATE_DISABLE_AUTONOMY");
        base_ = std::make_shared<NullBaseRouter>();
        bandit_ = std::make_shared<BanditRouter>(base_.get(),
                                                   BanditRouter::Config{});
    }
    std::shared_ptr<NullBaseRouter> base_;
    std::shared_ptr<BanditRouter> bandit_;
};

// T1: apply(shadow_to_live) transitions BanditRouter to Live mode.
TEST_F(BanditAutonomyApplierTest, ApplyShadowToLiveTransitionsRouter) {
    BanditAutonomyApplier applier(bandit_);
    auto p = makeProposal("shadow_to_live", "cost-first", 0.05, 0.65, 60, -10);

    EXPECT_EQ(bandit_->getMode(), BanditMode::Shadow);
    auto r = applier.apply(p, false);
    EXPECT_TRUE(r.success) << r.error_message;
    EXPECT_EQ(bandit_->getMode(), BanditMode::Live);
}

// T2: rollback reverts to Shadow.
TEST_F(BanditAutonomyApplierTest, RollbackRevertsToShadow) {
    BanditAutonomyApplier applier(bandit_);
    auto p = makeProposal("shadow_to_live", "cost-first", 0.05, 0.65, 60, -10);

    ASSERT_TRUE(applier.apply(p, false).success);
    ASSERT_EQ(bandit_->getMode(), BanditMode::Live);

    auto r = applier.rollback(p);
    EXPECT_TRUE(r.success) << r.error_message;
    EXPECT_EQ(bandit_->getMode(), BanditMode::Shadow);
}

// T3: dry_run does NOT mutate router state.
TEST_F(BanditAutonomyApplierTest, DryRunDoesNotTransitionRouter) {
    BanditAutonomyApplier applier(bandit_);
    auto p = makeProposal("shadow_to_live", "cost-first", 0.05, 0.65, 60, -10);

    auto r = applier.apply(p, true);
    EXPECT_TRUE(r.success) << r.error_message;
    EXPECT_EQ(bandit_->getMode(), BanditMode::Shadow)
        << "dry_run must not mutate production state";
}

// === M5 anchor: isLowRisk MUST honor 4 rules (R1-R4). ===
// Mutation injection log:
//   M5 Inject: bandit_autonomy_applier.cpp::isLowRisk { return true; }
//   Expected: this test FAILs (HIGH-risk payloads admit isLowRisk)
//   Verified: 2026-05-22, exit_code=1
//   Restore + PASS.
TEST_F(BanditAutonomyApplierTest, bandit_isLowRisk_4_rules) {
    BanditAutonomyApplier applier(bandit_);

    // OK payload (all 4 rules satisfied).
    auto ok = makeProposal("shadow_to_live", "cost-first", 0.05, 0.65, 60, -10);
    EXPECT_TRUE(applier.isLowRisk(ok));

    // R1 fail: win_rate too low (< 0.55).
    auto bad_r1 = makeProposal("shadow_to_live", "cost-first", 0.05, 0.50, 60, -10);
    EXPECT_FALSE(applier.isLowRisk(bad_r1)) << "R1 win_rate >= 0.55 should fire";

    // R2 fail: shadow duration too short (< 60 min).
    auto bad_r2 = makeProposal("shadow_to_live", "cost-first", 0.05, 0.65, 30, -10);
    EXPECT_FALSE(applier.isLowRisk(bad_r2)) << "R2 shadow_duration >= 60 should fire";

    // R3 fail: canary too aggressive (> 0.05).
    auto bad_r3 = makeProposal("shadow_to_live", "cost-first", 0.20, 0.65, 60, -10);
    EXPECT_FALSE(applier.isLowRisk(bad_r3)) << "R3 canary_pct <= 0.05 should fire";

    // R4 fail: cost delta worse than -20%.
    auto bad_r4 = makeProposal("shadow_to_live", "cost-first", 0.05, 0.65, 60, -25);
    EXPECT_FALSE(applier.isLowRisk(bad_r4)) << "R4 cost_delta_pct >= -20 should fire";
}

// === M6 anchor: apply() MUST consult isAutonomyEnabled (SR17 reuse). ===
// Mutation injection log:
//   M6 Inject: bandit_autonomy_applier.cpp::apply remove env-disabled guard.
//   Expected: this test FAILs (router transitions to Live with env=1)
//   Verified: 2026-05-22, exit_code=1
//   Restore + PASS.
TEST_F(BanditAutonomyApplierTest, bandit_applier_respects_env_killswitch) {
    BanditAutonomyApplier applier(bandit_);
    auto p = makeProposal("shadow_to_live", "cost-first", 0.05, 0.65, 60, -10);

    setenv("AEGISGATE_DISABLE_AUTONOMY", "1", 1);
    auto r = applier.apply(p, false);
    EXPECT_FALSE(r.success)
        << "M6: apply must refuse with AEGISGATE_DISABLE_AUTONOMY=1";
    // M6 anchor: error_code must be applier-level "autonomy_disabled" (NOT
    // the router-level "transition_refused"); this distinction is what
    // catches a missing applier guard even if BanditRouter still refuses.
    EXPECT_EQ(r.error_code, "autonomy_disabled")
        << "M6: applier MUST short-circuit before calling BanditRouter";
    EXPECT_EQ(bandit_->getMode(), BanditMode::Shadow);
    unsetenv("AEGISGATE_DISABLE_AUTONOMY");
}

// T7: schema_invalid surfaces clean ApplyResult fail.
TEST_F(BanditAutonomyApplierTest, MalformedPayloadFailsClosed) {
    BanditAutonomyApplier applier(bandit_);
    ApprovalProposal p;
    p.source = AutonomySource::BanditRouter;
    p.payload = "not an object";
    auto r = applier.apply(p, false);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_code, "schema_invalid");
}

// T8: unknown action fails cleanly.
TEST_F(BanditAutonomyApplierTest, UnknownActionFailsClosed) {
    BanditAutonomyApplier applier(bandit_);
    auto p = makeProposal("bogus_action", "cost-first", 0.05, 0.65, 60, -10);
    auto r = applier.apply(p, false);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_code, "unknown_action");
}

// T9: applierName returns expected.
TEST_F(BanditAutonomyApplierTest, ApplierNameMatchesContract) {
    BanditAutonomyApplier applier(bandit_);
    EXPECT_EQ(applier.applierName(), "bandit_autonomy");
}
