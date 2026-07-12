// Phase 11.1 TASK-20260523-01 — Epic 4.2 GuardModelApplier registered with
// AutonomyApprovalWorkflow. Validates the full propose -> approve -> apply
// path lands as a Live promotion in the model registry, and that the SR17
// kill switch refuses propose() when AEGISGATE_DISABLE_AUTONOMY=1.

#include "guardrail/audit.h"
#include "guardrail/autonomy/guard_model_applier.h"
#include "guardrail/model/memory_guard_model_registry.h"
#include "observe/autonomy/approval_proposal.h"
#include "observe/autonomy/approval_queue.h"
#include "observe/autonomy/approval_workflow.h"

#include <gtest/gtest.h>

using aegisgate::AuditLogger;
using aegisgate::autonomy::ApprovalProposal;
using aegisgate::autonomy::ApprovalQueue;
using aegisgate::autonomy::ApprovalState;
using aegisgate::autonomy::AutonomyApprovalWorkflow;
using aegisgate::autonomy::AutonomySource;
using aegisgate::autonomy::computePayloadSha256;
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
    live.status = GuardModelStatus::Live;
    live.artifact_sha256 = "sha-v1";
    reg->insert(live);

    ModelRegistryRecord shadow;
    shadow.model_id = "guardrail";
    shadow.version = "v2";
    shadow.status = GuardModelStatus::Shadow;
    shadow.artifact_sha256 = "sha-v2";
    reg->insert(shadow);
    return reg;
}

ApprovalProposal buildPromoteProposal() {
    ApprovalProposal p;
    p.source = AutonomySource::AdaptiveGuard;
    p.subject = "guardrail";
    p.payload = {
        {"action", "promote_shadow_to_live"},
        {"model_id", "guardrail"},
        {"version", "v2"},
        {"shadow_metrics", {{"win_rate", 0.62},
                              {"shadow_duration_min", 90},
                              {"fp_rate_delta", -0.04}}},
    };
    p.decision_trace = {
        {"source_id", "guardrail/v2"},
        {"algorithm_name", "supervised_classifier"},
        {"input_hash_sha256", "deadbeef"},
        {"proposed_at_ms", 1700000000000},
    };
    p.payload_sha256 = computePayloadSha256(p.payload);
    return p;
}

}  // namespace

TEST(GuardApplierWorkflowIntegrationTest, ProposeApproveApplyPromotes) {
    auto queue = std::make_shared<ApprovalQueue>(nullptr);
    auto audit = std::make_shared<AuditLogger>();
    AutonomyApprovalWorkflow wf(queue, audit);
    wf.setAutonomyEnabledOverride(true);

    auto registry = seedRegistry();
    auto applier = std::make_shared<GuardModelApplier>(registry);
    wf.registerApplier(AutonomySource::AdaptiveGuard, applier);

    auto id = wf.propose(buildPromoteProposal());
    ASSERT_FALSE(id.empty());

    ASSERT_TRUE(wf.approve(id, "alice"));
    ASSERT_TRUE(wf.apply(id));

    auto p = wf.get(id);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->state, ApprovalState::APPLIED);

    auto v2 = registry->get("guardrail", "v2");
    auto v1 = registry->get("guardrail", "v1");
    ASSERT_TRUE(v1.has_value());
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v1->status, GuardModelStatus::Retired);
    EXPECT_EQ(v2->status, GuardModelStatus::Live);
}

TEST(GuardApplierWorkflowIntegrationTest, AutonomyDisabledRefusesPropose) {
    auto queue = std::make_shared<ApprovalQueue>(nullptr);
    auto audit = std::make_shared<AuditLogger>();
    AutonomyApprovalWorkflow wf(queue, audit);
    wf.setAutonomyEnabledOverride(false);  // SR17 kill switch active

    auto registry = seedRegistry();
    auto applier = std::make_shared<GuardModelApplier>(registry);
    wf.registerApplier(AutonomySource::AdaptiveGuard, applier);

    auto id = wf.propose(buildPromoteProposal());
    EXPECT_TRUE(id.empty())
        << "AEGISGATE_DISABLE_AUTONOMY=1 must short-circuit propose()";

    // Registry untouched.
    auto v2 = registry->get("guardrail", "v2");
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v2->status, GuardModelStatus::Shadow);
}
