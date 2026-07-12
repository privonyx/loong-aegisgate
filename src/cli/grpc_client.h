#pragma once

// Phase 9.3 Epic 7 Task 7.1 — ControlPlaneClient.
//
// Client-side counterpart of the control-plane gRPC service. Builds a
// hardened gRPC channel (TLS always on, optional mTLS, system roots by
// default), attaches a Bearer API-Key to every outbound call, and applies
// a per-RPC deadline derived from ConnectOptions::timeout_seconds.
//
// Design notes:
//   * The class is a thin orchestrator. All precondition checks live in
//     the free function `validateConnectOptions()` so the fail-closed
//     semantics (empty endpoint / api_key / mismatched mTLS pair /
//     non-positive timeout) can be unit-tested without constructing a
//     channel.
//   * Bearer injection uses `grpc::MetadataCredentialsPlugin` composed
//     onto the channel credentials. The plugin is non-blocking and
//     exposes a pure `authorizationValue()` helper so unit tests never
//     need to synthesize an `AuthContext`.
//   * SR8 (client side): the api_key is stored in ConnectOptions but must
//     never be echoed to stderr or user-facing error output. Error paths
//     surface gRPC status codes/messages — these originate in the server
//     and are already operator-safe.

#include "control_plane/v1/control_plane.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/credentials.h>

#include <memory>
#include <string>

namespace aegisgate::cli {

// --------------------------------------------------------------------------
// Pure helpers
// --------------------------------------------------------------------------

class BearerTokenCredentialsPlugin final : public grpc::MetadataCredentialsPlugin {
public:
    explicit BearerTokenCredentialsPlugin(std::string api_key);

    // Returns the value this plugin would insert for a given key. Empty
    // key returns empty string so we refuse to emit a nonsense header.
    static std::string authorizationValue(const std::string& api_key);

    grpc::Status GetMetadata(
        grpc::string_ref service_url,
        grpc::string_ref method_name,
        const grpc::AuthContext& channel_auth_context,
        std::multimap<grpc::string, grpc::string>* metadata) override;

    bool        IsBlocking() const override;
    const char* GetType()    const override;

private:
    std::string api_key_;
};

// --------------------------------------------------------------------------
// ControlPlaneClient
// --------------------------------------------------------------------------

class ControlPlaneClient {
public:
    struct ConnectOptions {
        std::string endpoint;             // host:port (required)
        std::string ca_cert_path;         // empty => use system roots
        std::string client_cert_path;     // empty => no mTLS
        std::string client_key_path;      // empty => no mTLS
        std::string api_key;              // required (Bearer token)
        int         timeout_seconds = 30; // positive (required)
    };

    explicit ControlPlaneClient(ConnectOptions opts);

    // Fresh stub; callers own its lifetime. Channel is shared so creating
    // additional stubs is cheap.
    std::unique_ptr<aegisgate::controlplane::v1::ConfigService::Stub> stub() const;

    // Pre-flight for each RPC: installs a deadline derived from
    // options().timeout_seconds. Bearer metadata is carried by the
    // channel credentials so no per-call context setup is required for
    // authentication.
    void prepareContext(grpc::ClientContext& ctx) const;

    const ConnectOptions& options() const noexcept { return opts_; }

    std::shared_ptr<grpc::Channel> channel() const noexcept { return channel_; }

private:
    ConnectOptions                 opts_;
    std::shared_ptr<grpc::Channel> channel_;
};

// Precondition checks. Throws std::invalid_argument on any violation.
// Does not touch the filesystem — file existence is validated lazily
// inside the constructor so the pure validator stays cheap + testable.
void validateConnectOptions(const ControlPlaneClient::ConnectOptions& opts);

}  // namespace aegisgate::cli
