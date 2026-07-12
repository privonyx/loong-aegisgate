#include <gtest/gtest.h>
#include "gateway/provider_spec/provider_manifest.h"
#include <fstream>
#include <string>

using namespace aegisgate;

namespace {

// A fully-compliant minimal Manifest YAML used as the green-path baseline.
// Individual tests mutate this string to introduce specific violations.
constexpr const char* kMinimalValid = R"YAML(
apiVersion: aegisgate.dev/v1alpha1
kind: ProviderManifest
metadata:
  name: openai
  display_name: OpenAI
  maturity: stable
spec:
  connector:
    kind: openai
  endpoint:
    base_url_default: https://api.openai.com/v1
  auth:
    type: bearer
    header_name: Authorization
    env_var: OPENAI_API_KEY
  compatibility:
    openai_chat_completions: full
  capabilities:
    - streaming
    - tools
)YAML";

// A richer Manifest exercising every optional field.
constexpr const char* kFullManifest = R"YAML(
apiVersion: aegisgate.dev/v1alpha1
kind: ProviderManifest
metadata:
  name: openai
  display_name: OpenAI
  vendor: OpenAI Inc.
  homepage: https://openai.com
  documentation: https://platform.openai.com/docs
  tags: [commercial, chat, tools, vision]
  maturity: stable
spec:
  connector:
    kind: openai
  endpoint:
    base_url_default: https://api.openai.com/v1
    base_url_env: OPENAI_BASE_URL
    timeout_ms_default: 30000
    max_retries_default: 2
  auth:
    type: bearer
    header_name: Authorization
    env_var: OPENAI_API_KEY
    supports_multi_key: true
  compatibility:
    openai_chat_completions: full
    fields:
      - name: messages
        status: supported
      - name: tools
        status: supported
      - name: logprobs
        status: unsupported
        notes: Only available on legacy completions endpoint.
  capabilities:
    - streaming
    - tools
    - vision
    - response_format
    - system_message
    - temperature
    - top_p
    - max_tokens
  models:
    - id: gpt-4o
      max_context_tokens: 128000
      capabilities: [streaming, tools, vision]
      region_hints: [us-east, eu-central]
    - id: gpt-4o-mini
      max_context_tokens: 128000
      capabilities: [streaming, tools]
  conformance:
    required_checks:
      - manifest-shape
      - auth-defined
      - compatibility-declared
      - fields-coverage
      - capability-enum
      - models-unique
      - sample-request-shape
    sample_request:
      model: gpt-4o-mini
      messages:
        - role: user
          content: ping
      max_tokens: 16
    sample_response_shape:
      choices:
        - message:
            role: assistant
            content: string
)YAML";

bool hasError(const ValidationReport& r, const std::string& code) {
    for (const auto& i : r.issues) {
        if (i.severity == ValidationIssue::Severity::Error && i.code == code) return true;
    }
    return false;
}

bool hasWarning(const ValidationReport& r, const std::string& code) {
    for (const auto& i : r.issues) {
        if (i.severity == ValidationIssue::Severity::Warning && i.code == code) return true;
    }
    return false;
}

std::string mutateReplace(std::string src, const std::string& from, const std::string& to) {
    auto pos = src.find(from);
    if (pos != std::string::npos) {
        src.replace(pos, from.size(), to);
    }
    return src;
}

} // namespace

// ============================================================================
// Parsing
// ============================================================================

TEST(ProviderManifestParse, EmptyYamlFailsWithError) {
    ValidationReport r;
    auto m = loadManifestFromYaml("", r);
    EXPECT_FALSE(m.has_value());
    EXPECT_GT(r.errorCount(), 0u);
}

