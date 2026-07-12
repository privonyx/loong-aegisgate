#include "workflow/workflow_approval_applier.h"

#include "workflow/workflow_engine.h"
#include "workflow/workflow_state_store.h"

#include <spdlog/spdlog.h>

#include <unordered_set>

namespace aegisgate::workflow {

using aegisgate::autonomy::ApplyResult;
using aegisgate::autonomy::ApprovalProposal;
using aegisgate::autonomy::BaseAutonomyApplier;

namespace {

const std::unordered_set<std::string>& lowRiskToolSafelist() {
    static const std::unordered_set<std::string> kSafe = {
        "read_only_metrics_lookup",
        "audit_log_query",
        "shadow_inference",
        "tracing_lookup",
    };
    return kSafe;
}

} // namespace

WorkflowApprovalApplier::WorkflowApprovalApplier(WorkflowEngine*      engine,
                                                  IWorkflowStateStore* state_store)
    : engine_(engine), state_store_(state_store) {}

ApplyResult WorkflowApprovalApplier::applyImpl(const ApprovalProposal& p,
                                                bool dry_run) {
    if (!p.payload.is_object()) {
        return BaseAutonomyApplier::makeFailSchemaInvalid("payload not an object");
    }
    std::string run_id  = p.payload.value("run_id", std::string{});
    std::string node_id = p.payload.value("node_id", std::string{});
    std::string dsl_hash = p.payload.value("dsl_hash", std::string{});
    if (run_id.empty() || node_id.empty()) {
        return BaseAutonomyApplier::makeFailSchemaInvalid(
            "payload missing run_id or node_id");
    }

    if (!engine_ || !state_store_) {
        return BaseAutonomyApplier::makeFailMissingDep(
            "engine_unavailable", "applier wiring broken");
    }

    if (dry_run) {
        return BaseAutonomyApplier::makeDryRunOk(
            {{"run_id", run_id}, {"node_id", node_id}});
    }

    // T01 — verify the stored dsl_hash matches the proposal payload.
    auto run = state_store_->getRun(run_id);
    if (!run) {
        return ApplyResult::fail("run_not_found", run_id);
    }
    if (!dsl_hash.empty() && dsl_hash != run->dsl_hash) {
        return ApplyResult::fail("dsl_hash_mismatch",
                                  "proposal payload diverged from stored DSL");
    }

    auto res = engine_->resume(run_id);
    nlohmann::json det{
        {"run_id",        run_id},
        {"node_id",       node_id},
        {"final_status",  toString(res.final_status)},
        {"completed",     res.completed_nodes},
        {"failed",        res.failed_nodes},
    };
    if (res.ok) {
        auto r = ApplyResult::ok();
        r.details = std::move(det);
        return r;  // base wrapper fills duration_ms
    }
    return ApplyResult::fail("resume_failed",
                              res.error_message.empty() ? "engine refused"
                                                         : res.error_message,
                              std::move(det));
}

ApplyResult WorkflowApprovalApplier::rollbackImpl(const ApprovalProposal& p) {
    // Workflow rollback is a no-op in v1: HumanApproval nodes are decision
    // points; reversing a resume() would require side-effect recording of
    // downstream tool calls, which the v1 spec defers to Workflow v2.
    nlohmann::json det{
        {"run_id",  p.payload.value("run_id", std::string{})},
        {"node_id", p.payload.value("node_id", std::string{})},
        {"note",    "v1 rollback is a no-op"},
    };
    return ApplyResult::ok(std::move(det));  // base wrapper fills duration_ms
}

bool WorkflowApprovalApplier::isLowRisk(const ApprovalProposal& p) const {
    if (!p.payload.is_object()) return false;
    // Rule 1 — tool_id is in the curated safelist.
    auto tool_id = p.payload.value("tool_id", std::string{});
    if (!lowRiskToolSafelist().count(tool_id)) return false;
    // Rule 2 — argument footprint is small (< 5 keys).
    auto args = p.payload.value("arguments", nlohmann::json::object());
    if (!args.is_object() || args.size() >= 5) return false;
    // Rule 3 — timeout is short (<= 10s).
    int timeout = p.payload.value("timeout_ms", 30000);
    if (timeout > 10000) return false;
    // Rule 4 — explicit operator-vetted tag "low_risk_audited".
    if (!p.payload.contains("tags") || !p.payload["tags"].is_array()) {
        return false;
    }
    for (const auto& t : p.payload["tags"]) {
        if (t.is_string() && t.get<std::string>() == "low_risk_audited") {
            return true;
        }
    }
    return false;
}

} // namespace aegisgate::workflow
