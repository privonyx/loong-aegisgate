#include "workflow/workflow_dsl.h"

#include "core/crypto.h"

#include <map>

namespace aegisgate::workflow {

namespace {

nlohmann::json canonicalise(const nlohmann::json& v) {
    using json = nlohmann::json;
    if (v.is_object()) {
        std::map<std::string, json> sorted;
        for (auto it = v.begin(); it != v.end(); ++it) {
            sorted.emplace(it.key(), canonicalise(it.value()));
        }
        json out = json::object();
        for (auto& kv : sorted) out[kv.first] = std::move(kv.second);
        return out;
    }
    if (v.is_array()) {
        json out = json::array();
        for (const auto& el : v) out.push_back(canonicalise(el));
        return out;
    }
    return v;
}

nlohmann::json retryToJson(const RetryPolicy& r) {
    return {{"max_attempts", r.max_attempts},
            {"backoff_ms",   r.backoff_ms},
            {"exponential",  r.exponential}};
}

RetryPolicy retryFromJson(const nlohmann::json& j) {
    RetryPolicy r;
    if (j.is_object()) {
        if (j.contains("max_attempts") && j["max_attempts"].is_number_integer())
            r.max_attempts = j["max_attempts"].get<int>();
        if (j.contains("backoff_ms") && j["backoff_ms"].is_number_integer())
            r.backoff_ms = j["backoff_ms"].get<int>();
        if (j.contains("exponential") && j["exponential"].is_boolean())
            r.exponential = j["exponential"].get<bool>();
    }
    return r;
}

nlohmann::json nodeToJson(const NodeSpec& n) {
    nlohmann::json j;
    j["id"]               = n.id;
    j["type"]             = toString(n.type);
    j["tool_id"]          = n.tool_id;
    j["arguments"]        = n.arguments;
    j["depends_on"]       = n.depends_on;
    j["condition"]        = n.condition;
    j["retry"]            = retryToJson(n.retry);
    j["timeout_ms"]       = n.timeout_ms;
    j["dead_letter_log"]  = n.dead_letter_log;
    return j;
}

std::optional<NodeSpec> nodeFromJson(const nlohmann::json& j) {
    if (!j.is_object()) return std::nullopt;
    NodeSpec n;
    if (j.contains("id") && j["id"].is_string()) n.id = j["id"].get<std::string>();
    if (n.id.empty()) return std::nullopt;
    if (j.contains("type") && j["type"].is_string()) {
        auto t = nodeTypeFromString(j["type"].get<std::string>());
        if (!t) return std::nullopt;
        n.type = *t;
    }
    if (j.contains("tool_id") && j["tool_id"].is_string())
        n.tool_id = j["tool_id"].get<std::string>();
    if (j.contains("arguments")) n.arguments = j["arguments"];
    if (j.contains("depends_on") && j["depends_on"].is_array()) {
        for (const auto& d : j["depends_on"]) {
            if (d.is_string()) n.depends_on.push_back(d.get<std::string>());
        }
    }
    if (j.contains("condition") && j["condition"].is_string())
        n.condition = j["condition"].get<std::string>();
    if (j.contains("retry")) n.retry = retryFromJson(j["retry"]);
    if (j.contains("timeout_ms") && j["timeout_ms"].is_number_integer())
        n.timeout_ms = j["timeout_ms"].get<int>();
    if (j.contains("dead_letter_log") && j["dead_letter_log"].is_string())
        n.dead_letter_log = j["dead_letter_log"].get<std::string>();
    return n;
}

} // namespace

nlohmann::json toJson(const WorkflowDsl& dsl) {
    nlohmann::json out;
    out["id"]          = dsl.id;
    out["version"]     = dsl.version;
    out["description"] = dsl.description;
    out["metadata"]    = dsl.metadata;
    nlohmann::json nodes = nlohmann::json::array();
    for (const auto& n : dsl.nodes) nodes.push_back(nodeToJson(n));
    out["nodes"] = std::move(nodes);
    return out;
}

std::optional<WorkflowDsl> fromJson(const nlohmann::json& j) {
    if (!j.is_object()) return std::nullopt;
    WorkflowDsl dsl;
    if (j.contains("id") && j["id"].is_string()) dsl.id = j["id"].get<std::string>();
    if (j.contains("version") && j["version"].is_string())
        dsl.version = j["version"].get<std::string>();
    if (j.contains("description") && j["description"].is_string())
        dsl.description = j["description"].get<std::string>();
    if (j.contains("metadata") && j["metadata"].is_object())
        dsl.metadata = j["metadata"];
    if (j.contains("nodes") && j["nodes"].is_array()) {
        for (const auto& nj : j["nodes"]) {
            auto n = nodeFromJson(nj);
            if (!n) return std::nullopt;
            dsl.nodes.push_back(std::move(*n));
        }
    }
    return dsl;
}

std::string canonicalHash(const WorkflowDsl& dsl) {
    nlohmann::json canon;
    canon["id"]      = dsl.id;
    canon["version"] = dsl.version;
    nlohmann::json nodes = nlohmann::json::array();
    for (const auto& n : dsl.nodes) nodes.push_back(nodeToJson(n));
    canon["nodes"] = std::move(nodes);
    return crypto::sha256(canonicalise(canon).dump());
}

} // namespace aegisgate::workflow