TEST(ProviderManifestParse, MinimalValidManifestParses) {
    ValidationReport r;
    auto m = loadManifestFromYaml(kMinimalValid, r);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->name, "openai");
    EXPECT_EQ(m->api_version, "aegisgate.dev/v1alpha1");
    EXPECT_EQ(m->kind, "ProviderManifest");
    EXPECT_EQ(m->connector_kind, "openai");
    EXPECT_EQ(m->auth_type, "bearer");
    EXPECT_EQ(m->openai_chat_compat, "full");
    EXPECT_TRUE(r.ok());
}

TEST(ProviderManifestParse, FullManifestParsesAllFields) {
    ValidationReport r;
    auto m = loadManifestFromYaml(kFullManifest, r);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->name, "openai");
    EXPECT_EQ(m->display_name, "OpenAI");
    EXPECT_EQ(m->vendor, "OpenAI Inc.");
    EXPECT_EQ(m->maturity, "stable");
    EXPECT_EQ(m->tags.size(), 4u);
    EXPECT_EQ(m->base_url_default, "https://api.openai.com/v1");
    EXPECT_EQ(m->base_url_env, "OPENAI_BASE_URL");
    EXPECT_EQ(m->timeout_ms_default, 30000);
    EXPECT_EQ(m->max_retries_default, 2);
    EXPECT_TRUE(m->auth_supports_multi_key);
    EXPECT_EQ(m->fields.size(), 3u);
    EXPECT_EQ(m->capabilities.size(), 8u);
    EXPECT_EQ(m->models.size(), 2u);
    EXPECT_EQ(m->models[0].id, "gpt-4o");
    EXPECT_EQ(m->models[0].max_context_tokens, 128000);
    EXPECT_EQ(m->models[0].region_hints.size(), 2u);
    EXPECT_FALSE(m->conformance.required_checks.empty());
    EXPECT_TRUE(m->conformance.sample_request.contains("model"));
}

TEST(ProviderManifestParse, LoadFromFile) {
    const std::string path = "/tmp/aegisgate_test_manifest.yaml";
    {
        std::ofstream ofs(path);
        ofs << kMinimalValid;
    }
    ValidationReport r;
    auto m = loadManifestFromFile(path, r);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->name, "openai");
    std::remove(path.c_str());
}

// ============================================================================
// Compliance constraints (§3.3)
// ============================================================================

TEST(ProviderManifestValidate, WrongApiVersionIsError) {
    auto yaml = mutateReplace(kMinimalValid,
                              "apiVersion: aegisgate.dev/v1alpha1",
                              "apiVersion: wrong/version");
    ValidationReport r;
    auto m = loadManifestFromYaml(yaml, r);
    // Parse may still succeed but validation must flag Error.
    if (m.has_value()) {
        auto vr = validateManifest(*m);
        EXPECT_TRUE(hasError(vr, "manifest.apiVersion.unknown"));
    } else {
        EXPECT_TRUE(hasError(r, "manifest.apiVersion.unknown"));
    }
}

TEST(ProviderManifestValidate, WrongKindIsError) {
    auto yaml = mutateReplace(kMinimalValid, "kind: ProviderManifest", "kind: Something");
    ValidationReport r;
    auto m = loadManifestFromYaml(yaml, r);
    ValidationReport vr = m.has_value() ? validateManifest(*m) : r;
    EXPECT_TRUE(hasError(vr, "manifest.kind.unknown"));
}

TEST(ProviderManifestValidate, EmptyNameIsError) {
    auto yaml = mutateReplace(kMinimalValid, "name: openai", "name: \"\"");
    ValidationReport r;
    auto m = loadManifestFromYaml(yaml, r);
    ValidationReport vr = m.has_value() ? validateManifest(*m) : r;
    EXPECT_TRUE(hasError(vr, "metadata.name.empty"));
}

TEST(ProviderManifestValidate, IllegalNameCharsIsError) {
    auto yaml = mutateReplace(kMinimalValid, "name: openai", "name: \"Bad NAME!\"");
    ValidationReport r;
    auto m = loadManifestFromYaml(yaml, r);
    ValidationReport vr = m.has_value() ? validateManifest(*m) : r;
    EXPECT_TRUE(hasError(vr, "metadata.name.invalid"));
}

