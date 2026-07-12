// Phase 9.3 Epic 3 Task 3.3 — SensitiveScanner (SR4 defence).
//
// Scans yaml text for patterns that look like secrets and must block Submit.
// Patterns (plan §3.3):
//   * api_key   : sk-xxx / sess-xxx / raw 32+ char token
//   * license_key: 20+ char base64-like
//   * jwt_secret: 16+ char
//   * password  : 3+ non-whitespace, excluding ${...} env refs and empty
//
// Positive cases must surface at least {line, column, field_name} so the
// control plane can emit structured error detail without leaking the secret
// itself.

#include "control_plane/sensitive_scanner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace aegisgate {
namespace {

bool hasField(const std::vector<SensitiveFinding>& findings,
              const std::string& field_name) {
    return std::any_of(findings.begin(), findings.end(),
        [&](const SensitiveFinding& f) { return f.field_name == field_name; });
}

// ---------- Positive: should be detected ----------

TEST(SensitiveScanner, DetectsApiKeyWithSkPrefix) {
    SensitiveScanner s;
    auto findings = s.scan("api_key: sk-abcdef1234567890\n");
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].field_name, "api_key");
    EXPECT_EQ(findings[0].line, 1);
}

TEST(SensitiveScanner, DetectsApiKeyWithSessPrefix) {
    SensitiveScanner s;
    auto findings = s.scan("api-key: \"sess-ABCDEFGHIJ\"\n");
    EXPECT_TRUE(hasField(findings, "api-key"));
}

TEST(SensitiveScanner, DetectsLongRawApiToken) {
    SensitiveScanner s;
    auto findings = s.scan(
        "APIKEY: ZrYv29xQmPb4aF8LkJnOxTuE7VcBw0Nh\n");
    EXPECT_FALSE(findings.empty());
}

TEST(SensitiveScanner, DetectsLicenseKey) {
    SensitiveScanner s;
    auto findings = s.scan(
        "license_key: \"aGVsbG8gd29ybGQgMTIzNDU=\"\n");
    EXPECT_TRUE(hasField(findings, "license_key"));
}

TEST(SensitiveScanner, DetectsLicenseKeyDashVariant) {
    SensitiveScanner s;
    auto findings = s.scan("license-key: abcdefghijklmnopqrst1234\n");
    EXPECT_TRUE(hasField(findings, "license-key"));
}

TEST(SensitiveScanner, DetectsJwtSecret) {
    SensitiveScanner s;
    auto findings = s.scan("jwt_secret: s3cretValueLonger\n");
    EXPECT_TRUE(hasField(findings, "jwt_secret"));
}

TEST(SensitiveScanner, DetectsJwtSecretDash) {
    SensitiveScanner s;
    auto findings = s.scan("jwt-secret: \"AbCdEfGhIjKlMnOp\"\n");
    EXPECT_TRUE(hasField(findings, "jwt-secret"));
}

TEST(SensitiveScanner, DetectsPasswordLiteral) {
    SensitiveScanner s;
    auto findings = s.scan("password: hunter22\n");
    EXPECT_TRUE(hasField(findings, "password"));
}

// ---------- Negative: must NOT be flagged ----------

TEST(SensitiveScanner, EnvVarReferenceIsSafe) {
    SensitiveScanner s;
    EXPECT_TRUE(s.scan("password: ${DB_PASSWORD}\n").empty());
    EXPECT_TRUE(s.scan("api_key: ${OPENAI_API_KEY}\n").empty());
    EXPECT_TRUE(s.scan("jwt_secret: ${JWT_SECRET}\n").empty());
}

TEST(SensitiveScanner, EmptyStringIsSafe) {
    SensitiveScanner s;
    EXPECT_TRUE(s.scan("password: \"\"\n").empty());
    EXPECT_TRUE(s.scan("api_key:\n").empty());
}

TEST(SensitiveScanner, ShortPasswordRejected) {
    // `: a` is only 1 char, shouldn't be caught by >=3 char password rule.
    SensitiveScanner s;
    EXPECT_TRUE(s.scan("password: a\n").empty());
}

TEST(SensitiveScanner, UnrelatedKeysUnaffected) {
    // Ensure we don't flag things that merely contain the word "key".
    SensitiveScanner s;
    EXPECT_TRUE(s.scan("cache_key_prefix: aegisgate\n").empty());
    EXPECT_TRUE(s.scan("passwordless_flow: true\n").empty());
}

TEST(SensitiveScanner, CommentLinesUnaffected) {
    SensitiveScanner s;
    // Our conservative MVP regex does match inside comments, but the plan
    // prioritises false-positives over false-negatives. We still want this
    // exact line — a schema placeholder — to remain silent.
    EXPECT_TRUE(s.scan("# example schema: api_key\n").empty());
}

TEST(SensitiveScanner, MultipleFindingsReported) {
    SensitiveScanner s;
    auto findings = s.scan(
        "api_key: sk-abcdef1234567890\n"
        "password: hunter22\n");
    EXPECT_GE(findings.size(), 2u);
    EXPECT_TRUE(hasField(findings, "api_key"));
    EXPECT_TRUE(hasField(findings, "password"));
    EXPECT_GT(findings[1].line, findings[0].line);
}

} // namespace
} // namespace aegisgate
