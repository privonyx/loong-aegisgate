#pragma once

// Phase 9.3 Epic 5 — gRPC adaptation layer for the control plane.
//
// Only compiled when ENABLE_CONTROL_PLANE=ON. The adapter keeps a strict
// separation between the pure C++ business layer (aegisgate_core) and the
// proto-generated types: this is the ONLY place where both sides of the
// boundary are visible.
//
// Design notes:
//   * All converters are pure and throw no exceptions — they work on a copy
//     and return by value so the 1 MiB yaml_content never aliases after
//     mutation.
//   * `statusFromProto` returns std::optional: UNSPECIFIED and out-of-range
//     values map to nullopt so handlers can fail with INVALID_ARGUMENT
//     instead of silently accepting PENDING.
//   * The Impl class in the companion .cpp maps ConfigServiceCore error
//     codes to grpc::Status with SR8-safe messaging (non-user-facing
//     strings are flattened to "internal error").

#include "control_plane/v1/control_plane.grpc.pb.h"
#include "control_plane/v1/control_plane.pb.h"

#include "control_plane/config_version_record.h"

#include <grpcpp/grpcpp.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace aegisgate {

class ConfigServiceCore;

namespace control_plane::grpc_adapter {

// -------------------------------------------------------------------------
// Pure converters — Task 5.1. Exposed for direct unit testing.
// -------------------------------------------------------------------------

aegisgate::controlplane::v1::ConfigStatus statusToProto(ConfigStatus s);

std::optional<ConfigStatus> statusFromProto(
    aegisgate::controlplane::v1::ConfigStatus s);

aegisgate::controlplane::v1::ConfigVersion toProto(
    const ConfigVersionRecord& rec);

// yaml_content stripping (SR11) happens *before* this converter — callers
// must call `stripYaml(rec)` explicitly for list responses. The converter
// does not second-guess the caller so GetVersion can still return the
// bundle text.
ConfigVersionRecord fromProto(
    const aegisgate::controlplane::v1::ConfigVersion& msg);

// -------------------------------------------------------------------------
// Error mapping — Task 5.2. Exposed for direct unit testing.
// -------------------------------------------------------------------------

// Translates a ConfigServiceCore error_code to a grpc::Status. The message
// passed through from the core is assumed to be operator-safe (no raw
// stack traces or host-specific paths — DiffEngine already strips those).
// Unknown codes collapse to INTERNAL with a generic string so SR8 holds.
grpc::Status toGrpcStatus(const std::string& error_code,
                           const std::string& error_message);

// -------------------------------------------------------------------------
// ConfigServiceImpl — Task 5.2
// -------------------------------------------------------------------------

// UserExtractor returns the authenticated SuperAdmin user_id for the given
// gRPC ServerContext. It is injected rather than hardcoded so the impl
// remains unit-testable without a running AuthInterceptor (Task 5.3).
// Returning an empty string means "not authenticated" — the handler will
// respond with UNAUTHENTICATED.
using UserExtractor = std::function<std::string(grpc::ServerContext*)>;

class ConfigServiceImpl final
    : public aegisgate::controlplane::v1::ConfigService::Service {
public:
    ConfigServiceImpl(ConfigServiceCore* core, UserExtractor extractor);

    grpc::Status ListVersions(
        grpc::ServerContext* ctx,
        const aegisgate::controlplane::v1::ListVersionsRequest* req,
        aegisgate::controlplane::v1::ListVersionsResponse* resp) override;

    grpc::Status GetVersion(
        grpc::ServerContext* ctx,
        const aegisgate::controlplane::v1::GetVersionRequest* req,
        aegisgate::controlplane::v1::ConfigVersion* resp) override;

    grpc::Status GetActive(
        grpc::ServerContext* ctx,
        const aegisgate::controlplane::v1::GetActiveRequest* req,
        aegisgate::controlplane::v1::ConfigVersion* resp) override;

    grpc::Status DiffVersions(
        grpc::ServerContext* ctx,
        const aegisgate::controlplane::v1::DiffVersionsRequest* req,
        aegisgate::controlplane::v1::DiffVersionsResponse* resp) override;

    grpc::Status SubmitVersion(
        grpc::ServerContext* ctx,
        const aegisgate::controlplane::v1::SubmitVersionRequest* req,
        aegisgate::controlplane::v1::ConfigVersion* resp) override;

    grpc::Status ApproveVersion(
        grpc::ServerContext* ctx,
        const aegisgate::controlplane::v1::ApproveVersionRequest* req,
        aegisgate::controlplane::v1::ConfigVersion* resp) override;

    grpc::Status RejectVersion(
        grpc::ServerContext* ctx,
        const aegisgate::controlplane::v1::RejectVersionRequest* req,
        aegisgate::controlplane::v1::ConfigVersion* resp) override;

    grpc::Status ActivateVersion(
        grpc::ServerContext* ctx,
        const aegisgate::controlplane::v1::ActivateVersionRequest* req,
        aegisgate::controlplane::v1::ConfigVersion* resp) override;

    grpc::Status RollbackVersion(
        grpc::ServerContext* ctx,
        const aegisgate::controlplane::v1::RollbackVersionRequest* req,
        aegisgate::controlplane::v1::ConfigVersion* resp) override;

private:
    ConfigServiceCore* core_;
    UserExtractor      extract_user_;
};

} // namespace control_plane::grpc_adapter
} // namespace aegisgate
