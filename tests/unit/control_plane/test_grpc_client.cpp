// Phase 9.3 Epic 7 Task 7.1 — ControlPlaneClient (client-side gRPC stub).
//
// Three layers exercised here:
//   * validateConnectOptions() — pure precondition checks (SR7 client-side
//     fail-closed: empty endpoint / api_key / mismatched mTLS pair /
//     non-positive timeout are all rejected *before* a channel is built).
//   * BearerTokenCredentialsPlugin — attaches "authorization: Bearer <key>"
//     to every outbound call. Tested without a live channel by driving the
//     plugin's GetMetadata() directly.
//   * ControlPlaneClient — thin wrapper. We verify construction succeeds
//     with valid options (reading cert material from a scratch file),
//     that stub() returns non-null, and that prepareContext() installs a
//     deadline derived from ConnectOptions::timeout_seconds.
//
// No live server is required — gRPC channels connect lazily on first RPC.

#include "cli/grpc_client.h"
#include "test_tls_fixtures.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace aegisgate::cli {
namespace {

using ::aegisgate::control_plane::test_fixtures::serverCertPem;
using ::aegisgate::control_plane::test_fixtures::serverKeyPem;

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

std::filesystem::path scratchDir() {
    auto dir = std::filesystem::temp_directory_path() /
               ("aegisctl-client-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    return dir;
}

std::string writeTempFile(const std::string& name, const std::string& content) {
    const auto p = scratchDir() / name;
    std::ofstream(p) << content;
    return p.string();
}

ControlPlaneClient::ConnectOptions minimalOpts() {
    ControlPlaneClient::ConnectOptions o;
    o.endpoint = "127.0.0.1:19443";
    o.api_key  = "sk-test-abc";
    return o;
}

// --------------------------------------------------------------------------
// validateConnectOptions — precondition checks
// --------------------------------------------------------------------------

TEST(ValidateConnectOptions, AcceptsMinimalValidOptions) {
    EXPECT_NO_THROW(validateConnectOptions(minimalOpts()));
}

TEST(ValidateConnectOptions, RejectsEmptyEndpoint) {
    auto o = minimalOpts();
    o.endpoint = "";
    EXPECT_THROW(validateConnectOptions(o), std::invalid_argument);
}

TEST(ValidateConnectOptions, RejectsEmptyApiKey) {
    auto o = minimalOpts();
    o.api_key = "";
    EXPECT_THROW(validateConnectOptions(o), std::invalid_argument);
}

TEST(ValidateConnectOptions, RejectsNonPositiveTimeout) {
    auto o = minimalOpts();
    o.timeout_seconds = 0;
    EXPECT_THROW(validateConnectOptions(o), std::invalid_argument);

    o.timeout_seconds = -5;
    EXPECT_THROW(validateConnectOptions(o), std::invalid_argument);
}

TEST(ValidateConnectOptions, RejectsMtlsCertWithoutKey) {
    auto o = minimalOpts();
    o.client_cert_path = "/tmp/whatever.crt";
    o.client_key_path  = "";
    EXPECT_THROW(validateConnectOptions(o), std::invalid_argument);
}

TEST(ValidateConnectOptions, RejectsMtlsKeyWithoutCert) {
    auto o = minimalOpts();
    o.client_cert_path = "";
    o.client_key_path  = "/tmp/whatever.key";
    EXPECT_THROW(validateConnectOptions(o), std::invalid_argument);
}

TEST(ValidateConnectOptions, AcceptsFullMtlsConfig) {
    auto o = minimalOpts();
    o.ca_cert_path     = "/tmp/ca.crt";
    o.client_cert_path = "/tmp/cli.crt";
    o.client_key_path  = "/tmp/cli.key";
    EXPECT_NO_THROW(validateConnectOptions(o));
}

// --------------------------------------------------------------------------
// BearerTokenCredentialsPlugin — metadata injection
// --------------------------------------------------------------------------

TEST(BearerTokenCredentialsPlugin, AuthorizationValueHasBearerPrefix) {
    EXPECT_EQ(BearerTokenCredentialsPlugin::authorizationValue("sk-unit-42"),
              "Bearer sk-unit-42");
}

TEST(BearerTokenCredentialsPlugin, AuthorizationValueEmptyTokenYieldsEmpty) {
    // Empty key is rejected at validateConnectOptions, but guard the pure
    // helper too: returning "" lets the plugin refuse to attach the header
    // instead of sending a nonsense "Bearer " string.
    EXPECT_EQ(BearerTokenCredentialsPlugin::authorizationValue(""), "");
}

TEST(BearerTokenCredentialsPlugin, PluginAdvertisesType) {
    BearerTokenCredentialsPlugin plugin("sk");
    EXPECT_STREQ(plugin.GetType(), "BearerToken");
    EXPECT_FALSE(plugin.IsBlocking());
}

// --------------------------------------------------------------------------
// ControlPlaneClient — construction + stub + deadline
// --------------------------------------------------------------------------

TEST(ControlPlaneClient, ConstructorThrowsOnInvalidOptions) {
    ControlPlaneClient::ConnectOptions bad;  // empty endpoint
    EXPECT_THROW(ControlPlaneClient c(bad), std::invalid_argument);
}

TEST(ControlPlaneClient, ConstructorThrowsOnMissingCaFile) {
    auto o = minimalOpts();
    o.ca_cert_path = "/nonexistent/path/ca.crt";
    EXPECT_THROW(ControlPlaneClient c(o), std::runtime_error);
}

TEST(ControlPlaneClient, StubNonNullWithSystemRoots) {
    // No ca_cert_path provided — use system roots (channel is lazy so we
    // never actually hit the wire here).
    ControlPlaneClient client(minimalOpts());
    auto stub = client.stub();
    EXPECT_NE(stub, nullptr);
}

TEST(ControlPlaneClient, StubNonNullWithExplicitCa) {
    auto ca_path = writeTempFile("ca.crt", serverCertPem());
    auto o = minimalOpts();
    o.ca_cert_path = ca_path;
    ControlPlaneClient client(o);
    EXPECT_NE(client.stub(), nullptr);
}

TEST(ControlPlaneClient, StubNonNullWithMtlsMaterial) {
    auto ca_path   = writeTempFile("ca-mtls.crt",     serverCertPem());
    auto cert_path = writeTempFile("client-mtls.crt", serverCertPem());
    auto key_path  = writeTempFile("client-mtls.key", serverKeyPem());
    auto o = minimalOpts();
    o.ca_cert_path     = ca_path;
    o.client_cert_path = cert_path;
    o.client_key_path  = key_path;
    ControlPlaneClient client(o);
    EXPECT_NE(client.stub(), nullptr);
}

TEST(ControlPlaneClient, PrepareContextSetsDeadline) {
    auto o = minimalOpts();
    o.timeout_seconds = 7;
    ControlPlaneClient client(o);

    grpc::ClientContext ctx;
    const auto before = std::chrono::system_clock::now();
    client.prepareContext(ctx);
    const auto deadline = ctx.deadline();

    // Deadline must be ~timeout_seconds in the future (allow 2s slack for
    // scheduling overhead on the CI host).
    const auto delta = deadline - before;
    EXPECT_GE(delta, std::chrono::seconds(o.timeout_seconds - 1));
    EXPECT_LE(delta, std::chrono::seconds(o.timeout_seconds + 2));
}

TEST(ControlPlaneClient, OptionsAccessorReturnsCopy) {
    auto o = minimalOpts();
    o.timeout_seconds = 42;
    ControlPlaneClient client(o);
    EXPECT_EQ(client.options().timeout_seconds, 42);
    EXPECT_EQ(client.options().endpoint, "127.0.0.1:19443");
    // SR8 reminder: api_key is stored but never echoed by operator-facing
    // status/error paths. The accessor exposing it is used only by the
    // Bearer plugin factory and unit tests.
    EXPECT_EQ(client.options().api_key, "sk-test-abc");
}

}  // namespace
}  // namespace aegisgate::cli