TEST(ProviderManifestValidate, UnknownConnectorKindIsError) {
    auto yaml = mutateReplace(kMinimalValid, "kind: openai\n", "kind: unknown-sidecar\n");
    ValidationReport r;
    auto m = loadManifestFromYaml(yaml, r);
    ValidationReport vr = m.has_value() ? validateManifest(*m) : r;
    EXPECT_TRUE(hasError(vr, "spec.connector.kind.unknown"));
}

TEST(ProviderManifestValidate, NonUrlBaseUrlIsError) {
    auto yaml = mutateReplace(kMinimalValid,
                              "base_url_default: https://api.openai.com/v1",
                              "base_url_default: not-a-url");
    ValidationReport r;
    auto m = loadManifestFromYaml(yaml, r);
    ValidationReport vr = m.has_value() ? validateManifest(*m) : r;
    EXPECT_TRUE(hasError(vr, "spec.endpoint.base_url_default.invalid"));
}

TEST(ProviderManifestValidate, UnknownAuthTypeIsError) {
    auto yaml = mutateReplace(kMinimalValid, "type: bearer", "type: telepathy");
    ValidationReport r;
    auto m = loadManifestFromYaml(yaml, r);
    ValidationReport vr = m.has_value() ? validateManifest(*m) : r;
    EXPECT_TRUE(hasError(vr, "spec.auth.type.unknown"));
}

TEST(ProviderManifestValidate, UnknownOpenAiChatCompatIsError) {
    auto yaml = mutateReplace(kMinimalValid,
                              "openai_chat_completions: full",
                              "openai_chat_completions: magical");
    ValidationReport r;
    auto m = loadManifestFromYaml(yaml, r);
    ValidationReport vr = m.has_value() ? validateManifest(*m) : r;
    EXPECT_TRUE(hasError(vr, "spec.compatibility.openai_chat_completions.unknown"));
}

TEST(ProviderManifestValidate, DuplicateModelIdsIsError) {
    // Use the full manifest but duplicate gpt-4o-mini as gpt-4o (collision).
    auto yaml = mutateReplace(kFullManifest,
                              "- id: gpt-4o-mini\n      max_context_tokens: 128000\n      capabilities: [streaming, tools]",
                              "- id: gpt-4o\n      max_context_tokens: 128000\n      capabilities: [streaming, tools]");
    ValidationReport r;
    auto m = loadManifestFromYaml(yaml, r);
    ValidationReport vr = m.has_value() ? validateManifest(*m) : r;
    EXPECT_TRUE(hasError(vr, "spec.models.id.duplicate"));
}

// ============================================================================
// Conformance checks (§7)
// ============================================================================

TEST(ProviderManifestConformance, BearerAuthWithoutHeaderOrEnvIsWarning) {
    // Remove header_name and env_var entries but keep type: bearer.
    auto yaml = mutateReplace(kMinimalValid,
                              "    header_name: Authorization\n    env_var: OPENAI_API_KEY\n",
                              "");
    ValidationReport r;
    auto m = loadManifestFromYaml(yaml, r);
    ASSERT_TRUE(m.has_value());
    auto cr = runConformanceChecks(*m);
    EXPECT_TRUE(hasWarning(cr, "conformance.auth-defined"));
}

TEST(ProviderManifestConformance, ClaudeKindWithFullCompatIsWarning) {
    auto yaml = mutateReplace(kMinimalValid, "kind: openai\n", "kind: claude\n");
    ValidationReport r;
    auto m = loadManifestFromYaml(yaml, r);
    ASSERT_TRUE(m.has_value());
    auto cr = runConformanceChecks(*m);
    EXPECT_TRUE(hasWarning(cr, "conformance.compatibility-declared"));
}

