#include "gateway/provider_spec/provider_manifest.h"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace aegisgate {

namespace {

// -----------------------------------------------------------------------------
// Constants — the canonical enums for this Manifest schema version.
// -----------------------------------------------------------------------------

constexpr const char* kApiVersionV1Alpha1 = "aegisgate.dev/v1alpha1";
constexpr const char* kKindProviderManifest = "ProviderManifest";

const std::unordered_set<std::string>& connectorKinds() {
    static const std::unordered_set<std::string> s{"openai", "claude", "external"};
    return s;
}

const std::unordered_set<std::string>& authTypes() {
    static const std::unordered_set<std::string> s{"bearer", "api_key_header", "query", "none"};
    return s;
}

const std::unordered_set<std::string>& compatLevels() {
    static const std::unordered_set<std::string> s{"full", "partial", "translated", "none"};
    return s;
}

const std::unordered_set<std::string>& fieldStatuses() {
    static const std::unordered_set<std::string> s{"supported", "unsupported", "translated", "ignored"};
    return s;
}

const std::unordered_set<std::string>& capabilityEnum() {
    static const std::unordered_set<std::string> s{
        "streaming", "tools", "vision", "response_format", "logprobs",
        "system_message", "temperature", "top_p", "max_tokens"
    };
    return s;
}

// OpenAI chat/completions request fields — used by fields-coverage conformance.
const std::unordered_set<std::string>& openAiChatFields() {
    static const std::unordered_set<std::string> s{
        "messages", "model", "temperature", "top_p", "max_tokens", "stream",
        "stop", "tools", "tool_choice", "response_format", "presence_penalty",
        "frequency_penalty", "seed", "n", "logit_bias", "user", "logprobs",
        "top_logprobs", "parallel_tool_calls"
    };
    return s;
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

bool isHttpUrl(const std::string& s) {
    return s.rfind("https://", 0) == 0 || s.rfind("http://", 0) == 0;
}

bool isValidName(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) {
        const bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

// Safe-access helpers around yaml-cpp. All return "" / false / {} on missing.
std::string yamlStr(const YAML::Node& node, const std::string& key,
                    const std::string& fallback = "") {
    if (!node || !node.IsMap()) return fallback;
    auto v = node[key];
    if (!v) return fallback;
    try { return v.as<std::string>(fallback); } catch (...) { return fallback; }
}

int yamlInt(const YAML::Node& node, const std::string& key, int fallback) {
    if (!node || !node.IsMap()) return fallback;
    auto v = node[key];
    if (!v) return fallback;
    try { return v.as<int>(fallback); } catch (...) { return fallback; }
}

bool yamlBool(const YAML::Node& node, const std::string& key, bool fallback) {
    if (!node || !node.IsMap()) return fallback;
    auto v = node[key];
    if (!v) return fallback;
    try { return v.as<bool>(fallback); } catch (...) { return fallback; }
}

std::vector<std::string> yamlStrList(const YAML::Node& node, const std::string& key) {
    std::vector<std::string> out;
    if (!node || !node.IsMap()) return out;
    auto v = node[key];
    if (!v || !v.IsSequence()) return out;
    for (const auto& item : v) {
        try { out.push_back(item.as<std::string>("")); } catch (...) {}
    }
    return out;
}

// Convert a YAML::Node subtree into nlohmann::json by serializing through
// YAML's emitter (yaml-cpp ↔ JSON round-trip is lossless for simple shapes).
nlohmann::json yamlNodeToJson(const YAML::Node& node) {
    if (!node) return nullptr;
    switch (node.Type()) {
        case YAML::NodeType::Null: return nullptr;
        case YAML::NodeType::Scalar: {
            const auto s = node.Scalar();
            // try int
            try {
                size_t pos = 0;
                int i = std::stoi(s, &pos);
                if (pos == s.size()) return i;
            } catch (...) {}
            // try bool
            if (s == "true") return true;
            if (s == "false") return false;
            return s;
        }
        case YAML::NodeType::Sequence: {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& child : node) {
                arr.push_back(yamlNodeToJson(child));
            }
            return arr;
        }
        case YAML::NodeType::Map: {
            nlohmann::json obj = nlohmann::json::object();
            for (const auto& kv : node) {
                obj[kv.first.as<std::string>("")] = yamlNodeToJson(kv.second);
            }
            return obj;
        }
        case YAML::NodeType::Undefined:
        default:
            return nullptr;
    }
}

} // namespace

// =============================================================================
// ValidationReport
// =============================================================================

bool ValidationReport::ok() const {
    for (const auto& i : issues) {
        if (i.severity == ValidationIssue::Severity::Error) return false;
    }
    return true;
}

size_t ValidationReport::errorCount() const {
    size_t n = 0;
    for (const auto& i : issues) {
        if (i.severity == ValidationIssue::Severity::Error) ++n;
    }
    return n;
}

size_t ValidationReport::warningCount() const {
    size_t n = 0;
    for (const auto& i : issues) {
        if (i.severity == ValidationIssue::Severity::Warning) ++n;
    }
    return n;
}

void ValidationReport::addError(std::string code, std::string message,
                                std::string field_path) {
    issues.push_back({ValidationIssue::Severity::Error,
                      std::move(code), std::move(message), std::move(field_path)});
}

void ValidationReport::addWarning(std::string code, std::string message,
                                  std::string field_path) {
    issues.push_back({ValidationIssue::Severity::Warning,
                      std::move(code), std::move(message), std::move(field_path)});
}

void ValidationReport::merge(const ValidationReport& other) {
    for (const auto& i : other.issues) issues.push_back(i);
}

// =============================================================================
// Parsing
// =============================================================================

std::optional<ProviderManifest> loadManifestFromYaml(
    const std::string& yaml_text,
    ValidationReport& report) {
    if (yaml_text.empty()) {
        report.addError("manifest.yaml.empty", "Manifest YAML is empty");
        return std::nullopt;
    }

    YAML::Node root;
    try {
        root = YAML::Load(yaml_text);
    } catch (const std::exception& e) {
        report.addError("manifest.yaml.parse_error",
                        std::string("YAML parse error: ") + e.what());
        return std::nullopt;
    } catch (...) {
        report.addError("manifest.yaml.parse_error", "YAML parse error");
        return std::nullopt;
    }

    if (!root || !root.IsMap()) {
        report.addError("manifest.yaml.not_a_map",
                        "Manifest YAML must have a top-level map");
        return std::nullopt;
    }

    ProviderManifest m;
    m.api_version = yamlStr(root, "apiVersion");
    m.kind = yamlStr(root, "kind");

    // Top-level structural checks — reported here so callers need not re-run
    // validateManifest to catch the most common mistakes.
    if (m.api_version != kApiVersionV1Alpha1) {
        report.addError("manifest.apiVersion.unknown",
                        "Unknown apiVersion '" + m.api_version +
                        "'; expected " + kApiVersionV1Alpha1,
                        "apiVersion");
    }
    if (m.kind != kKindProviderManifest) {
        report.addError("manifest.kind.unknown",
                        "Unknown kind '" + m.kind + "'; expected ProviderManifest",
                        "kind");
    }

    // --- metadata ---
    auto meta = root["metadata"];
    m.name = yamlStr(meta, "name");
    m.display_name = yamlStr(meta, "display_name");
    m.vendor = yamlStr(meta, "vendor");
    m.homepage = yamlStr(meta, "homepage");
    m.documentation = yamlStr(meta, "documentation");
    m.tags = yamlStrList(meta, "tags");
    m.maturity = yamlStr(meta, "maturity", "preview");

    // --- spec ---
    auto spec = root["spec"];

    // connector
    auto conn = spec ? spec["connector"] : YAML::Node{};
    m.connector_kind = yamlStr(conn, "kind");

    // endpoint
    auto ep = spec ? spec["endpoint"] : YAML::Node{};
    m.base_url_default = yamlStr(ep, "base_url_default");
    m.base_url_env = yamlStr(ep, "base_url_env");
    m.timeout_ms_default = yamlInt(ep, "timeout_ms_default", 30000);
    m.max_retries_default = yamlInt(ep, "max_retries_default", 2);

    // auth
    auto auth = spec ? spec["auth"] : YAML::Node{};
    m.auth_type = yamlStr(auth, "type");
    m.auth_header_name = yamlStr(auth, "header_name");
    m.auth_env_var = yamlStr(auth, "env_var");
    m.auth_supports_multi_key = yamlBool(auth, "supports_multi_key", false);

    // compatibility
    auto compat = spec ? spec["compatibility"] : YAML::Node{};
    m.openai_chat_compat = yamlStr(compat, "openai_chat_completions");
    if (compat && compat.IsMap()) {
        auto fields = compat["fields"];
        if (fields && fields.IsSequence()) {
            for (const auto& f : fields) {
                ManifestFieldCompat fc;
                fc.name = yamlStr(f, "name");
                fc.status = yamlStr(f, "status");
                fc.notes = yamlStr(f, "notes");
                m.fields.push_back(std::move(fc));
            }
        }
    }

    // capabilities
    m.capabilities = spec ? yamlStrList(spec, "capabilities") : std::vector<std::string>{};

    // models
    if (spec) {
        auto models = spec["models"];
        if (models && models.IsSequence()) {
            for (const auto& model : models) {
                ManifestModel mm;
                mm.id = yamlStr(model, "id");
                mm.max_context_tokens = yamlInt(model, "max_context_tokens", 0);
                mm.capabilities = yamlStrList(model, "capabilities");
                mm.region_hints = yamlStrList(model, "region_hints");
                m.models.push_back(std::move(mm));
            }
        }

        // conformance
        auto conf = spec["conformance"];
        if (conf && conf.IsMap()) {
            m.conformance.required_checks = yamlStrList(conf, "required_checks");
            if (auto sr = conf["sample_request"]) {
                m.conformance.sample_request = yamlNodeToJson(sr);
            }
            if (auto sr = conf["sample_response_shape"]) {
                m.conformance.sample_response_shape = yamlNodeToJson(sr);
            }
        }
    }

    return m;
}

std::optional<ProviderManifest> loadManifestFromFile(
    const std::string& path,
    ValidationReport& report) {
    std::ifstream ifs(path);
    if (!ifs) {
        report.addError("manifest.file.open_failed",
                        "Failed to open manifest file: " + path);
        return std::nullopt;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    return loadManifestFromYaml(ss.str(), report);
}

// =============================================================================
// Validation
// =============================================================================

ValidationReport validateManifest(const ProviderManifest& m) {
    ValidationReport r;

    if (m.api_version != kApiVersionV1Alpha1) {
        r.addError("manifest.apiVersion.unknown",
                   "Unknown apiVersion; expected " + std::string(kApiVersionV1Alpha1),
                   "apiVersion");
    }
    if (m.kind != kKindProviderManifest) {
        r.addError("manifest.kind.unknown",
                   "Unknown kind; expected ProviderManifest",
                   "kind");
    }

    if (m.name.empty()) {
        r.addError("metadata.name.empty", "metadata.name must not be empty",
                   "metadata.name");
    } else if (!isValidName(m.name)) {
        r.addError("metadata.name.invalid",
                   "metadata.name must match [a-z0-9_-]{1,64}",
                   "metadata.name");
    }

    if (m.connector_kind.empty() || !connectorKinds().count(m.connector_kind)) {
        r.addError("spec.connector.kind.unknown",
                   "Unknown connector.kind; expected openai, claude, or external",
                   "spec.connector.kind");
    }

    if (m.base_url_default.empty() || !isHttpUrl(m.base_url_default)) {
        r.addError("spec.endpoint.base_url_default.invalid",
                   "spec.endpoint.base_url_default must be an http(s):// URL",
                   "spec.endpoint.base_url_default");
    }

    if (m.auth_type.empty() || !authTypes().count(m.auth_type)) {
        r.addError("spec.auth.type.unknown",
                   "Unknown auth.type; expected bearer, api_key_header, query, or none",
                   "spec.auth.type");
    }

    if (!m.openai_chat_compat.empty() && !compatLevels().count(m.openai_chat_compat)) {
        r.addError("spec.compatibility.openai_chat_completions.unknown",
                   "Unknown openai_chat_completions value; expected full, partial, translated, or none",
                   "spec.compatibility.openai_chat_completions");
    }

    // fields[].status in enum
    for (size_t i = 0; i < m.fields.size(); ++i) {
        const auto& f = m.fields[i];
        if (!f.status.empty() && !fieldStatuses().count(f.status)) {
            r.addError("spec.compatibility.fields.status.unknown",
                       "Unknown field status for '" + f.name + "': " + f.status,
                       "spec.compatibility.fields[" + std::to_string(i) + "].status");
        }
    }

    // duplicate models[].id
    std::unordered_set<std::string> seen_ids;
    for (size_t i = 0; i < m.models.size(); ++i) {
        const auto& mm = m.models[i];
        if (mm.id.empty()) {
            r.addError("spec.models.id.empty",
                       "models[" + std::to_string(i) + "].id must not be empty",
                       "spec.models[" + std::to_string(i) + "].id");
            continue;
        }
        if (!seen_ids.insert(mm.id).second) {
            r.addError("spec.models.id.duplicate",
                       "Duplicate model id: " + mm.id,
                       "spec.models[" + std::to_string(i) + "].id");
        }
    }

    return r;
}

// =============================================================================
// Conformance
// =============================================================================

ValidationReport runConformanceChecks(const ProviderManifest& m) {
    ValidationReport r;

    // conformance.auth-defined — bearer / api_key_header must provide header_name or env_var
    if (m.auth_type == "bearer" || m.auth_type == "api_key_header") {
        if (m.auth_header_name.empty() && m.auth_env_var.empty()) {
            r.addWarning("conformance.auth-defined",
                         "auth.type '" + m.auth_type +
                         "' without header_name or env_var is ambiguous",
                         "spec.auth");
        }
    }

    // conformance.compatibility-declared — kind=claude with full compat is suspect
    if (m.connector_kind == "claude" && m.openai_chat_compat == "full") {
        r.addWarning("conformance.compatibility-declared",
                     "connector.kind=claude with openai_chat_completions=full "
                     "is unusual; Claude requests are typically 'translated'",
                     "spec.compatibility.openai_chat_completions");
    }

    // conformance.fields-coverage — declared fields should be OpenAI-known
    for (const auto& f : m.fields) {
        if (!f.name.empty() && !openAiChatFields().count(f.name)) {
            r.addWarning("conformance.fields-coverage",
                         "Declared field '" + f.name +
                         "' is not a known OpenAI chat/completions field",
                         "spec.compatibility.fields[]");
        }
    }

    // conformance.capability-enum — capabilities must be from enum
    for (const auto& cap : m.capabilities) {
        if (!capabilityEnum().count(cap)) {
            r.addError("conformance.capability-enum",
                       "Unknown capability: " + cap,
                       "spec.capabilities[]");
        }
    }

    // conformance.models-unique — duplicates already caught by validateManifest,
    // but we repeat here so `runConformanceChecks` alone is meaningful.
    std::unordered_set<std::string> seen;
    for (const auto& mm : m.models) {
        if (mm.id.empty()) continue;
        if (!seen.insert(mm.id).second) {
            r.addError("conformance.models-unique",
                       "Duplicate model id: " + mm.id,
                       "spec.models[]");
        }
    }

    // conformance.sample-request-shape — if sample_request declared,
    // must include non-empty messages[] and model
    if (!m.conformance.sample_request.is_null()) {
        const auto& sr = m.conformance.sample_request;
        const bool has_messages = sr.is_object() &&
                                   sr.contains("messages") &&
                                   sr["messages"].is_array() &&
                                   !sr["messages"].empty();
        if (!has_messages) {
            r.addError("conformance.sample-request-shape",
                       "sample_request.messages must be a non-empty array",
                       "spec.conformance.sample_request.messages");
        }
        if (sr.is_object() && !sr.contains("model")) {
            r.addError("conformance.sample-request-shape",
                       "sample_request must include 'model'",
                       "spec.conformance.sample_request.model");
        }
    }

    return r;
}

} // namespace aegisgate
