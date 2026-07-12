#pragma once

// Phase 11.3 TASK-20260523-02 — In-memory IWorkflowStateStore backend.
//
// Used for tests and ephemeral local runs. Mirrors the SQLite schema so
// behavior parity is direct (lookups by run_id, node_id; status filters).

#include "workflow/workflow_state_store.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace aegisgate::workflow {

class MemoryWorkflowStateStore final : public IWorkflowStateStore {
public:
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

private:
    // I30 (TASK-20260703-04)：主键含 attempt，使每次重试为独立行（审计链），
    // 与 SQLite `(run_id,node_id,attempt)` parity。getNodeRun 返回最高 attempt。
    struct NodeKey {
        std::string run_id;
        std::string node_id;
        int         attempt;
        bool operator==(const NodeKey& o) const {
            return run_id == o.run_id && node_id == o.node_id &&
                   attempt == o.attempt;
        }
    };
    struct NodeKeyHash {
        std::size_t operator()(const NodeKey& k) const noexcept {
            return std::hash<std::string>{}(k.run_id) ^
                   (std::hash<std::string>{}(k.node_id) << 1) ^
                   (std::hash<int>{}(k.attempt) << 2);
        }
    };

    std::mutex                                                              mu_;
    std::unordered_map<std::string, WorkflowRunRecord>                      runs_;
    std::unordered_map<NodeKey, WorkflowNodeRunRecord, NodeKeyHash>         nodes_;
};

} // namespace aegisgate::workflow
