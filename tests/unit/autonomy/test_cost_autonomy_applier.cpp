// Phase 11.5 TASK-20260518-02 Epic 2.2 — CostAutonomyApplier tests.
//
// 4 mandatory scenarios from plan §D Task 2.2 step 1:
//   1. apply_overrides_quality_tier        (per-tenant override installed)
//   2. rollback_restores_quality_tier      (override cleared)
//   3. apply_validates_payload_schema      (missing fields fail closed)
//   4. isLowRisk_judges_correctly          (C2 4 rules — all matrix cells)
//
// Plus dry-run smoke + apply-no-router defensive coverage.

#include "observe/autonomy/cost_autonomy_applier.h"

#include "gateway/ml_router.h"
#include "observe/autonomy/approval_proposal.h"
#include "observe/autonomy/approval_state.h"

#include <gtest/gtest.h>
#include <memory>

using namespace aegisgate;
using namespace aegisgate::autonomy;

namespace {

ApprovalProposal makeCostProp(
    const std::string& tenant   = "tenant-A",
    const std::string& from_tier = "standard",
    const std::string& to_tier   = "economy",
    double savings_usd_24h       = 10.0,
    int    affected_rps          = 100) {
    ApprovalProposal p;
    p.id              = "01HNTEST0000000000000000";
    p.source          = AutonomySource::CostOptimizer;
    p.subject         = "test downgrade";
    p.payload         = nlohmann::json{
        {"action",                      "override_quality_tier"},
        {"tenant_id",                   tenant},
        {"current_model",               "gpt-4o"},
        {"recommended_model",           "gpt-4o-mini"},
        {"from_quality_tier",           from_tier},
        {"to_quality_tier",             to_tier},
        {"estimated_savings_usd_24h",   savings_usd_24h},
        {"affected_requests_per_hour",  affected_rps}};
    p.decision_trace  = nlohmann::json{
        {"source_id",         "cost_optimizer"},
        {"algorithm_name",    "cost_per_quality_v1"},
        {"input_hash_sha256", std::string(64, 'a')},
        {"proposed_at_ms",    1716030000000LL}};
    p.state           = ApprovalState::APPROVED;
    p.payload_sha256  = computePayloadSha256(p.payload);
    return p;
}

} // namespace

// ---------- 1. apply installs override -----------------------------------

TEST(CostAutonomyApplierTest, ApplyOverridesQualityTier) {
    auto router = std::make_shared<MLRouter>();
    CostAutonomyApplier applier(router);

    auto p = makeCostProp("tenant-A", "premium", "standard");
    auto r = applier.apply(p, /*dry_run=*/false);
    EXPECT_TRUE(r.success) << r.error_message;
    ASSERT_TRUE(r.details.contains("router_after"));
    EXPECT_EQ(r.details["router_after"], "standard");

    auto override = router->getQualityTierOverride("tenant-A");
    ASSERT_TRUE(override.has_value());
    EXPECT_EQ(*override, "standard");
}

// ---------- 2. rollback restores ------------------------------------------

TEST(CostAutonomyApplierTest, RollbackRestoresQualityTier) {
    auto router = std::make_shared<MLRouter>();
    CostAutonomyApplier applier(router);

    auto p = makeCostProp("tenant-B", "standard", "economy");
    ASSERT_TRUE(applier.apply(p, false).success);
    ASSERT_TRUE(router->getQualityTierOverride("tenant-B").has_value());

    auto r = applier.rollback(p);
    EXPECT_TRUE(r.success);
    EXPECT_FALSE(router->getQualityTierOverride("tenant-B").has_value());
    ASSERT_TRUE(r.details.contains("router_after"));
    EXPECT_TRUE(r.details["router_after"].is_null());
}

// ---------- 3. apply validates payload schema -----------------------------

