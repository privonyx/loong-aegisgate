#pragma once

// Phase 11.3 TASK-20260523-02 — HumanApprovalNodeHandler.
//
// Decision D4=C: reuse AutonomyApprovalWorkflow. The handler is the seam
// the WorkflowEngine calls when it sees a NodeType::HumanApproval node;
// it crafts an ApprovalProposal with source=Workflow and pushes it into
// the existing 5-state machine (PROPOSED -> APPROVED -> APPLIED).
//
// SR-NEW4 enforcement: every string in the proposal payload + decision
// trace is scrubbed by PIIFilter::mask() before the proposal is enqueued.
// PIIFilter usage is mandatory in production; tests may pass nullptr for
// unit scenarios that explicitly verify the no-filter behaviour, but the
// GatewayRuntime wiring always supplies the runtime singleton.
//
// Design references:
//   docs/specs/2026-05-23-phase11.3-workflow-2.0-design.md §4.3 §4.4

#include "workflow/workflow_dsl.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace aegisgate {
class PIIFilter;
} // namespace aegisgate

namespace aegisgate::autonomy {
class AutonomyApprovalWorkflow;
} // namespace aegisgate::autonomy

namespace aegisgate::workflow {

class HumanApprovalNodeHandler {
public:
    HumanApprovalNodeHandler(
        std::shared_ptr<aegisgate::autonomy::AutonomyApprovalWorkflow> workflow,
        std::shared_ptr<aegisgate::PIIFilter>                          pii_filter);

    // Build an ApprovalProposal from the in-flight workflow context and
    // submit it via workflow_->propose(). Returns the assigned ULID on
    // success, or an empty string on failure (autonomy disabled, queue
    // rejected, validation failed). The returned id is what
    // WorkflowApprovalApplier and resume() use to correlate the proposal
    // back to the paused run.
    std::string enqueue(const std::string& run_id,
                         const std::string& workflow_id,
                         const std::string& dsl_hash,
                         const NodeSpec&    node,
                         const nlohmann::json& context);

private:
    nlohmann::json scrub(nlohmann::json v) const;

    std::shared_ptr<aegisgate::autonomy::AutonomyApprovalWorkflow> workflow_;
    std::shared_ptr<aegisgate::PIIFilter>                           pii_filter_;
};

} // namespace aegisgate::workflow
