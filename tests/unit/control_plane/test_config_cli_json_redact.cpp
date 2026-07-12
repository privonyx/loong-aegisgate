// SR-NEW1 (TASK-20260515-01 C1): coverage tests for `--output json` redaction.
//
// Mode: [覆盖补充] (writing-plans.mdc) — implementation already exists,
// tests pin the contract. Mutation testing (later in same Epic) verifies
// the tests actually catch a regression by injecting:
//   M_C1_a: skip the redaction call -> SecretsAreScrubbed FAIL
//   M_C1_b: case-sensitive key match -> CaseInsensitiveKeyMatch FAIL

#include <gtest/gtest.h>

#include "cli/config_cli.h"

#include <string>

using namespace aegisgate::cli;

namespace {

TEST(ConfigCliJsonRedactTest, SecretsAreScrubbedFromCommonKeys) {
    std::string json = R"({
        "api_key": "sk-live-AAAA",
        "password": "hunter2",
        "client_secret": "shhhh",
        "bearer_token": "Bearer xxxx",
        "submitter_comment": "we leaked sk-live-BBBB by accident",
        "user": "alice"
    })";
    auto count = redactSensitiveJsonForTest(json);
    EXPECT_GE(count, 5u);
    // Each redacted field must be replaced with the literal "<redacted>"
    // sentinel and the original value must be GONE.
    EXPECT_EQ(json.find("sk-live-AAAA"), std::string::npos);
    EXPECT_EQ(json.find("hunter2"), std::string::npos);
    EXPECT_EQ(json.find("shhhh"), std::string::npos);
    EXPECT_EQ(json.find("Bearer xxxx"), std::string::npos);
    EXPECT_EQ(json.find("sk-live-BBBB"), std::string::npos);
    EXPECT_NE(json.find("<redacted>"), std::string::npos);
    // Non-sensitive fields are preserved.
    EXPECT_NE(json.find("alice"), std::string::npos);
}

TEST(ConfigCliJsonRedactTest, CaseInsensitiveKeyMatch) {
    std::string json = R"({
        "API_KEY": "AAA",
        "Password": "BBB",
        "AuthToken": "CCC"
    })";
    auto count = redactSensitiveJsonForTest(json);
    EXPECT_GE(count, 3u);
    EXPECT_EQ(json.find("AAA"), std::string::npos);
    EXPECT_EQ(json.find("BBB"), std::string::npos);
    EXPECT_EQ(json.find("CCC"), std::string::npos);
}

TEST(ConfigCliJsonRedactTest, RecursesIntoNestedObjectsAndArrays) {
    std::string json = R"({
        "outer": {
            "credentials": {
                "api_key": "sk-nested",
                "ok_field": "visible"
            }
        },
        "list": [
            {"token": "TKN1"},
            {"token": "TKN2"}
        ]
    })";
    auto count = redactSensitiveJsonForTest(json);
    EXPECT_GE(count, 3u);
    EXPECT_EQ(json.find("sk-nested"), std::string::npos);
    EXPECT_EQ(json.find("TKN1"), std::string::npos);
    EXPECT_EQ(json.find("TKN2"), std::string::npos);
    EXPECT_NE(json.find("visible"), std::string::npos);
}

TEST(ConfigCliJsonRedactTest, IsIdempotent) {
    std::string json = R"({"api_key": "secret"})";
    redactSensitiveJsonForTest(json);
    auto count_second = redactSensitiveJsonForTest(json);
    EXPECT_EQ(count_second, 0u);
    EXPECT_NE(json.find("<redacted>"), std::string::npos);
}

TEST(ConfigCliJsonRedactTest, NonStringValuesAreUntouched) {
    std::string json = R"({
        "size_bytes": 42,
        "active": true,
        "items": [1, 2, 3]
    })";
    auto count = redactSensitiveJsonForTest(json);
    EXPECT_EQ(count, 0u);
    EXPECT_NE(json.find("42"), std::string::npos);
    EXPECT_NE(json.find("true"), std::string::npos);
}

TEST(ConfigCliJsonRedactTest, EmptyAndMissingKeys) {
    std::string json = R"({"empty_secret": "", "user": "bob"})";
    auto count = redactSensitiveJsonForTest(json);
    // empty_secret matches "secret" substring -> still redacted (treats
    // empty as a redactable string for consistency).
    EXPECT_GE(count, 1u);
    EXPECT_NE(json.find("bob"), std::string::npos);
}

}  // namespace
