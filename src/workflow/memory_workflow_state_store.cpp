#include "workflow/memory_workflow_state_store.h"

#include <vector>

namespace aegisgate::workflow {

std::optional<WorkflowRunStatus> workflowRunStatusFromString(std::string_view s) {
    if (s == "pending")              return WorkflowRunStatus::Pending;
    if (s == "running")              return WorkflowRunStatus::Running;
    if (s == "waiting_for_approval") return WorkflowRunStatus::WaitingForApproval;
    if (s == "succeeded")            return WorkflowRunStatus::Succeeded;
    if (s == "failed")               return WorkflowRunStatus::Failed;
    if (s == "cancelled")            return WorkflowRunStatus::Cancelled;
    if (s == "dead_letter")          return WorkflowRunStatus::DeadLetter;
    return std::nullopt;
}

std::optional<WorkflowNodeStatus> workflowNodeStatusFromString(std::string_view s) {
    if (s == "pending")              return WorkflowNodeStatus::Pending;
    if (s == "running")              return WorkflowNodeStatus::Running;
    if (s == "succeeded")            return WorkflowNodeStatus::Succeeded;
    if (s == "failed")               return WorkflowNodeStatus::Failed;
    if (s == "skipped")              return WorkflowNodeStatus::Skipped;
    if (s == "waiting_for_approval") return WorkflowNodeStatus::WaitingForApproval;
    if (s == "dead_letter")          return WorkflowNodeStatus::DeadLetter;
    return std::nullopt;
}

bool MemoryWorkflowStateStore::createRun(const WorkflowRunRecord& r) {
    std::lock_guard<std::mutex> g(mu_);
    if (runs_.find(r.run_id) != runs_.end()) return false;
    runs_.emplace(r.run_id, r);
    return true;
}

std::optional<WorkflowRunRecord>
MemoryWorkflowStateStore::getRun(const std::string& run_id) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = runs_.find(run_id);
    if (it == runs_.end()) return std::nullopt;
    return it->second;
}

std::vector<WorkflowRunRecord>
MemoryWorkflowStateStore::listRuns(std::optional<WorkflowRunStatus> filter) {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<WorkflowRunRecord> out;
    out.reserve(runs_.size());
    for (const auto& kv : runs_) {
        if (filter && kv.second.status != *filter) continue;
        out.push_back(kv.second);
    }
    return out;
}

bool MemoryWorkflowStateStore::transitionRunStatus(const std::string& run_id,
                                                    WorkflowRunStatus new_status,
                                                    std::int64_t when_ms) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = runs_.find(run_id);
    if (it == runs_.end()) return false;
    it->second.status        = new_status;
    it->second.updated_at_ms = when_ms;
    return true;
}

bool MemoryWorkflowStateStore::updateRunContext(const std::string& run_id,
                                                 const std::string& new_context_json,
                                                 std::int64_t when_ms) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = runs_.find(run_id);
    if (it == runs_.end()) return false;
    it->second.context_json  = new_context_json;
    it->second.updated_at_ms = when_ms;
    return true;
}

bool MemoryWorkflowStateStore::upsertNodeRun(const WorkflowNodeRunRecord& n) {
    std::lock_guard<std::mutex> g(mu_);
    // I30：按 (run_id,node_id,attempt) 键 → 同 attempt 内状态转移覆盖，跨 attempt
    // 独立行（append 审计链，不再互相覆盖）。
    nodes_[{n.run_id, n.node_id, n.attempt}] = n;
    return true;
}

std::vector<WorkflowNodeRunRecord>
MemoryWorkflowStateStore::listNodeRuns(const std::string& run_id) {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<WorkflowNodeRunRecord> out;
    for (const auto& kv : nodes_) {
        if (kv.first.run_id == run_id) out.push_back(kv.second);
    }
    return out;
}

std::optional<WorkflowNodeRunRecord>
MemoryWorkflowStateStore::getNodeRun(const std::string& run_id,
                                       const std::string& node_id) {
    std::lock_guard<std::mutex> g(mu_);
    // I30：返回该节点最高 attempt 的记录（最新一次尝试），与 SQLite
    // `ORDER BY attempt DESC LIMIT 1` parity。
    const WorkflowNodeRunRecord* best = nullptr;
    for (const auto& kv : nodes_) {
        if (kv.first.run_id == run_id && kv.first.node_id == node_id) {
            if (!best || kv.first.attempt > best->attempt) best = &kv.second;
        }
    }
    if (!best) return std::nullopt;
    return *best;
}

int MemoryWorkflowStateStore::pruneOldRuns(std::int64_t cutoff_ms) {
    std::lock_guard<std::mutex> g(mu_);
    int pruned = 0;
    std::vector<std::string> ids_to_drop;
    for (const auto& kv : runs_) {
        const auto& r = kv.second;
        // Use whichever timestamp is younger so still-pending runs aren't
        // prematurely culled when only created_at_ms is populated.
        std::int64_t ts = r.updated_at_ms > 0 ? r.updated_at_ms : r.created_at_ms;
        if (ts < cutoff_ms) ids_to_drop.push_back(kv.first);
    }
    for (const auto& id : ids_to_drop) {
        runs_.erase(id);
        for (auto it = nodes_.begin(); it != nodes_.end(); ) {
            if (it->first.run_id == id) it = nodes_.erase(it);
            else                          ++it;
        }
        ++pruned;
    }
    return pruned;
}

} // namespace aegisgate::workflow
