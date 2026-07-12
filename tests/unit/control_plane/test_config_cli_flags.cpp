// Phase 9.3 Epic 7 Task 7.6 — common flags + env defaults.
//
// The dispatcher builds a live ControlPlaneClient from the parsed
// GlobalFlags so we cannot unit-test it without a running control plane.
// Instead we exercise the pure `parseGlobalFlags` helper + the output
// format parser, which together drive every decision the dispatcher
// makes before opening a channel.

#include "cli/config_cli.h"

#include <gtest/gtest.h>

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace aegisgate::cli {
namespace {

std::vector<std::string> argv(std::initializer_list<const char*> args) {
    return std::vector<std::string>(args.begin(), args.end());
}

EnvLookupFn makeEnv(std::map<std::string, std::string> m) {
    return [m = std::move(m)](const std::string& k) -> std::string {
        auto it = m.find(k);
        return it == m.end() ? std::string() : it->second;
    };
}

// --------------------------------------------------------------------------
// outputFormatFromString
// --------------------------------------------------------------------------

TEST(OutputFormatFromString, AcceptsKnownFormats) {
    EXPECT_EQ(outputFormatFromString("table"), OutputFormat::Table);
    EXPECT_EQ(outputFormatFromString("TABLE"), OutputFormat::Table);
    EXPECT_EQ(outputFormatFromString("json"),  OutputFormat::Json);
    EXPECT_EQ(outputFormatFromString("yaml"),  OutputFormat::Yaml);
}

TEST(OutputFormatFromString, RejectsUnknown) {
    EXPECT_THROW(outputFormatFromString("xml"), std::invalid_argument);
    EXPECT_THROW(outputFormatFromString(""),    std::invalid_argument);
}

// --------------------------------------------------------------------------
// parseGlobalFlags — environment defaults
// --------------------------------------------------------------------------

TEST(ParseGlobalFlags, EnvProvidesDefaults) {
    auto env = makeEnv({
        {"AEGISGATE_CP_ENDPOINT", "cp.example:9443"},
        {"AEGISGATE_CP_TLS_CA",   "/etc/ca.pem"},
        {"AEGISGATE_CP_API_KEY",  "sk-from-env"},
        {"AEGISGATE_CP_TIMEOUT",  "60"},
    });
    auto flags = parseGlobalFlags(argv({"apply", "file.yaml",
                                         "--comment", "x"}),
                                   env);
    EXPECT_EQ(flags.connect.endpoint, "cp.example:9443");
    EXPECT_EQ(flags.connect.ca_cert_path, "/etc/ca.pem");
    EXPECT_EQ(flags.connect.api_key, "sk-from-env");
    EXPECT_EQ(flags.connect.timeout_seconds, 60);
    EXPECT_EQ(flags.subcommand, "apply");
    // subcommand-specific tokens are preserved in order.
    ASSERT_EQ(flags.subcommand_args.size(), 3u);
    EXPECT_EQ(flags.subcommand_args[0], "file.yaml");
    EXPECT_EQ(flags.subcommand_args[1], "--comment");
    EXPECT_EQ(flags.subcommand_args[2], "x");
}

TEST(ParseGlobalFlags, DefaultsWhenEnvUnset) {
    auto flags = parseGlobalFlags(argv({"list"}),
                                   makeEnv({{"AEGISGATE_CP_API_KEY", "sk"}}));
    // Hard-coded fallback endpoint. Stays stable so ops docs don't drift.
    EXPECT_EQ(flags.connect.endpoint, "127.0.0.1:9443");
    EXPECT_EQ(flags.connect.timeout_seconds, 30);
    EXPECT_EQ(flags.output, OutputFormat::Table);
    EXPECT_EQ(flags.subcommand, "list");
    EXPECT_TRUE(flags.subcommand_args.empty());
}

// --------------------------------------------------------------------------
// parseGlobalFlags — explicit flags override env
// --------------------------------------------------------------------------

TEST(ParseGlobalFlags, ExplicitFlagsOverrideEnv) {
    auto env = makeEnv({
        {"AEGISGATE_CP_ENDPOINT", "env.local:9443"},
        {"AEGISGATE_CP_TIMEOUT",  "15"},
        {"AEGISGATE_CP_API_KEY",  "sk-env"},
    });
    auto flags = parseGlobalFlags(argv({
        "--endpoint", "override.local:9443",
        "--tls-ca",   "/p/ca.pem",
        "--tls-cert", "/p/cli.crt",
        "--tls-key",  "/p/cli.key",
        "--timeout",  "45",
        "--output",   "json",
        "list",
    }), env);
    EXPECT_EQ(flags.connect.endpoint,         "override.local:9443");
    EXPECT_EQ(flags.connect.ca_cert_path,     "/p/ca.pem");
    EXPECT_EQ(flags.connect.client_cert_path, "/p/cli.crt");
    EXPECT_EQ(flags.connect.client_key_path,  "/p/cli.key");
    EXPECT_EQ(flags.connect.timeout_seconds,  45);
    EXPECT_EQ(flags.connect.api_key,          "sk-env");  // env still wins
    EXPECT_EQ(flags.output, OutputFormat::Json);
    EXPECT_EQ(flags.subcommand, "list");
}

TEST(ParseGlobalFlags, FlagsMayFollowSubcommand) {
    // Operators often invoke "aegisctl config apply <file> --endpoint ..."
    // The parser must not be positional-sensitive about --endpoint etc.
    auto env = makeEnv({{"AEGISGATE_CP_API_KEY", "sk"}});
    auto flags = parseGlobalFlags(argv({
        "apply", "file.yaml", "--endpoint", "h:1", "--comment", "x"
    }), env);
    EXPECT_EQ(flags.connect.endpoint, "h:1");
    EXPECT_EQ(flags.subcommand, "apply");
    ASSERT_EQ(flags.subcommand_args.size(), 3u);
    EXPECT_EQ(flags.subcommand_args[0], "file.yaml");
    EXPECT_EQ(flags.subcommand_args[1], "--comment");
    EXPECT_EQ(flags.subcommand_args[2], "x");
}

TEST(ParseGlobalFlags, RejectsInvalidTimeout) {
    auto env = makeEnv({{"AEGISGATE_CP_API_KEY", "sk"}});
    EXPECT_THROW(parseGlobalFlags(argv({"--timeout", "abc", "list"}), env),
                 std::invalid_argument);
    EXPECT_THROW(parseGlobalFlags(argv({"--timeout", "0",   "list"}), env),
                 std::invalid_argument);
}

TEST(ParseGlobalFlags, RejectsInvalidOutput) {
    auto env = makeEnv({{"AEGISGATE_CP_API_KEY", "sk"}});
    EXPECT_THROW(parseGlobalFlags(argv({"--output", "xml", "list"}), env),
                 std::invalid_argument);
}

TEST(ParseGlobalFlags, ThrowsWhenApiKeyMissing_SR8) {
    // The api_key must come from env. Missing it is a hard error so we
    // never silently build a channel that can't authenticate.
    auto env = makeEnv({});
    EXPECT_THROW(parseGlobalFlags(argv({"list"}), env),
                 std::invalid_argument);
}

TEST(ParseGlobalFlags, ThrowsOnMissingSubcommand) {
    auto env = makeEnv({{"AEGISGATE_CP_API_KEY", "sk"}});
    EXPECT_THROW(parseGlobalFlags(argv({}), env), std::invalid_argument);
    EXPECT_THROW(parseGlobalFlags(argv({"--endpoint", "h:1"}), env),
                 std::invalid_argument);
}

TEST(ParseGlobalFlags, ApiKeyValueFlagIsForbidden_SR8) {
    // Deliberately NOT exposing --api-key: accepting it on argv would let
    // `ps` / syslog snapshots leak the token. Treat it as unknown so the
    // subcommand parser reports the error.
    auto env = makeEnv({{"AEGISGATE_CP_API_KEY", "sk"}});
    auto flags = parseGlobalFlags(argv({"list", "--api-key", "hackme"}), env);
    EXPECT_EQ(flags.connect.api_key, "sk");
    // --api-key and its value are passed through untouched.
    ASSERT_EQ(flags.subcommand_args.size(), 2u);
    EXPECT_EQ(flags.subcommand_args[0], "--api-key");
    EXPECT_EQ(flags.subcommand_args[1], "hackme");
}

}  // namespace
}  // namespace aegisgate::cli
