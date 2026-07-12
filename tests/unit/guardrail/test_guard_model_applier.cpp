// Phase 11.1 TASK-20260523-01 — Epic 4 GuardModelApplier tests.
//
// Concrete IApprovalApplier wired under AutonomySource::AdaptiveGuard.
// Translates an APPROVED proposal into a ModelRegistry promote / revert.
//
// Payload schema:
//   {
//     "action":      "promote_shadow_to_live" | "revert_to_previous" | "register_shadow",
//     "model_id":    "guardrail",
//     "version":     "v2.0.1",
//     "shadow_metrics": {
//       "win_rate":            0.62,
//       "shadow_duration_min": 60,
//       "fp_rate_delta":      -0.03
//     }
//   }
//
// isLowRisk implements the 4-rule whitelist (analogous to BanditAutonomy):
//   R1: action ∈ {promote_shadow_to_live, register_shadow}  (revert always allowed but not auto)
//   R2: win_rate >= 0.55
//   R3: shadow_duration_min >= 60
//   R4: fp_rate_delta >= -0.10   (no >10pp false-positive regression)

#include "guardrail/autonomy/guard_model_applier.h"
#include "guardrail/model/memory_guard_model_registry.h"
#include "observe/autonomy/approval_proposal.h"

#include <gtest/gtest.h>

using aegisgate::autonomy::ApprovalProposal;
using aegisgate::autonomy::AutonomySource;
using aegisgate::guard::GuardModelApplier;
using aegisgate::guard::GuardModelStatus;
using aegisgate::guard::MemoryGuardModelRegistry;
using aegisgate::guard::ModelRegistryRecord;

namespace {

std::shared_ptr<MemoryGuardModelRegistry> seedRegistry() {
    auto reg = std::make_shared<MemoryGuardModelRegistry>();
    ModelRegistryRecord live;
    live.model_id = "guardrail";
    live.version = "v1";
    live.path = "/models/guardrail-v1.onnx";
    live.status = GuardModelStatus::Live;
    live.classifier_threshold = 0.5f;
    live.artifact_sha256 = "sha-v1";
    reg->insert(live);

    ModelRegistryRecord shadow;
    shadow.model_id = "guardrail";
    shadow.version = "v2";
    shadow.path = "/models/guardrail-v2.onnx";
    shadow.status = GuardModelStatus::Shadow;
    shadow.classifier_threshold = 0.5f;
    shadow.artifact_sha256 = "sha-v2";
    reg->insert(shadow);
    return reg;
}

ApprovalProposal makeProposal(const std::string& action,
                              const std::string& version = "v2",
                              double win_rate = 0.62,
                              int duration_min = 60,
                              double fp_delta = -0.03) {
    ApprovalProposal p;
    p.id = "prop-001";
    p.source = AutonomySource::AdaptiveGuard;
    p.subject = "guardrail";
    p.payload = {
        {"action", action},
        {"model_id", "guardrail"},
        {"version", version},
        {"shadow_metrics", {
            {"win_rate", win_rate},
            {"shadow_duration_min", duration_min},
            {"fp_rate_delta", fp_delta},
        }},
    };
    return p;
}

}  // namespace

TEST(GuardModelApplierTest, ApplierNameMatchesAutonomySource) {
    auto reg = seedRegistry();
    GuardModelApplier applier(reg);
    EXPECT_EQ(applier.applierName(), "guard_model");
}

TEST(GuardModelApplierTest, ApplyPromotionAdvancesRegistry) {
    auto reg = seedRegistry();
    GuardModelApplier applier(reg);
    auto result = applier.apply(makeProposal("promote_shadow_to_live"),
                                 /*dry_run=*/false);
    ASSERT_TRUE(result.success) << result.error_code;
    auto v1 = reg->get("guardrail", "v1");
    auto v2 = reg->get("guardrail", "v2");
    ASSERT_TRUE(v1.has_value());
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v1->status, GuardModelStatus::Retired);
    EXPECT_EQ(v2->status, GuardModelStatus::Live);
}

