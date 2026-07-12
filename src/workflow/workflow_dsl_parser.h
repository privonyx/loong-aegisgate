#pragma once

// Phase 11.3 TASK-20260523-02 — Workflow DSL parser + validators.
//
// parseWorkflowDslYaml / parseWorkflowDslJson read the wire format the Engine
// accepts. The two validators guard SR-NEW3 (cycle detection) and SR3
// (sandbox bypass — see workflow_dsl_validator).
//
// Errors collected into a vector so callers (Admin UI, audit) can surface
// all problems at once instead of dribbling them out one parse-call apart.
//
// Design reference: docs/specs/2026-05-23-phase11.3-workflow-2.0-design.md §4.1

#include "workflow/workflow_dsl.h"

#include <optional>
#include <string>
#include <vector>

namespace aegisgate {
class ToolRegistry;
} // namespace aegisgate

namespace aegisgate::workflow {

struct WorkflowDslParseResult {
    bool                       ok = false;
    std::optional<WorkflowDsl> dsl;
    std::vector<std::string>   errors;
};

WorkflowDslParseResult parseWorkflowDslYaml(const std::string& yaml_text);
WorkflowDslParseResult parseWorkflowDslJson(const std::string& json_text);

// Kahn topological scan. Returns false on any cycle (including self-loop).
// When error_node_id_out != nullptr it receives the id of one offending node.
bool validateNoCycle(const WorkflowDsl& dsl,
                     std::string* error_node_id_out = nullptr);

// SR3 sandbox bypass detection. Every NodeType::Tool / HumanApproval node
// must carry a non-empty tool_id and (when registry != nullptr) the tool_id
// must resolve in ToolRegistry. Empty registry → only the "non-empty"
// invariant is enforced (used by tests + parser-time best-effort scan).
// offending_node_ids_out collects every violator (not just the first).
bool validateNoSandboxBypass(const WorkflowDsl& dsl,
                              const ToolRegistry* registry,
                              std::vector<std::string>* offending_node_ids_out = nullptr);

} // namespace aegisgate::workflow
