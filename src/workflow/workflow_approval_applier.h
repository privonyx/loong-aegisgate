#pragma once

// Phase 11.3 TASK-20260523-02 — WorkflowApprovalApplier.
//
// IApprovalApplier registered against AutonomySource::Workflow inside
// AutonomyApprovalWorkflow. apply() drives a paused run forward by
// invoking WorkflowEngine::resume(). isLowRisk() implements the v1
// 4-rule predicate used by the auto_low_risk autonomy mode (SR2).
//
// Defense-in-depth (D9=C dual-layer SR17): the base now owns the SR17
// layer 2 check via AutonomyApprovalWorkflow::isAutonomyEnabled(),
// which protects against:
//   - tests instantiating the applier directly without the workflow
//   - future code paths short-circuiting straight to applier->apply()
//
// TASK-20260523-03 — migrated to BaseAutonomyApplier. The T01 dsl_hash
// verification remains inside applyImpl() (subclass-specific invariant).

#include "observe/autonomy/base_autonomy_applier.h"

namespace aegisgate::workflow {

class WorkflowEngine;
class IWorkflowStateStore;

class WorkflowApprovalApplier final
    : public aegisgate::autonomy::BaseAutonomyApplier {
public:
    WorkflowApprovalApplier(WorkflowEngine*       engine,
                             IWorkflowStateStore*  state_store);

    std::string applierName() const override { return "WorkflowApprovalApplier"; }

    bool isLowRisk(const aegisgate::autonomy::ApprovalProposal& p) const override;

protected:
    aegisgate::autonomy::ApplyResult applyImpl(
        const aegisgate::autonomy::ApprovalProposal& p,
        bool dry_run) override;

    aegisgate::autonomy::ApplyResult rollbackImpl(
        const aegisgate::autonomy::ApprovalProposal& p) override;

private:
    WorkflowEngine*      engine_;
    IWorkflowStateStore* state_store_;
};

} // namespace aegisgate::workflow
