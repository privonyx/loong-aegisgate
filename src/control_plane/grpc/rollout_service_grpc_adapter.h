#pragma once

// Phase 9.3.4 Epic C.1 — gRPC adaptation layer for RolloutService.
//
// Mirrors the ConfigServiceImpl pattern (Task 5.1/5.2): pure converters
// exposed for unit testing, an error-mapping function shared with the
// existing toGrpcStatus(), and a RolloutServiceImpl that delegates to
// RolloutController while enforcing SR1 (SuperAdmin gate) via the
// injected UserExtractor.

#include "control_plane/v1/control_plane.grpc.pb.h"
#include "control_plane/v1/control_plane.pb.h"

#include "control_plane/rollout/rollout_record.h"

#include <grpcpp/grpcpp.h>

#include <functional>
#include <optional>
#include <string>

namespace aegisgate {

class RolloutController;

namespace control_plane::grpc_adapter {

// Re-use the same UserExtractor type from config_service_grpc_adapter.h.
using UserExtractor = std::function<std::string(grpc::ServerContext*)>;

// -------------------------------------------------------------------------
// Pure converters — proto ↔ POCO
// -------------------------------------------------------------------------

controlplane::v1::RolloutStatus rolloutStatusToProto(RolloutStatus s);
std::optional<RolloutStatus> rolloutStatusFromProto(controlplane::v1::RolloutStatus s);

controlplane::v1::PauseReason pauseReasonToProto(PauseReason r);
std::optional<PauseReason> pauseReasonFromProto(controlplane::v1::PauseReason r);

controlplane::v1::Rollout rolloutToProto(const RolloutRecord& rec);
RolloutRecord rolloutFromProto(const controlplane::v1::Rollout& msg);

RolloutSpec rolloutSpecFromProto(const controlplane::v1::RolloutSpec& msg);
controlplane::v1::RolloutSpec rolloutSpecToProto(const RolloutSpec& spec);

// -------------------------------------------------------------------------
// RolloutServiceImpl — 8 RPCs
// -------------------------------------------------------------------------

class RolloutServiceImpl final
    : public controlplane::v1::RolloutService::Service {
public:
    RolloutServiceImpl(RolloutController* ctrl, UserExtractor extractor);

    grpc::Status GetRollout(
        grpc::ServerContext* ctx,
        const controlplane::v1::GetRolloutRequest* req,
        controlplane::v1::Rollout* resp) override;

    grpc::Status ListRollouts(
        grpc::ServerContext* ctx,
        const controlplane::v1::ListRolloutsRequest* req,
        controlplane::v1::ListRolloutsResponse* resp) override;

    grpc::Status CreateRollout(
        grpc::ServerContext* ctx,
        const controlplane::v1::CreateRolloutRequest* req,
        controlplane::v1::Rollout* resp) override;

    grpc::Status StartRollout(
        grpc::ServerContext* ctx,
        const controlplane::v1::StartRolloutRequest* req,
        controlplane::v1::Rollout* resp) override;

    grpc::Status PauseRollout(
        grpc::ServerContext* ctx,
        const controlplane::v1::PauseRolloutRequest* req,
        controlplane::v1::Rollout* resp) override;

    grpc::Status ResumeRollout(
        grpc::ServerContext* ctx,
        const controlplane::v1::ResumeRolloutRequest* req,
        controlplane::v1::Rollout* resp) override;

    grpc::Status PromoteRollout(
        grpc::ServerContext* ctx,
        const controlplane::v1::PromoteRolloutRequest* req,
        controlplane::v1::Rollout* resp) override;

    grpc::Status AbortRollout(
        grpc::ServerContext* ctx,
        const controlplane::v1::AbortRolloutRequest* req,
        controlplane::v1::Rollout* resp) override;

private:
    RolloutController* ctrl_;
    UserExtractor      extract_user_;
};

} // namespace control_plane::grpc_adapter
} // namespace aegisgate
