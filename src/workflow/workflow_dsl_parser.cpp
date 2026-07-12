#include "workflow/workflow_dsl_parser.h"

#include "agent/tool_registry.h"

#include <yaml-cpp/yaml.h>

#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace aegisgate::workflow {

namespace {

// --- YAML helpers ---------------------------------------------------------

nlohmann::json yamlScalarToJson(const YAML::Node& n) {
    if (!n) return nullptr;
    if (n.IsNull()) return nullptr;
    try {
        // Try int / double / bool first to preserve types in YAML.
        try { return n.as<bool>(); } catch (...) {}
        try { return n.as<long long>(); } catch (...) {}
        try { return n.as<double>(); } catch (...) {}
    } catch (...) {}
    return n.as<std::string>("");
}

nlohmann::json yamlToJson(const YAML::Node& n) {
    if (!n || n.IsNull()) return nullptr;
    if (n.IsScalar()) return yamlScalarToJson(n);
    if (n.IsSequence()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& el : n) arr.push_back(yamlToJson(el));
        return arr;
    }
    if (n.IsMap()) {
        nlohmann::json obj = nlohmann::json::object();
        for (const auto& kv : n) {
            obj[kv.first.as<std::string>()] = yamlToJson(kv.second);
        }
        return obj;
    }
    return nullptr;
}

// Convert parsed YAML node -> NodeSpec ---------------------------------------

bool parseNode(const YAML::Node& yn, NodeSpec& out, std::vector<std::string>& errors) {
    if (!yn.IsMap()) {
        errors.emplace_back("node entry is not a map");
        return false;
    }
    try {
        out.id = yn["id"].as<std::string>("");
        if (out.id.empty()) {
            errors.emplace_back("node missing 'id'");
            return false;
        }
        std::string type_str = yn["type"].as<std::string>("tool");
        auto t = nodeTypeFromString(type_str);
        if (!t) {
            errors.emplace_back("node " + out.id + ": unknown type '" + type_str + "'");
            return false;
        }
        out.type    = *t;
        out.tool_id = yn["tool_id"].as<std::string>("");
        if (yn["arguments"]) out.arguments = yamlToJson(yn["arguments"]);
        if (yn["depends_on"] && yn["depends_on"].IsSequence()) {
            for (const auto& d : yn["depends_on"]) {
                out.depends_on.push_back(d.as<std::string>(""));
            }
        }
        out.condition = yn["condition"].as<std::string>("");
        if (yn["retry"] && yn["retry"].IsMap()) {
            out.retry.max_attempts = yn["retry"]["max_attempts"].as<int>(1);
            out.retry.backoff_ms   = yn["retry"]["backoff_ms"].as<int>(0);
            out.retry.exponential  = yn["retry"]["exponential"].as<bool>(false);
        }
        out.timeout_ms       = yn["timeout_ms"].as<int>(30000);
        out.dead_letter_log  = yn["dead_letter_log"].as<std::string>("");
        return true;
    } catch (const YAML::Exception& e) {
        errors.emplace_back(std::string("node parse error: ") + e.what());
        return false;
    }
}

// Common cross-node checks shared by YAML/JSON parsers.
bool runStructuralChecks(const WorkflowDsl& dsl, std::vector<std::string>& errors) {
    // 1. duplicate ids
    std::unordered_set<std::string> seen;
    for (const auto& n : dsl.nodes) {
        if (!seen.insert(n.id).second) {
            errors.emplace_back("duplicate node id '" + n.id + "'");
        }
    }
    // 2. missing dependency targets
    for (const auto& n : dsl.nodes) {
        for (const auto& d : n.depends_on) {
            if (!seen.count(d)) {
                errors.emplace_back("node '" + n.id + "' depends on missing node '" + d + "'");
            }
        }
    }
    return errors.empty();
}

} // namespace