TEST(CostAutonomyApplierTest, ApplyValidatesPayloadSchema) {
    auto router = std::make_shared<MLRouter>();
    CostAutonomyApplier applier(router);

    // Missing tenant_id
    ApprovalProposal p;
    p.payload = nlohmann::json{{"to_quality_tier", "economy"}};
    auto r = applier.apply(p, false);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_code, "schema_invalid");
    EXPECT_FALSE(r.error_message.empty());

    // Missing to_quality_tier
    p.payload = nlohmann::json{{"tenant_id", "x"}, {"from_quality_tier", "premium"}};
    r = applier.apply(p, false);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_code, "schema_invalid");

    // Payload not an object at all
    p.payload = nlohmann::json("not-an-object");
    r = applier.apply(p, false);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_code, "schema_invalid");
}

TEST(CostAutonomyApplierTest, ApplyFailsWithoutRouter) {
    CostAutonomyApplier applier(/*router=*/nullptr);
    auto r = applier.apply(makeCostProp(), false);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_code, "router_missing");
}

TEST(CostAutonomyApplierTest, DryRunDoesNotMutateRouter) {
    auto router = std::make_shared<MLRouter>();
    CostAutonomyApplier applier(router);

    auto p = makeCostProp("tenant-DR", "premium", "standard");
    auto r = applier.apply(p, /*dry_run=*/true);
    EXPECT_TRUE(r.success);
    EXPECT_TRUE(r.details.value("dry_run", false));
    EXPECT_EQ(r.details.value("router_after", std::string{}), "standard");
    // But the live router was never mutated.
    EXPECT_FALSE(router->getQualityTierOverride("tenant-DR").has_value());
}

// ---------- 4. isLowRisk — full C2 matrix --------------------------------

TEST(CostAutonomyApplierTest, IsLowRiskAcceptsValidDowngrade) {
    CostAutonomyApplier applier(std::make_shared<MLRouter>());
    // standard → economy, $10 24h, 100 RPS → all rules pass.
    EXPECT_TRUE(applier.isLowRisk(makeCostProp("t", "standard", "economy",
                                                10.0, 100)));
}

TEST(CostAutonomyApplierTest, IsLowRiskR1RejectsUpgrade) {
    CostAutonomyApplier applier(std::make_shared<MLRouter>());
    EXPECT_FALSE(applier.isLowRisk(makeCostProp("t", "economy", "premium",
                                                 5.0, 50)));
    EXPECT_FALSE(applier.isLowRisk(makeCostProp("t", "standard", "premium",
                                                 5.0, 50)));
}

TEST(CostAutonomyApplierTest, IsLowRiskR1RejectsSameTier) {
    CostAutonomyApplier applier(std::make_shared<MLRouter>());
    EXPECT_FALSE(applier.isLowRisk(makeCostProp("t", "standard", "standard",
                                                 5.0, 50)));
}

TEST(CostAutonomyApplierTest, IsLowRiskR2RejectsTwoStepDowngrade) {
    CostAutonomyApplier applier(std::make_shared<MLRouter>());
    EXPECT_FALSE(applier.isLowRisk(makeCostProp("t", "premium", "economy",
                                                 5.0, 50)));
}

TEST(CostAutonomyApplierTest, IsLowRiskR3RejectsHighSavings) {
    CostAutonomyApplier applier(std::make_shared<MLRouter>());
    EXPECT_FALSE(applier.isLowRisk(makeCostProp("t", "standard", "economy",
                                                 50.01, 100)));
}

TEST(CostAutonomyApplierTest, IsLowRiskR4RejectsHighRps) {
    CostAutonomyApplier applier(std::make_shared<MLRouter>());
    EXPECT_FALSE(applier.isLowRisk(makeCostProp("t", "standard", "economy",
                                                 5.0, 1001)));
}

TEST(CostAutonomyApplierTest, IsLowRiskFailClosedOnMissingFields) {
    CostAutonomyApplier applier(std::make_shared<MLRouter>());
    ApprovalProposal p;
    p.payload = nlohmann::json::object();
    EXPECT_FALSE(applier.isLowRisk(p));
}

TEST(CostAutonomyApplierTest, IsLowRiskRejectsUnknownTierName) {
    CostAutonomyApplier applier(std::make_shared<MLRouter>());
    EXPECT_FALSE(applier.isLowRisk(makeCostProp("t", "standard", "zzz", 5.0, 5)));
    EXPECT_FALSE(applier.isLowRisk(makeCostProp("t", "zzz", "economy", 5.0, 5)));
}
