#pragma once

// Phase 9.3.4 Epic E.1 — aegisctl rollout subcommand suite.
//
// Same layering as config_cli.h: abstract client seam, pure arg parsers,
// stateless runners, top-level dispatcher.

#include "cli/grpc_client.h"
#include "cli/config_cli.h"  // OutputFormat, GlobalFlags, EnvLookupFn
#include "control_plane/v1/control_plane.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace aegisgate::cli {

// --------------------------------------------------------------------------
// Abstract seam over the 8 Rollout RPCs.
// --------------------------------------------------------------------------

class RolloutServiceClient {
public:
    virtual ~RolloutServiceClient() = default;

    virtual grpc::Status Create(
        const controlplane::v1::CreateRolloutRequest& req,
        controlplane::v1::Rollout* out) = 0;
    virtual grpc::Status Get(
        const controlplane::v1::GetRolloutRequest& req,
        controlplane::v1::Rollout* out) = 0;
    virtual grpc::Status List(
        const controlplane::v1::ListRolloutsRequest& req,
        controlplane::v1::ListRolloutsResponse* out) = 0;
    virtual grpc::Status Start(
        const controlplane::v1::StartRolloutRequest& req,
        controlplane::v1::Rollout* out) = 0;
    virtual grpc::Status Pause(
        const controlplane::v1::PauseRolloutRequest& req,
        controlplane::v1::Rollout* out) = 0;
    virtual grpc::Status Resume(
        const controlplane::v1::ResumeRolloutRequest& req,
        controlplane::v1::Rollout* out) = 0;
    virtual grpc::Status Promote(
        const controlplane::v1::PromoteRolloutRequest& req,
        controlplane::v1::Rollout* out) = 0;
    virtual grpc::Status Abort(
        const controlplane::v1::AbortRolloutRequest& req,
        controlplane::v1::Rollout* out) = 0;
};

std::unique_ptr<RolloutServiceClient> makeGrpcRolloutServiceClient(
    ControlPlaneClient& base);

// --------------------------------------------------------------------------
// Arg structs + parsers
// --------------------------------------------------------------------------

struct RolloutCreateArgs {
    std::string spec_file;
};

struct RolloutIdArgs {
    std::string rollout_id;
    std::string comment;
};

struct RolloutListArgs {
    int32_t     page_size = 50;
    std::string page_token;
};

RolloutCreateArgs parseRolloutCreateArgs(const std::vector<std::string>& argv);
RolloutIdArgs     parseRolloutIdArgs    (const std::vector<std::string>& argv);
RolloutListArgs   parseRolloutListArgs  (const std::vector<std::string>& argv);

// --------------------------------------------------------------------------
// Runners — return process exit code (0 = success)
// --------------------------------------------------------------------------

int runRolloutCreate (RolloutServiceClient& client, const RolloutCreateArgs& args,
                      std::ostream& out, std::ostream& err,
                      OutputFormat fmt = OutputFormat::Table);
int runRolloutStart  (RolloutServiceClient& client, const RolloutIdArgs& args,
                      std::ostream& out, std::ostream& err,
                      OutputFormat fmt = OutputFormat::Table);
int runRolloutPause  (RolloutServiceClient& client, const RolloutIdArgs& args,
                      std::ostream& out, std::ostream& err,
                      OutputFormat fmt = OutputFormat::Table);
int runRolloutResume (RolloutServiceClient& client, const RolloutIdArgs& args,
                      std::ostream& out, std::ostream& err,
                      OutputFormat fmt = OutputFormat::Table);
int runRolloutPromote(RolloutServiceClient& client, const RolloutIdArgs& args,
                      std::ostream& out, std::ostream& err,
                      OutputFormat fmt = OutputFormat::Table);
int runRolloutAbort  (RolloutServiceClient& client, const RolloutIdArgs& args,
                      std::ostream& out, std::ostream& err,
                      OutputFormat fmt = OutputFormat::Table);
int runRolloutStatus (RolloutServiceClient& client, const RolloutIdArgs& args,
                      std::ostream& out, std::ostream& err,
                      OutputFormat fmt = OutputFormat::Table);
int runRolloutList   (RolloutServiceClient& client, const RolloutListArgs& args,
                      std::ostream& out, std::ostream& err,
                      OutputFormat fmt = OutputFormat::Table);

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

std::string rolloutStatusToString(controlplane::v1::RolloutStatus s);
std::string pauseReasonToString(controlplane::v1::PauseReason r);

// --------------------------------------------------------------------------
// Top-level dispatcher (behind AEGISGATE_ENABLE_CONTROL_PLANE)
// --------------------------------------------------------------------------

int runRolloutCommand(const std::vector<std::string>& argv,
                      std::ostream& out, std::ostream& err);

}  // namespace aegisgate::cli
