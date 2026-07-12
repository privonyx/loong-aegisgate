// Phase 9.3 Epic 7 Task 7.4 — aegisctl config rollback.
//
// Key safety invariant (SR9 / T15): `--emergency` is advertised as a
// deliberate Phase-12 reservation, not an usable feature. The server
// replies UNIMPLEMENTED; this CLI maps that response to a human message
// that explicitly states the bypass is rejected. The test matrix here
// covers both the successful standard rollback path *and* the refusal
// path to ensure the friendly error message stays in lockstep with the
// server's enforcement.

#include "cli/config_cli.h"

#include <gtest/gtest.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace aegisgate::cli {
namespace {

namespace pb = ::aegisgate::controlplane::v1;

std::filesystem::path scratch() {
    auto dir = std::filesystem::temp_directory_path() /
               ("aegisctl-rollback-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    return dir;
}

std::vector<std::string> argv(std::initializer_list<const char*> args) {
    return std::vector<std::string>(args.begin(), args.end());
}

class RollbackFake : public ConfigServiceClient {
public:
    grpc::Status Submit(const pb::SubmitVersionRequest&, pb::ConfigVersion*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "x");
    }
    grpc::Status Approve(const pb::ApproveVersionRequest&, pb::ConfigVersion*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "x");
    }
    grpc::Status Reject(const pb::RejectVersionRequest&, pb::ConfigVersion*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "x");
    }
    grpc::Status Activate(const pb::ActivateVersionRequest&, pb::ConfigVersion*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "x");
    }
    grpc::Status Rollback(const pb::RollbackVersionRequest& req,
                          pb::ConfigVersion* out) override {
        last_req = req;
        *out = response;
        return status;
    }
    grpc::Status List(const pb::ListVersionsRequest&, pb::ListVersionsResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "x");
    }
    grpc::Status GetVersion(const pb::GetVersionRequest&, pb::ConfigVersion*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "x");
    }
    grpc::Status GetActive(const pb::GetActiveRequest&, pb::ConfigVersion*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "x");
    }
    grpc::Status Diff(const pb::DiffVersionsRequest&, pb::DiffVersionsResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "x");
    }

    pb::RollbackVersionRequest last_req;
    pb::ConfigVersion          response;
    grpc::Status               status = grpc::Status::OK;
};

pb::ConfigVersion makeActive(const std::string& id, const std::string& yaml) {
    pb::ConfigVersion v;
    v.set_version_id(id);
    v.set_status(pb::CONFIG_STATUS_ACTIVE);
    v.set_yaml_content(yaml);
    v.set_size_bytes(static_cast<int64_t>(yaml.size()));
    return v;
}

// --------------------------------------------------------------------------
// parseRollbackArgs
// --------------------------------------------------------------------------

TEST(ParseRollbackArgs, HappyPathIdAndComment) {
    auto a = parseRollbackArgs(argv({"01H-ROLL", "--comment", "revert bad push"}));
    EXPECT_EQ(a.target_version_id, "01H-ROLL");
    EXPECT_EQ(a.comment, "revert bad push");
    EXPECT_FALSE(a.emergency);
    EXPECT_EQ(a.signal_pid, 0);
}

TEST(ParseRollbackArgs, AcceptsEmergencyFlag) {
    auto a = parseRollbackArgs(argv({"01H-EMG", "--comment", "!", "--emergency"}));
    EXPECT_TRUE(a.emergency);
}

TEST(ParseRollbackArgs, AcceptsDataPlaneConfigPathAndSignalPid) {
    auto a = parseRollbackArgs(argv({"01H-FULL", "--comment", "x",
                                      "--data-plane-config-path", "/tmp/out.yaml",
                                      "--signal-pid", "321"}));
    EXPECT_EQ(a.data_plane_config_path, "/tmp/out.yaml");
    EXPECT_EQ(a.signal_pid, 321);
}

TEST(ParseRollbackArgs, RejectsMissingId) {
    EXPECT_THROW(parseRollbackArgs(argv({"--comment", "x"})),
                 std::invalid_argument);
}

TEST(ParseRollbackArgs, RejectsMissingComment) {
    EXPECT_THROW(parseRollbackArgs(argv({"id"})), std::invalid_argument);
}

TEST(ParseRollbackArgs, RejectsNonNumericSignalPid) {
    EXPECT_THROW(parseRollbackArgs(argv({"id", "--comment", "x",
                                          "--signal-pid", "abc"})),
                 std::invalid_argument);
}

// --------------------------------------------------------------------------
// runRollback — happy path and side effects
// --------------------------------------------------------------------------

TEST(RunRollback, WiresTargetIdAndComment) {
    RollbackFake cli;
    cli.response = makeActive("01H-PREV", "old: yes\n");

    RollbackArgs args;
    args.target_version_id = "01H-PREV";
    args.comment = "revert rollout";

    auto noop_signal = [](int, int) { return 0; };

    std::ostringstream out, err;
    int rc = runRollback(cli, args, out, err, noop_signal);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(cli.last_req.target_version_id(), "01H-PREV");
    EXPECT_EQ(cli.last_req.rollback_comment(), "revert rollout");
    EXPECT_FALSE(cli.last_req.emergency());
    EXPECT_NE(out.str().find("Rolled back"), std::string::npos);
    EXPECT_NE(out.str().find("ACTIVE"), std::string::npos);
}