TEST(GuardModelApplierTest, DryRunDoesNotMutateRegistry) {
    auto reg = seedRegistry();
    GuardModelApplier applier(reg);
    auto result = applier.apply(makeProposal("promote_shadow_to_live"),
                                 /*dry_run=*/true);
    EXPECT_TRUE(result.success);
    auto v2 = reg->get("guardrail", "v2");
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v2->status, GuardModelStatus::Shadow)
        << "dry_run must leave shadow untouched";
}

TEST(GuardModelApplierTest, ApplyRevertReturnsToPrevious) {
    // Pre-promote v2 to Live so revert is legal.
    auto reg = seedRegistry();
    reg->promote("guardrail", "v2", 1);
    GuardModelApplier applier(reg);

    auto result = applier.apply(makeProposal("revert_to_previous"),
                                 /*dry_run=*/false);
    ASSERT_TRUE(result.success);
    auto v2 = reg->get("guardrail", "v2");
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v2->status, GuardModelStatus::Retired);
}

TEST(GuardModelApplierTest, MissingActionRejected) {
    auto reg = seedRegistry();
    GuardModelApplier applier(reg);
    ApprovalProposal p;
    p.source = AutonomySource::AdaptiveGuard;
    p.subject = "guardrail";
    p.payload = nlohmann::json::object();
    auto result = applier.apply(p, false);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_code, "schema_invalid");
}

TEST(GuardModelApplierTest, IsLowRiskApprovesValidCanary) {
    auto reg = seedRegistry();
    GuardModelApplier applier(reg);
    auto p = makeProposal("promote_shadow_to_live", "v2", 0.6, 90, -0.05);
    EXPECT_TRUE(applier.isLowRisk(p));
}

TEST(GuardModelApplierTest, IsLowRiskRejectsLowWinRate) {
    auto reg = seedRegistry();
    GuardModelApplier applier(reg);
    auto p = makeProposal("promote_shadow_to_live", "v2", 0.40, 90, -0.05);
    EXPECT_FALSE(applier.isLowRisk(p)) << "R2 win_rate >= 0.55 must reject 0.40";
}

TEST(GuardModelApplierTest, IsLowRiskRejectsShortShadow) {
    auto reg = seedRegistry();
    GuardModelApplier applier(reg);
    auto p = makeProposal("promote_shadow_to_live", "v2", 0.7, 10, -0.05);
    EXPECT_FALSE(applier.isLowRisk(p)) << "R3 shadow_duration_min >= 60";
}

TEST(GuardModelApplierTest, IsLowRiskRejectsHighFalsePositiveRegression) {
    auto reg = seedRegistry();
    GuardModelApplier applier(reg);
    auto p = makeProposal("promote_shadow_to_live", "v2", 0.7, 120, -0.25);
    EXPECT_FALSE(applier.isLowRisk(p)) << "R4 fp_rate_delta >= -0.10";
}

TEST(GuardModelApplierTest, IsLowRiskRejectsRevertAction) {
    // R1 — revert is destructive; never auto-approve.
    auto reg = seedRegistry();
    GuardModelApplier applier(reg);
    auto p = makeProposal("revert_to_previous", "v2", 0.7, 120, -0.05);
    EXPECT_FALSE(applier.isLowRisk(p));
}

TEST(GuardModelApplierTest, RefusesApplyWhenAutonomyDisabledByEnv) {
    // SR17 defense-in-depth: even if a caller bypasses the Workflow and hits
    // the applier directly, the AEGISGATE_DISABLE_AUTONOMY env var must
    // short-circuit the apply path.
    auto reg = seedRegistry();
    GuardModelApplier applier(reg);

    setenv("AEGISGATE_DISABLE_AUTONOMY", "1", /*overwrite=*/1);
    auto result = applier.apply(makeProposal("promote_shadow_to_live"), false);
    unsetenv("AEGISGATE_DISABLE_AUTONOMY");

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_code, "autonomy_disabled");
    auto v2 = reg->get("guardrail", "v2");
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v2->status, GuardModelStatus::Shadow)
        << "kill switch must prevent mutation";
}
