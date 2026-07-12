#include "workflow/human_approval_node_handler.h"

#include "core/crypto.h"
#include "guardrail/inbound/pii_filter.h"
#include "observe/autonomy/approval_workflow.h"

#include <chrono>
#include <utility>

namespace aegisgate::workflow {

HumanApprovalNodeHandler::HumanApprovalNodeHandler(
    std::shared_ptr<aegisgate::autonomy::AutonomyApprovalWorkflow> workflow,
    std::shared_ptr<aegisgate::PIIFilter>                          pii_filter)
    : workflow_(std::move(workflow)), pii_filter_(std::move(pii_filter)) {}

nlohmann::json HumanApprovalNodeHandler::scrub(nlohmann::json v) const {
    if (!pii_filter_) return v;
    if (v.is_string()) {
        return pii_filter_->mask(v.get<std::string>());
    }
    if (v.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        for (auto it = v.begin(); it != v.end(); ++it) {
            out[it.key()] = scrub(it.value());
        }
        return out;
    }
    if (v.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& el : v) out.push_back(scrub(el));
        return out;
    }
    return v;
}

std::string HumanApprovalNodeHandler::enqueue(const std::string& run_id,
                                               const std::string& workflow_id,
                                               const std::string& dsl_hash,
                                               const NodeSpec&    node,
                                               const nlohmann::json& context) {
    if (!workflow_) return {};

    aegisgate::autonomy::ApprovalProposal p;
    p.source  = aegisgate::autonomy::AutonomySource::Workflow;
    p.subject = "workflow:" + workflow_id + "/" + node.id;

    nlohmann::json payload;
    payload["run_id"]      = run_id;
    payload["workflow_id"] = workflow_id;
    payload["node_id"]     = node.id;
    payload["dsl_hash"]    = dsl_hash;
    payload["tool_id"]     = node.tool_id;
    payload["arguments"]   = node.arguments;
    payload["context"]     = context;
    p.payload = scrub(std::move(payload));

    nlohmann::json trace;
    trace["source_id"]         = run_id;
    trace["algorithm_name"]    = "workflow_human_approval_v1";
    trace["input_hash_sha256"] = crypto::sha256(dsl_hash + "|" + node.id);
    trace["proposed_at_ms"]    =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    p.decision_trace = scrub(std::move(trace));

    return workflow_->propose(std::move(p));
}

} // namespace aegisgate::workflow