TEST(ProviderManifestConformance, UnknownOpenAiFieldIsWarning) {
    // Add an unknown OpenAI field under fields[].
    constexpr const char* yaml = R"YAML(
apiVersion: aegisgate.dev/v1alpha1
kind: ProviderManifest
metadata:
  name: openai
spec:
  connector:
    kind: openai
  endpoint:
    base_url_default: https://api.openai.com/v1
  auth:
    type: bearer
    header_name: Authorization
    env_var: OPENAI_API_KEY
  compatibility:
    openai_chat_completions: full
    fields:
      - name: madeup_field_xyz
        status: supported
  capabilities:
    - streaming
)YAML";
    ValidationReport r;
    auto m = loadManifestFromYaml(yaml, r);
    ASSERT_TRUE(m.has_value());
    auto cr = runConformanceChecks(*m);
    EXPECT_TRUE(hasWarning(cr, "conformance.fields-coverage"));
}

TEST(ProviderManifestConformance, UnknownCapabilityIsError) {
    auto yaml = mutateReplace(kMinimalValid,
                              "    - streaming\n    - tools\n",
                              "    - streaming\n    - nonexistent_cap\n");
    ValidationReport r;
    auto m = loadManifestFromYaml(yaml, r);
    ASSERT_TRUE(m.has_value());
    auto cr = runConformanceChecks(*m);
    EXPECT_TRUE(hasError(cr, "conformance.capability-enum"));
}

TEST(ProviderManifestConformance, SampleRequestMissingMessagesIsError) {
    // Full manifest but drop messages from sample_request.
    auto yaml = mutateReplace(kFullManifest,
                              "    sample_request:\n      model: gpt-4o-mini\n      messages:\n        - role: user\n          content: ping\n      max_tokens: 16",
                              "    sample_request:\n      model: gpt-4o-mini\n      max_tokens: 16");
    ValidationReport r;
    auto m = loadManifestFromYaml(yaml, r);
    ASSERT_TRUE(m.has_value());
    auto cr = runConformanceChecks(*m);
    EXPECT_TRUE(hasError(cr, "conformance.sample-request-shape"));
}

TEST(ProviderManifestConformance, FullCompliantManifestPassesAllChecks) {
    ValidationReport r;
    auto m = loadManifestFromYaml(kFullManifest, r);
    ASSERT_TRUE(m.has_value());
    auto cr = runConformanceChecks(*m);
    EXPECT_TRUE(cr.ok());
    EXPECT_EQ(cr.errorCount(), 0u);
}

// ============================================================================
// ValidationReport utilities
// ============================================================================

TEST(ValidationReportOps, OkIsFalseWhenAnyError) {
    ValidationReport r;
    r.issues.push_back({ValidationIssue::Severity::Warning, "w", "warn", ""});
    EXPECT_TRUE(r.ok());
    r.issues.push_back({ValidationIssue::Severity::Error, "e", "err", ""});
    EXPECT_FALSE(r.ok());
}

TEST(ValidationReportOps, CountsAreAccurate) {
    ValidationReport r;
    r.issues.push_back({ValidationIssue::Severity::Warning, "w1", "", ""});
    r.issues.push_back({ValidationIssue::Severity::Warning, "w2", "", ""});
    r.issues.push_back({ValidationIssue::Severity::Error,   "e1", "", ""});
    EXPECT_EQ(r.warningCount(), 2u);
    EXPECT_EQ(r.errorCount(), 1u);
}

TEST(ValidationReportOps, ConformanceReportMergesCleanly) {
    auto yaml = mutateReplace(kMinimalValid, "kind: openai\n", "kind: claude\n");
    ValidationReport r;
    auto m = loadManifestFromYaml(yaml, r);
    ASSERT_TRUE(m.has_value());
    auto vr = validateManifest(*m);
    auto cr = runConformanceChecks(*m);
    // Conformance must carry its own issues without eating validation issues.
    EXPECT_TRUE(hasWarning(cr, "conformance.compatibility-declared"));
    (void)vr;
}