WorkflowDslParseResult parseWorkflowDslYaml(const std::string& yaml_text) {
    WorkflowDslParseResult r;
    YAML::Node root;
    try {
        root = YAML::Load(yaml_text);
    } catch (const YAML::Exception& e) {
        r.errors.emplace_back(std::string("yaml load failed: ") + e.what());
        return r;
    }
    if (!root || !root.IsMap()) {
        r.errors.emplace_back("yaml root must be a map");
        return r;
    }
    WorkflowDsl dsl;
    dsl.id          = root["id"].as<std::string>("");
    dsl.version     = root["version"].as<std::string>("");
    dsl.description = root["description"].as<std::string>("");
    if (root["metadata"]) dsl.metadata = yamlToJson(root["metadata"]);

    if (!root["nodes"] || !root["nodes"].IsSequence()) {
        r.errors.emplace_back("yaml missing 'nodes' sequence");
        return r;
    }
    for (const auto& yn : root["nodes"]) {
        NodeSpec n;
        if (parseNode(yn, n, r.errors)) {
            dsl.nodes.push_back(std::move(n));
        }
    }

    runStructuralChecks(dsl, r.errors);
    if (!r.errors.empty()) return r;

    r.ok  = true;
    r.dsl = std::move(dsl);
    return r;
}

WorkflowDslParseResult parseWorkflowDslJson(const std::string& json_text) {
    WorkflowDslParseResult r;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_text);
    } catch (const nlohmann::json::parse_error& e) {
        r.errors.emplace_back(std::string("json parse failed: ") + e.what());
        return r;
    }
    auto dsl_opt = fromJson(j);
    if (!dsl_opt) {
        r.errors.emplace_back("json shape did not match WorkflowDsl");
        return r;
    }
    runStructuralChecks(*dsl_opt, r.errors);
    if (!r.errors.empty()) return r;
    r.ok  = true;
    r.dsl = std::move(dsl_opt);
    return r;
}

bool validateNoCycle(const WorkflowDsl& dsl, std::string* error_node_id_out) {
    // Kahn topological sort: nodes with in-degree 0 enter the queue; each pop
    // decrements its successors. If the visited count != dsl.nodes.size(),
    // there is a cycle.
    std::unordered_map<std::string, int> in_degree;
    std::unordered_map<std::string, std::vector<std::string>> succ;
    for (const auto& n : dsl.nodes) {
        in_degree.emplace(n.id, 0);
    }
    for (const auto& n : dsl.nodes) {
        for (const auto& d : n.depends_on) {
            // self-loop is the simplest cycle and Kahn would still flag it
            // through the visited != total check, but we surface the offender
            // explicitly for the audit trail.
            if (d == n.id) {
                if (error_node_id_out) *error_node_id_out = n.id;
                return false;
            }
            succ[d].push_back(n.id);
            in_degree[n.id]++;
        }
    }
    std::queue<std::string> q;
    for (const auto& kv : in_degree) {
        if (kv.second == 0) q.push(kv.first);
    }
    int visited = 0;
    while (!q.empty()) {
        auto cur = q.front();
        q.pop();
        visited++;
        for (const auto& s : succ[cur]) {
            if (--in_degree[s] == 0) q.push(s);
        }
    }
    if (visited != static_cast<int>(dsl.nodes.size())) {
        // surface one offender: a node that never reached in_degree 0
        if (error_node_id_out) {
            for (const auto& kv : in_degree) {
                if (kv.second > 0) { *error_node_id_out = kv.first; break; }
            }
        }
        return false;
    }
    return true;
}

bool validateNoSandboxBypass(const WorkflowDsl& dsl,
                              const ToolRegistry* registry,
                              std::vector<std::string>* offending_node_ids_out) {
    bool ok = true;
    for (const auto& n : dsl.nodes) {
        if (n.type == NodeType::FanOut || n.type == NodeType::FanIn ||
            n.type == NodeType::Conditional) {
            continue;  // pure topology nodes never invoke executors
        }
        // SR3 invariant #1: tool_id MUST NOT be empty for any executable node.
        if (n.tool_id.empty()) {
            if (offending_node_ids_out) offending_node_ids_out->push_back(n.id);
            ok = false;
            continue;
        }
        // SR3 invariant #2: when a registry is provided, the tool_id MUST be
        // registered. Phase 11.3 v1 lets HumanApproval target a reviewer role
        // (any non-empty string) so we only enforce registry membership for
        // NodeType::Tool.
        if (registry && n.type == NodeType::Tool) {
            if (!registry->getTool(n.tool_id).has_value()) {
                if (offending_node_ids_out) offending_node_ids_out->push_back(n.id);
                ok = false;
            }
        }
    }
    return ok;
}

} // namespace aegisgate::workflow
