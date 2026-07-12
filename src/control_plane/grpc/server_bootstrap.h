#pragma once

// Phase 9.3 Epic 5 Task 5.4 — ServerBootstrap.
//
// Constructs a hardened grpc::Server for the control plane:
//   * TLS always on — no insecure code path exists (SR7). Constructing
//     a server without cert+key throws std::invalid_argument.
//   * Message size cap 1 MiB matches SR2 (the control plane never needs
//     larger payloads; bigger inputs are refused before business logic).
//   * Keepalive 60 s + max concurrent streams 100 counter T12 abuse where
//     a malicious client parks many idle streams to exhaust resources.
//   * Optional mTLS (Task 5.5): enforces client certificate presentation
//     and, when an allowlist is supplied, restricts to a known set of
//     SHA-256 fingerprints.
//
// The factory returns a std::unique_ptr<grpc::Server>; callers own the
// lifetime and are expected to invoke Shutdown() before destruction.

#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>
#include <vector>

namespace aegisgate::control_plane::grpc_adapter {

struct ServerBootstrapConfig {
    std::string listen_address = "0.0.0.0:9443";

    // PEM-encoded server certificate and private key. Both are required —
    // empty either triggers a startup exception (SR7 fail-closed).
    std::string cert_pem;
    std::string key_pem;

    // mTLS options (Task 5.5). When enabled, clients must present a cert
    // rooted in `client_ca_pem`; optionally their SHA-256 fingerprint
    // (hex, lowercase) must appear in `allowed_client_fingerprints_sha256`.
    bool        mutual_tls = false;
    std::string client_ca_pem;
    std::vector<std::string> allowed_client_fingerprints_sha256;

    // gRPC hardening knobs — exposed so operators can tune per deployment.
    int keepalive_ms = 60'000;
    int max_concurrent_streams = 100;
    int max_receive_message_bytes = 1 * 1024 * 1024;  // SR2
};

// Builds (but does not start) a gRPC server bound to `cfg.listen_address`
// and registered with `service`. Throws std::invalid_argument if the TLS
// material is missing (SR7) or if mTLS is requested without a trust root.
std::unique_ptr<grpc::Server> bootstrapServer(
    const ServerBootstrapConfig& cfg,
    grpc::Service* service);

// Multi-service variant: registers all non-null services in order.
std::unique_ptr<grpc::Server> bootstrapServer(
    const ServerBootstrapConfig& cfg,
    const std::vector<grpc::Service*>& services);

// ---------------------------------------------------------------------------
// Task 5.5 — pure fingerprint helpers.
//
// Extracted as free functions so they can be unit-tested without a live
// TLS handshake. The production mTLS path calls `allowlistMatches()` in
// the interceptor after gRPC surfaces the peer certificate PEM.
// ---------------------------------------------------------------------------

// Returns the SHA-256 digest of the DER-encoded certificate rendered as
// lowercase hex. Input must be a single PEM block; returns empty string
// on malformed input.
std::string computeCertFingerprintSha256(const std::string& cert_pem);

// Case-insensitive lookup: allowlist entries may be stored in whatever
// casing operators prefer (our policy recommends lowercase hex without
// colons). Returns true when the fingerprint matches any allowlist entry.
// An empty allowlist means "trust any client whose cert chains to
// client_ca_pem" — useful during bootstrap rollouts.
bool allowlistMatches(const std::string& fingerprint_hex,
                       const std::vector<std::string>& allowlist);

} // namespace aegisgate::control_plane::grpc_adapter
