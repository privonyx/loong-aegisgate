#pragma once

// Phase 11.3 TASK-20260523-02 — SQLite IWorkflowStateStore backend.
//
// Schema:
//   workflow_runs(run_id PK, workflow_id, dsl_hash, status CHECK in (...),
//                 created_at_ms, updated_at_ms, dsl_json, context_json,
//                 initiator_user_id)
//   workflow_node_runs(run_id FK ON DELETE CASCADE, node_id, attempt,
//                       status CHECK in (...), started_at_ms, ended_at_ms,
//                       result_json, error_message, approval_proposal_id,
//                       PRIMARY KEY(run_id, node_id))
//
// All write paths wrap their statements in `BEGIN IMMEDIATE; ...; COMMIT;`
// so concurrent transitions on the same row serialise without deadlocking
// (SR-NEW3 invariant).

#include "workflow/workflow_state_store.h"

#include <mutex>
#include <string>

struct sqlite3;

namespace aegisgate::workflow {

class SQLiteWorkflowStateStore final : public IWorkflowStateStore {
public:
    explicit SQLiteWorkflowStateStore(const std::string& db_path);
    ~SQLiteWorkflowStateStore() override;
    SQLiteWorkflowStateStore(const SQLiteWorkflowStateStore&)            = delete;
    SQLiteWorkflowStateStore& operator=(const SQLiteWorkflowStateStore&) = delete;

    bool initialize();

    bool                              createRun(const WorkflowRunRecord& r) override;
    std::optional<WorkflowRunRecord>  getRun(const std::string& run_id) override;
    std::vector<WorkflowRunRecord>    listRuns(std::optional<WorkflowRunStatus> filter) override;
    bool                              transitionRunStatus(const std::string& run_id,
                                                           WorkflowRunStatus new_status,
                                                           std::int64_t when_ms) override;
    bool                              updateRunContext(const std::string& run_id,
                                                        const std::string& new_context_json,
                                                        std::int64_t when_ms) override;
    bool                              upsertNodeRun(const WorkflowNodeRunRecord& n) override;
    std::vector<WorkflowNodeRunRecord> listNodeRuns(const std::string& run_id) override;
    std::optional<WorkflowNodeRunRecord> getNodeRun(const std::string& run_id,
                                                      const std::string& node_id) override;
    int                               pruneOldRuns(std::int64_t cutoff_ms) override;

    // Test-only escape hatch — executes raw SQL inside the same lock so
    // tests can probe DDL invariants (CHECK constraints, foreign keys).
    bool execRawForTesting(const std::string& sql);

private:
    sqlite3*    db_       = nullptr;
    std::string db_path_;
    std::mutex  mu_;
};

} // namespace aegisgate::workflow
