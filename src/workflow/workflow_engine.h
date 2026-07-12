#pragma once

// Phase 11.3 TASK-20260523-02 — DAG WorkflowEngine.
//
// Decisions:
//   D2=A  self-implemented DAG scheduler (no graph lib)
//   D5=C  thread pool dispatch (ThreadPool)
//   D6=A  every executable node goes through ToolSandbox (SR3 layer 1)
//   D8=A  retry with exponential backoff option, terminal failure -> DLQ
//   D9=C  SR17 dual-layer kill switch — engine layer is checked here;
//         applier layer is checked in WorkflowApprovalApplier.
//
// Invariants surfaced as test anchors:
//   I1 cycle detection refuses execute (SR-NEW3)
//   I2 sandbox bypass attempt (empty tool_id) refuses execute (SR3)
//   I3 every Tool node calls ToolSandbox::execute (SR3 layer 1)
//   I4 SR17 layer 1 — execute short-circuits to Cancelled when disabled
//   I5 retries exhaust -> Node DLQ -> Run DeadLetter
//   I6 dependency order: succ runs only after all preds succeed

#include "workflow/workflow_dsl.h"
#include "workflow/workflow_state_store.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace aegisgate {
class ToolSandbox;
} // namespace aegisgate

namespace aegisgate::workflow {

struct WorkflowEngineConfig {
    std::size_t                   worker_count             = 4;
    std::chrono::milliseconds     default_node_timeout     {30'000};
    bool                          stop_on_first_failure    = true;
    int                           max_concurrent_runs      = 16;
};

struct WorkflowExecutionResult {
    bool                ok                = false;
    WorkflowRunStatus   final_status      = WorkflowRunStatus::Failed;
    std::string         error_message;
    int                 completed_nodes   = 0;
    int                 failed_nodes      = 0;
};

// Hook for HumanApproval node: returns true if the run should pause and
// wait for approval; the engine then transitions the run to
// WaitingForApproval and exits execute() with ok=false +
// final_status=WaitingForApproval. A subsequent call to resume() drives
// the run forward once the proposal is applied.
using HumanApprovalNodeCallback = std::function<bool(
    const std::string& run_id,
    const NodeSpec&    node,
    const nlohmann::json& context)>;

class WorkflowEngine {
public:
    WorkflowEngine(WorkflowEngineConfig         cfg,
                   aegisgate::ToolSandbox*      sandbox,
                   IWorkflowStateStore*         state_store);
    ~WorkflowEngine();

    WorkflowEngine(const WorkflowEngine&)            = delete;
    WorkflowEngine& operator=(const WorkflowEngine&) = delete;

    void setHumanApprovalCallback(HumanApprovalNodeCallback cb) {
        approval_cb_ = std::move(cb);
    }

    // Test/override hook: when set, overrides env-derived SR17 result.
    void setAutonomyEnabledOverride(std::optional<bool> v) {
        autonomy_override_ = v;
    }

    WorkflowExecutionResult execute(const WorkflowDsl&   dsl,
                                    const std::string&   run_id,
                                    const nlohmann::json& context);

    // Resume a run currently in WaitingForApproval. Reads the run + nodes
    // back from the store; the approved HumanApproval node is marked
    // Succeeded and any successors fire.
    WorkflowExecutionResult resume(const std::string& run_id);

    // SR17 engine layer — same env var pattern as
    // AutonomyApprovalWorkflow::isAutonomyEnabled.
    static bool isAutonomyEnabled();

    // TASK-20260703-02 C8：ToolSandbox 是否已装配。nullptr 时所有 NodeType::Tool
    // 节点安全降级入 DLQ（不崩）；生产 gateway_runtime 应从 pipeline_->tool_sandbox
    // 装配（此前误传 nullptr 导致 Tool 节点全进 DLQ）。
    bool hasSandbox() const noexcept { return sandbox_ != nullptr; }

private:
    bool checkAutonomyOrCancel(const WorkflowDsl& dsl,
                                const std::string& run_id,
                                WorkflowExecutionResult& out);

    WorkflowEngineConfig     cfg_;
    aegisgate::ToolSandbox*  sandbox_      = nullptr;
    IWorkflowStateStore*     state_store_  = nullptr;
    HumanApprovalNodeCallback approval_cb_;
    std::optional<bool>      autonomy_override_;
    std::mutex               mu_;
    std::atomic<int>         active_runs_{0};
};

} // namespace aegisgate::workflow
