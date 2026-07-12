#pragma once

// Phase 11.3 TASK-20260523-02 — Workflow DSL data model.
//
// The DSL serialises a DAG of stages (NodeSpec) plus their dependencies,
// retries, timeouts, and the human-approval gate. It is the canonical
// representation the Engine, StateStore and AuditLogger all agree on.
//
// canonicalHash() defends T01 tampering between propose and execute, mirroring
// the payload_sha256 pattern from Phase 11.5 ApprovalProposal.
//
// Design references:
//   docs/specs/2026-05-23-phase11.3-workflow-2.0-design.md §4.1
//   docs/plans/2026-05-23-phase11.3-workflow-2.0.md §1.1 Epic 1.1

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace aegisgate::workflow {

enum class NodeType {
    Tool          = 0,
    HumanApproval = 1,
    Conditional   = 2,
    FanOut        = 3,
    FanIn         = 4,
};

inline std::string toString(NodeType t) {
    switch (t) {
        case NodeType::Tool:          return "tool";
        case NodeType::HumanApproval: return "human_approval";
        case NodeType::Conditional:   return "conditional";
        case NodeType::FanOut:        return "fan_out";
        case NodeType::FanIn:         return "fan_in";
    }
    return "tool";
}

inline std::optional<NodeType> nodeTypeFromString(std::string_view s) {
    if (s == "tool")           return NodeType::Tool;
    if (s == "human_approval") return NodeType::HumanApproval;
    if (s == "conditional")    return NodeType::Conditional;
    if (s == "fan_out")        return NodeType::FanOut;
    if (s == "fan_in")         return NodeType::FanIn;
    return std::nullopt;
}

struct RetryPolicy {
    int  max_attempts = 1;   // 1 = no retry
    int  backoff_ms   = 0;
    bool exponential  = false;
};

struct NodeSpec {
    std::string                id;
    NodeType                   type        = NodeType::Tool;
    std::string                tool_id;          // for Tool / HumanApproval
    nlohmann::json             arguments   = nlohmann::json::object();
    std::vector<std::string>   depends_on;
    std::string                condition;        // optional JSON-path expression
    RetryPolicy                retry;
    int                        timeout_ms  = 30000;
    std::string                dead_letter_log;  // optional sink path
};

struct WorkflowDsl {
    std::string              id;
    std::string              version;
    std::string              description;
    std::vector<NodeSpec>    nodes;
    nlohmann::json           metadata = nlohmann::json::object();
};

// --- JSON bridge ----------------------------------------------------------

nlohmann::json                   toJson(const WorkflowDsl& dsl);
std::optional<WorkflowDsl>       fromJson(const nlohmann::json& j);

// --- canonical hashing (T01 mitigation) -----------------------------------

// Returns a hex-encoded sha256 over a canonical-form JSON of the DSL.
// `description` and `metadata` are deliberately excluded so cosmetic edits
// do not invalidate proposals already in the AutonomyApprovalWorkflow queue.
std::string canonicalHash(const WorkflowDsl& dsl);

} // namespace aegisgate::workflow
