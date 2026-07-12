// Phase 9.3 Epic 5 Task 5.4 + 5.5 — ServerBootstrap tests.
//
// Two suites share this translation unit:
//   * ServerBootstrapFailClosed — validates SR7 (no insecure code path)
//                                  and every misconfiguration branch by
//                                  constructing the bootstrap and asserting
//                                  it throws without opening a socket.
//   * ServerBootstrapStartup    — actually binds to 127.0.0.1:0 (ephemeral)
//                                  and verifies the server object is
//                                  returned and can be shut down cleanly,
//                                  plus mTLS config is accepted.
//   * FingerprintHelpers         — unit-tests the pure SHA-256 fingerprint
//                                  + allowlist logic (no TLS handshake).

#include "control_plane/grpc/server_bootstrap.h"
#include "control_plane/v1/control_plane.grpc.pb.h"
#include "test_tls_fixtures.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace aegisgate::control_plane::grpc_adapter {
namespace {

using aegisgate::controlplane::v1::ConfigService;
using aegisgate::control_plane::test_fixtures::serverCertPem;
using aegisgate::control_plane::test_fixtures::serverKeyPem;

// Plain sync Service — every method returns UNIMPLEMENTED by default,
// which is fine because these tests never invoke an RPC. Using the sync
// (not Async) variant avoids the completion-queue polling requirement.
ConfigService::Service& dummyService() {
    static ConfigService::Service svc;
    return svc;
}

// ---------------------------------------------------------------------------
// Task 5.4 — fail-closed configuration
// ---------------------------------------------------------------------------

TEST(ServerBootstrapFailClosed, MissingCertThrows_SR7) {
    ServerBootstrapConfig cfg;
    cfg.cert_pem = "";
    cfg.key_pem = serverKeyPem();
    cfg.listen_address = "127.0.0.1:0";
    EXPECT_THROW(bootstrapServer(cfg, &dummyService()),
                 std::invalid_argument);
}

TEST(ServerBootstrapFailClosed, MissingKeyThrows_SR7) {
    ServerBootstrapConfig cfg;
    cfg.cert_pem = serverCertPem();
    cfg.key_pem = "";
    cfg.listen_address = "127.0.0.1:0";
    EXPECT_THROW(bootstrapServer(cfg, &dummyService()),
                 std::invalid_argument);
}

TEST(ServerBootstrapFailClosed, MissingAddressThrows) {
    ServerBootstrapConfig cfg;
    cfg.cert_pem = serverCertPem();
    cfg.key_pem = serverKeyPem();
    cfg.listen_address = "";
    EXPECT_THROW(bootstrapServer(cfg, &dummyService()),
                 std::invalid_argument);
}

TEST(ServerBootstrapFailClosed, NullServiceThrows) {
    ServerBootstrapConfig cfg;
    cfg.cert_pem = serverCertPem();
    cfg.key_pem = serverKeyPem();
    cfg.listen_address = "127.0.0.1:0";
    EXPECT_THROW(bootstrapServer(cfg, nullptr), std::invalid_argument);
}

TEST(ServerBootstrapFailClosed, MtlsWithoutCaThrows) {
    ServerBootstrapConfig cfg;
    cfg.cert_pem = serverCertPem();
    cfg.key_pem = serverKeyPem();
    cfg.listen_address = "127.0.0.1:0";
    cfg.mutual_tls = true;
    cfg.client_ca_pem = "";  // misconfigured — mTLS demands a trust root
    EXPECT_THROW(bootstrapServer(cfg, &dummyService()),
                 std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Task 5.4 — happy path: TLS handshake plumbing + graceful shutdown.
// Uses an ephemeral port to avoid collisions in parallel CTest runs.
// ---------------------------------------------------------------------------

TEST(ServerBootstrapStartup, TlsConfigStartsAndShutsDown) {
    ServerBootstrapConfig cfg;
    cfg.cert_pem = serverCertPem();
    cfg.key_pem = serverKeyPem();
    cfg.listen_address = "127.0.0.1:0";

    std::unique_ptr<grpc::Server> server;
    ASSERT_NO_THROW(server = bootstrapServer(cfg, &dummyService()));
    ASSERT_TRUE(server != nullptr);

    // Give Shutdown a short deadline so the test never hangs even if the
    // event loop is slow to drain.
    auto deadline = std::chrono::system_clock::now() +
                    std::chrono::milliseconds(500);
    server->Shutdown(deadline);
}

TEST(ServerBootstrapStartup, MtlsConfigStartsWithClientCa) {
    // We reuse the server cert as the client CA — just verifies that the
    // mTLS path wires through SSL options without blowing up. A real
    // fingerprint-matching integration test lives outside the unit suite.
    ServerBootstrapConfig cfg;
    cfg.cert_pem = serverCertPem();
    cfg.key_pem = serverKeyPem();
    cfg.listen_address = "127.0.0.1:0";
    cfg.mutual_tls = true;
    cfg.client_ca_pem = serverCertPem();

    std::unique_ptr<grpc::Server> server;
    ASSERT_NO_THROW(server = bootstrapServer(cfg, &dummyService()));
    ASSERT_TRUE(server != nullptr);
    server->Shutdown(std::chrono::system_clock::now() +
                      std::chrono::milliseconds(500));
}

// ---------------------------------------------------------------------------
// Task 5.5 — fingerprint computation + allowlist matching
// ---------------------------------------------------------------------------

TEST(FingerprintHelpers, ComputesDeterministicSha256ForKnownCert) {
    auto fp = computeCertFingerprintSha256(serverCertPem());
    ASSERT_FALSE(fp.empty());
    // SHA-256 hex is 64 lowercase characters.
    EXPECT_EQ(fp.size(), 64u);
    for (char c : fp) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "non-lowercase-hex char in fingerprint: " << c;
    }

    // Idempotency: calling twice yields the same digest (no RNG involved).
    EXPECT_EQ(fp, computeCertFingerprintSha256(serverCertPem()));
}

TEST(FingerprintHelpers, MalformedPemYieldsEmptyFingerprint) {
    EXPECT_EQ(computeCertFingerprintSha256(""), "");
    EXPECT_EQ(computeCertFingerprintSha256("not a pem"), "");
    EXPECT_EQ(
        computeCertFingerprintSha256(
            "-----BEGIN CERTIFICATE-----\nnot-valid-b64\n-----END CERTIFICATE-----\n"),
        "");
}

TEST(FingerprintHelpers, AllowlistEmptyMeansAnyClient) {
    // Document the contract: empty allowlist is "trust all (assuming CA
    // still enforced at TLS layer)". Operators must surface this in docs.
    EXPECT_TRUE(allowlistMatches("deadbeef", {}));
}

TEST(FingerprintHelpers, AllowlistAcceptsExactMatch) {
    std::vector<std::string> allow{"aabbccddeeff"};
    EXPECT_TRUE(allowlistMatches("aabbccddeeff", allow));
}

TEST(FingerprintHelpers, AllowlistIsCaseInsensitive) {
    std::vector<std::string> allow{"AA:BB:CC:DD:EE:FF"};  // openssl-style
    EXPECT_TRUE(allowlistMatches("aabbccddeeff", allow));
    EXPECT_TRUE(allowlistMatches("AABBCCDDEEFF", allow));
}

TEST(FingerprintHelpers, AllowlistRejectsUnknown) {
    std::vector<std::string> allow{"aabbccddeeff"};
    EXPECT_FALSE(allowlistMatches("0011223344556677", allow));
}

TEST(FingerprintHelpers, AllowlistRejectsEmptyFingerprint) {
    std::vector<std::string> allow{"aabbccddeeff"};
    EXPECT_FALSE(allowlistMatches("", allow));
}

TEST(FingerprintHelpers, AllowlistRoundTripWithActualCert) {
    auto fp = computeCertFingerprintSha256(serverCertPem());
    ASSERT_FALSE(fp.empty());
    std::vector<std::string> allow{fp};
    EXPECT_TRUE(allowlistMatches(fp, allow));

    // Flip the first hex char so the digest no longer matches — proves
    // that the comparison is not a prefix-only check.
    std::string tampered = fp;
    tampered[0] = (tampered[0] == 'f') ? 'a' : 'f';
    EXPECT_FALSE(allowlistMatches(tampered, allow));
}

} // namespace
} // namespace aegisgate::control_plane::grpc_adapter
