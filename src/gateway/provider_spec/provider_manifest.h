#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace aegisgate {

// One Manifest-level `fields[]` entry — describes how a single OpenAI chat
// completions request field is handled by this provider.
struct ManifestFieldCompat {
    std::string name;
    std::string status;   // supported | unsupported | translated | ignored
    std::string notes;
};

// A Manifest-declared model. Purely a contract; runtime still uses
// `ProviderConfig::models` for the actual registry.
struct ManifestModel {
    std::string id;
    int max_context_tokens = 0;
    std::vector<std::string> capabilities;
    std::vector<std::string> region_hints;
};

// Conformance expectations — runner uses these to materialize shape-checks.
struct ManifestConformance {
    std::vector<std::string> required_checks;
    nlohmann::json sample_request;
    nlohmann::json sample_response_shape;
};

// Full ProviderManifest. Mirrors the v1alpha1 YAML schema.
struct ProviderManifest {
    // metadata
    std::string api_version;
    std::string kind;
    std::string name;
    std::string display_name;
    std::string vendor;
    std::string homepage;
    std::string documentation;
    std::vector<std::string> tags;
    std::string maturity = "preview";

    // spec.connector
    std::string connector_kind;

    // spec.endpoint
    std::string base_url_default;
    std::string base_url_env;
    int timeout_ms_default = 30000;
    int max_retries_default = 2;

    // spec.auth
    std::string auth_type;
    std::string auth_header_name;
    std::string auth_env_var;
    bool auth_supports_multi_key = false;

    // spec.compatibility
    std::string openai_chat_compat;  // full | partial | translated | none
    std::vector<ManifestFieldCompat> fields;

    // spec.capabilities
    std::vector<std::string> capabilities;

    // spec.models
    std::vector<ManifestModel> models;

    // spec.conformance
    ManifestConformance conformance;
};

// A single validation or conformance issue.
struct ValidationIssue {
    enum class Severity { Warning, Error };
    Severity severity;
    std::string code;       // machine-readable, e.g. "manifest.kind.unknown"
    std::string message;    // human-readable
    std::string field_path; // e.g. "spec.auth.type"
};

struct ValidationReport {
    std::vector<ValidationIssue> issues;

    bool ok() const;            // true iff no Error-severity issues
    size_t errorCount() const;
    size_t warningCount() const;

    void addError(std::string code, std::string message, std::string field_path = "");
    void addWarning(std::string code, std::string message, std::string field_path = "");
    void merge(const ValidationReport& other);
};

// --- Parsing ---

// Load and parse a Manifest from YAML text. Returns nullopt on unrecoverable
// parse failure; the report is populated either way. Structural validation
// errors ("wrong kind", "bad apiVersion") are reported as Errors but do not
// necessarily prevent returning a populated Manifest — callers should check
// `report.ok()` before trusting the manifest.
std::optional<ProviderManifest> loadManifestFromYaml(
    const std::string& yaml_text,
    ValidationReport& report);

std::optional<ProviderManifest> loadManifestFromFile(
    const std::string& path,
    ValidationReport& report);

// --- Validation ---

// Deep structural validation. Safe to re-run on loadManifest*'s result for
// explicit strict-mode reporting.
ValidationReport validateManifest(const ProviderManifest& m);

// --- Conformance ---

// Static conformance runner. Performs shape checks (no network I/O).
// Current checks (v0):
//   - conformance.auth-defined
//   - conformance.compatibility-declared
//   - conformance.fields-coverage
//   - conformance.capability-enum
//   - conformance.sample-request-shape
//   - conformance.models-unique
// (Structural manifest-shape checks are done by validateManifest.)
ValidationReport runConformanceChecks(const ProviderManifest& m);

} // namespace aegisgate
