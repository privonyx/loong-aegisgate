#pragma once

// Phase 11.3 TASK-20260523-02 — IWorkflowStateStore abstraction.
//
// Decision D3=D ("abstract + Memory + SQLite") gives the WorkflowEngine a
// single persistence surface so tests run against the in-memory backend and
// production swaps in SQLite without engine code changes.
//
// All methods are thread-safe: implementations protect their internal state
// with a mutex / SQLite transaction (BEGIN IMMEDIATE).
//
// Design reference: docs/specs/2026-05-23-phase11.3-workflow-2.0-design.md §4.2

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aegisgate::workflow {

enum class WorkflowRunStatus {
    Pending             = 0,
    Running             = 1,
    WaitingForApproval  = 2,
    Succeeded           = 3,
    Failed              = 4,
    Cancelled           = 5,
    DeadLetter          = 6,
};

enum class WorkflowNodeStatus {
    Pending             = 0,
    Running             = 1,
    Succeeded           = 2,
    Failed              = 3,
    Skipped             = 4,
    WaitingForApproval  = 5,
    DeadLetter          = 6,
};

inline const char* toString(WorkflowRunStatus s) {
    switch (s) {
        case WorkflowRunStatus::Pending:            return "pending";
        case WorkflowRunStatus::Running:            return "running";
        case WorkflowRunStatus::WaitingForApproval: return "waiting_for_approval";
        case WorkflowRunStatus::Succeeded:          return "succeeded";
        case WorkflowRunStatus::Failed:             return "failed";
        case WorkflowRunStatus::Cancelled:          return "cancelled";
        case WorkflowRunStatus::DeadLetter:         return "dead_letter";
    }
    return "pending";
}

inline const char* toString(WorkflowNodeStatus s) {
    switch (s) {
        case WorkflowNodeStatus::Pending:            return "pending";
        case WorkflowNodeStatus::Running:            return "running";
        case WorkflowNodeStatus::Succeeded:          return "succeeded";
        case WorkflowNodeStatus::Failed:             return "failed";
        case WorkflowNodeStatus::Skipped:            return "skipped";
        case WorkflowNodeStatus::WaitingForApproval: return "waiting_for_approval";
        case WorkflowNodeStatus::DeadLetter:         return "dead_letter";
    }
    return "pending";
}

std::optional<WorkflowRunStatus>  workflowRunStatusFromString(std::string_view s);
std::optional<WorkflowNodeStatus> workflowNodeStatusFromString(std::string_view s);

struct WorkflowRunRecord {
    std::string         run_id;
    std::string         workflow_id;
    std::string         dsl_hash;       // T01 anti-tamper anchor
    WorkflowRunStatus   status        = WorkflowRunStatus::Pending;
    std::int64_t        created_at_ms = 0;
    std::int64_t        updated_at_ms = 0;
    std::string         dsl_json;       // canonical serialised DSL
    std::string         context_json;   // shared blackboard for nodes
    std::string         initiator_user_id;
};

struct WorkflowNodeRunRecord {
    std::string         run_id;
    std::string         node_id;
    int                 attempt        = 1;
    WorkflowNodeStatus  status         = WorkflowNodeStatus::Pending;
    std::int64_t        started_at_ms  = 0;
    std::int64_t        ended_at_ms    = 0;
    std::string         result_json;
    std::string         error_message;
    std::string         approval_proposal_id;  // set for HumanApproval nodes
};

class IWorkflowStateStore {
public:
    virtual ~IWorkflowStateStore() = default;

    // --- run lifecycle ---------------------------------------------------
    virtual bool                              createRun(const WorkflowRunRecord& r)                            = 0;
    virtual std::optional<WorkflowRunRecord>  getRun(const std::string& run_id)                                 = 0;
    virtual std::vector<WorkflowRunRecord>    listRuns(std::optional<WorkflowRunStatus> filter)                  = 0;
    virtual bool                              transitionRunStatus(const std::string& run_id,
                                                                   WorkflowRunStatus new_status,
                                                                   std::int64_t when_ms)                         = 0;
    virtual bool                              updateRunContext(const std::string& run_id,
                                                                const std::string& new_context_json,
                                                                std::int64_t when_ms)                            = 0;

    // --- node runs -------------------------------------------------------
    virtual bool                                  upsertNodeRun(const WorkflowNodeRunRecord& n)               = 0;
    virtual std::vector<WorkflowNodeRunRecord>    listNodeRuns(const std::string& run_id)                     = 0;
    virtual std::optional<WorkflowNodeRunRecord>  getNodeRun(const std::string& run_id,
                                                              const std::string& node_id)                      = 0;

    // --- retention -------------------------------------------------------
    // Removes runs whose updated_at_ms < cutoff_ms. Returns the number of
    // runs deleted (node rows are cascaded).
    virtual int                               pruneOldRuns(std::int64_t cutoff_ms)                             = 0;
};

} // namespace aegisgate::workflow