TEST(RunRollback, ForwardsDataPlaneWriteAndSignal) {
    RollbackFake cli;
    const std::string yaml = "rolled: true\n";
    cli.response = makeActive("01H-WRITE", yaml);

    const auto target = scratch() / "dp-rollback.yaml";
    std::filesystem::remove(target);

    RollbackArgs args;
    args.target_version_id     = "01H-WRITE";
    args.comment               = "revert";
    args.data_plane_config_path = target.string();
    args.signal_pid            = 44;

    int captured_pid = 0, captured_sig = 0;
    auto capture = [&](int p, int s) { captured_pid = p; captured_sig = s; return 0; };

    std::ostringstream out, err;
    EXPECT_EQ(runRollback(cli, args, out, err, capture), 0);

    std::ifstream in(target);
    std::stringstream ss;
    ss << in.rdbuf();
    EXPECT_EQ(ss.str(), yaml);
    EXPECT_EQ(captured_pid, 44);
    EXPECT_EQ(captured_sig, SIGHUP);
}

// --------------------------------------------------------------------------
// runRollback — SR9 / T15: emergency bypass is refused
// --------------------------------------------------------------------------

TEST(RunRollback, EmergencyFlagForwardedToServer) {
    RollbackFake cli;
    cli.status = grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                               "emergency rollback is reserved for Phase 12");

    RollbackArgs args;
    args.target_version_id = "01H-BAD";
    args.comment = "!";
    args.emergency = true;

    auto noop_signal = [](int, int) { return 0; };

    std::ostringstream out, err;
    int rc = runRollback(cli, args, out, err, noop_signal);
    EXPECT_NE(rc, 0);
    EXPECT_TRUE(cli.last_req.emergency())
        << "the client must still forward the request so server-side audit fires";
}

TEST(RunRollback, EmergencyRejectionRendersFriendlyMessage_SR9) {
    RollbackFake cli;
    cli.status = grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                               "emergency rollback is reserved for Phase 12");

    RollbackArgs args;
    args.target_version_id = "01H-BAD";
    args.comment = "!";
    args.emergency = true;

    auto noop_signal = [](int, int) { return 0; };
    std::ostringstream out, err;
    (void)runRollback(cli, args, out, err, noop_signal);

    const auto msg = err.str();
    EXPECT_NE(msg.find("emergency bypass"), std::string::npos);
    EXPECT_NE(msg.find("Phase 12"), std::string::npos);
    EXPECT_NE(msg.find("rejected"), std::string::npos);
}

TEST(RunRollback, EmergencyRejection_NoFileNoSignal) {
    RollbackFake cli;
    cli.status = grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                               "emergency rollback is reserved for Phase 12");

    const auto target = scratch() / "dp-emerg.yaml";
    std::filesystem::remove(target);

    RollbackArgs args;
    args.target_version_id      = "01H-EMG";
    args.comment                = "!";
    args.emergency              = true;
    args.data_plane_config_path = target.string();
    args.signal_pid             = 7;

    int signals_sent = 0;
    auto capture = [&](int, int) { ++signals_sent; return 0; };

    std::ostringstream out, err;
    (void)runRollback(cli, args, out, err, capture);
    EXPECT_FALSE(std::filesystem::exists(target));
    EXPECT_EQ(signals_sent, 0);
}

// --------------------------------------------------------------------------
// Generic error path distinct from the emergency helper
// --------------------------------------------------------------------------

TEST(RunRollback, NonEmergencyUnimplementedShowsGenericMessage) {
    // Future-proofing: if UNIMPLEMENTED ever arrives for a non-emergency
    // rollback, the friendly emergency-bypass message must NOT be shown.
    RollbackFake cli;
    cli.status = grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                               "server is too old for this RPC");

    RollbackArgs args;
    args.target_version_id = "01H-BAD";
    args.comment = "x";
    // emergency = false;

    auto noop_signal = [](int, int) { return 0; };
    std::ostringstream out, err;
    int rc = runRollback(cli, args, out, err, noop_signal);
    EXPECT_NE(rc, 0);
    EXPECT_EQ(err.str().find("emergency bypass"), std::string::npos);
    EXPECT_NE(err.str().find("UNIMPLEMENTED"), std::string::npos);
}

TEST(RunRollback, SurfacesFailedPrecondition) {
    RollbackFake cli;
    cli.status = grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                               "target must be ACTIVE or SUPERSEDED; use ActivateVersion");

    RollbackArgs args;
    args.target_version_id = "01H-PEND";
    args.comment = "x";

    auto noop_signal = [](int, int) { return 0; };
    std::ostringstream out, err;
    int rc = runRollback(cli, args, out, err, noop_signal);
    EXPECT_NE(rc, 0);
    EXPECT_NE(err.str().find("FAILED_PRECONDITION"), std::string::npos);
    EXPECT_NE(err.str().find("ActivateVersion"), std::string::npos);
}

}  // namespace
}  // namespace aegisgate::cli
