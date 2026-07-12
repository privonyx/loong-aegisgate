// Phase 9.3 Epic 7 Task 7.3 — aegisctl config activate.
//
// Three surfaces under test:
//   * parseActivateArgs — argv wiring + --data-plane-config-path /
//                          --signal-pid handling.
//   * writeYamlAtomic    — atomicity: temporary file disappears, target
//                          ends up with the requested bytes, existing
//                          targets are overwritten.
//   * runActivate        — orchestration: RPC → file → signal, any step
//                          failure must abort the subsequent ones.
//
// Signals are verified through an injected SignalSenderFn; no real
// process is killed.

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
               ("aegisctl-activate-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    return dir;
}

std::vector<std::string> argv(std::initializer_list<const char*> args) {
    return std::vector<std::string>(args.begin(), args.end());
}

class ActivateFake : public ConfigServiceClient {
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
    grpc::Status Activate(const pb::ActivateVersionRequest& req,
                          pb::ConfigVersion* out) override {
        last_req = req;
        *out = response;
        return status;
    }
    grpc::Status Rollback(const pb::RollbackVersionRequest&, pb::ConfigVersion*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "x");
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

    pb::ActivateVersionRequest last_req;
    pb::ConfigVersion          response;
    grpc::Status               status = grpc::Status::OK;
};

pb::ConfigVersion makeActiveVersion(const std::string& id,
                                     const std::string& yaml) {
    pb::ConfigVersion v;
    v.set_version_id(id);
    v.set_status(pb::CONFIG_STATUS_ACTIVE);
    v.set_yaml_content(yaml);
    v.set_size_bytes(static_cast<int64_t>(yaml.size()));
    v.set_content_sha256(
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
    return v;
}

// --------------------------------------------------------------------------
// parseActivateArgs
// --------------------------------------------------------------------------

TEST(ParseActivateArgs, HappyPathIdAndComment) {
    auto a = parseActivateArgs(argv({"01H-ACT", "--comment", "go"}));
    EXPECT_EQ(a.version_id, "01H-ACT");
    EXPECT_EQ(a.comment, "go");
    EXPECT_TRUE(a.data_plane_config_path.empty());
    EXPECT_EQ(a.signal_pid, 0);
}

TEST(ParseActivateArgs, AcceptsDataPlaneConfigPath) {
    auto a = parseActivateArgs(argv({"01H-ACT",
                                      "--comment", "go",
                                      "--data-plane-config-path", "/etc/ag.yaml"}));
    EXPECT_EQ(a.data_plane_config_path, "/etc/ag.yaml");
}

TEST(ParseActivateArgs, AcceptsSignalPid) {
    auto a = parseActivateArgs(argv({"01H-ACT", "--comment", "go", "--signal-pid", "42"}));
    EXPECT_EQ(a.signal_pid, 42);
}

TEST(ParseActivateArgs, RejectsNonNumericSignalPid) {
    EXPECT_THROW(parseActivateArgs(argv({"01H-ACT", "--comment", "go",
                                          "--signal-pid", "abc"})),
                 std::invalid_argument);
}

TEST(ParseActivateArgs, RejectsNonPositiveSignalPid) {
    EXPECT_THROW(parseActivateArgs(argv({"01H-ACT", "--comment", "go",
                                          "--signal-pid", "0"})),
                 std::invalid_argument);
    EXPECT_THROW(parseActivateArgs(argv({"01H-ACT", "--comment", "go",
                                          "--signal-pid", "-5"})),
                 std::invalid_argument);
}

TEST(ParseActivateArgs, RejectsMissingId) {
    EXPECT_THROW(parseActivateArgs(argv({"--comment", "go"})),
                 std::invalid_argument);
}

TEST(ParseActivateArgs, RejectsMissingComment) {
    EXPECT_THROW(parseActivateArgs(argv({"01H-ACT"})), std::invalid_argument);
}

// --------------------------------------------------------------------------
// writeYamlAtomic
// --------------------------------------------------------------------------

TEST(WriteYamlAtomic, WritesContentAndRemovesTmpFile) {
    const auto target = scratch() / "atomic-01.yaml";
    std::filesystem::remove(target);

    writeYamlAtomic(target.string(), "hello: world\n");

    std::ifstream in(target);
    std::stringstream ss;
    ss << in.rdbuf();
    EXPECT_EQ(ss.str(), "hello: world\n");

    // No lingering .tmp.* files in the directory.
    for (const auto& entry : std::filesystem::directory_iterator(target.parent_path())) {
        const auto name = entry.path().filename().string();
        EXPECT_EQ(name.find("atomic-01.yaml.tmp."), std::string::npos)
            << "stray tmp file: " << name;
    }
}

TEST(WriteYamlAtomic, OverwritesExistingFile) {
    const auto target = scratch() / "atomic-02.yaml";
    std::ofstream(target) << "old\n";

    writeYamlAtomic(target.string(), "new\n");

    std::ifstream in(target);
    std::stringstream ss;
    ss << in.rdbuf();
    EXPECT_EQ(ss.str(), "new\n");
}

TEST(WriteYamlAtomic, ThrowsOnUnwritableDirectory) {
    EXPECT_THROW(writeYamlAtomic("/proc/1/root/denied.yaml", "x\n"),
                 std::runtime_error);
}

// --------------------------------------------------------------------------
// runActivate
// --------------------------------------------------------------------------

TEST(RunActivate, RpcOnlyWhenNoPathAndNoSignal) {
    ActivateFake cli;
    cli.response = makeActiveVersion("01H-RPC", "k: v\n");

    ActivateArgs args;
    args.version_id = "01H-RPC";
    args.comment    = "go";

    int signals_sent = 0;
    auto fake_signal = [&](int, int) { ++signals_sent; return 0; };

    std::ostringstream out, err;
    int rc = runActivate(cli, args, out, err, fake_signal);

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(cli.last_req.version_id(), "01H-RPC");
    EXPECT_EQ(cli.last_req.activation_comment(), "go");
    EXPECT_EQ(signals_sent, 0);
    EXPECT_NE(out.str().find("Activated"), std::string::npos);
    EXPECT_NE(out.str().find("ACTIVE"), std::string::npos);
}

TEST(RunActivate, WritesDataPlaneFileAtomically) {
    ActivateFake cli;
    const std::string yaml = "gateway:\n  port: 8080\n";
    cli.response = makeActiveVersion("01H-FILE", yaml);

    const auto target = scratch() / "dp-activate.yaml";
    std::filesystem::remove(target);

    ActivateArgs args;
    args.version_id            = "01H-FILE";
    args.comment               = "go";
    args.data_plane_config_path = target.string();

    auto noop_signal = [](int, int) { return 0; };

    std::ostringstream out, err;
    int rc = runActivate(cli, args, out, err, noop_signal);
    EXPECT_EQ(rc, 0);

    std::ifstream in(target);
    std::stringstream ss;
    ss << in.rdbuf();
    EXPECT_EQ(ss.str(), yaml);

    // output mentions the write.
    EXPECT_NE(out.str().find(target.string()), std::string::npos);
}

TEST(RunActivate, SendsSighupToProvidedPid) {
    ActivateFake cli;
    cli.response = makeActiveVersion("01H-SIG", "x\n");

    ActivateArgs args;
    args.version_id = "01H-SIG";
    args.comment    = "go";
    args.signal_pid = 99;

    int captured_pid = 0, captured_sig = 0;
    auto capture = [&](int p, int s) {
        captured_pid = p;
        captured_sig = s;
        return 0;
    };

    std::ostringstream out, err;
    int rc = runActivate(cli, args, out, err, capture);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(captured_pid, 99);
    EXPECT_EQ(captured_sig, SIGHUP);
    EXPECT_NE(out.str().find("SIGHUP"), std::string::npos);
    EXPECT_NE(out.str().find("99"), std::string::npos);
}

TEST(RunActivate, WritesThenSignals) {
    ActivateFake cli;
    const std::string yaml = "a: 1\n";
    cli.response = makeActiveVersion("01H-BOTH", yaml);

    const auto target = scratch() / "dp-both.yaml";
    std::filesystem::remove(target);

    ActivateArgs args;
    args.version_id            = "01H-BOTH";
    args.comment               = "go";
    args.data_plane_config_path = target.string();
    args.signal_pid            = 123;

    bool file_written_first = false;
    auto verify_order = [&](int, int) {
        file_written_first = std::filesystem::exists(target);
        return 0;
    };

    std::ostringstream out, err;
    EXPECT_EQ(runActivate(cli, args, out, err, verify_order), 0);
    EXPECT_TRUE(file_written_first) << "signal must fire AFTER file write";
}

TEST(RunActivate, SurfacesRpcError_NoFileNoSignal) {
    ActivateFake cli;
    cli.status = grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                               "version not APPROVED");

    const auto target = scratch() / "dp-fail.yaml";
    std::filesystem::remove(target);

    ActivateArgs args;
    args.version_id            = "01H-FAIL";
    args.comment               = "go";
    args.data_plane_config_path = target.string();
    args.signal_pid            = 1;

    int signals_sent = 0;
    auto fake_signal = [&](int, int) { ++signals_sent; return 0; };

    std::ostringstream out, err;
    int rc = runActivate(cli, args, out, err, fake_signal);
    EXPECT_NE(rc, 0);
    EXPECT_FALSE(std::filesystem::exists(target));
    EXPECT_EQ(signals_sent, 0);
    EXPECT_NE(err.str().find("FAILED_PRECONDITION"), std::string::npos);
}

TEST(RunActivate, ReportsSignalFailure) {
    ActivateFake cli;
    cli.response = makeActiveVersion("01H-SIGFAIL", "x\n");

    ActivateArgs args;
    args.version_id = "01H-SIGFAIL";
    args.comment    = "go";
    args.signal_pid = 77;

    auto failing_signal = [](int, int) { errno = EPERM; return -1; };

    std::ostringstream out, err;
    int rc = runActivate(cli, args, out, err, failing_signal);
    EXPECT_NE(rc, 0);
    EXPECT_NE(err.str().find("77"), std::string::npos);
    EXPECT_NE(err.str().find("signal"), std::string::npos);
}

TEST(RunActivate, ReportsFileWriteFailure_NoSignalSent) {
    ActivateFake cli;
    cli.response = makeActiveVersion("01H-WFAIL", "x\n");

    ActivateArgs args;
    args.version_id             = "01H-WFAIL";
    args.comment                = "go";
    // Path inside /proc that cannot be created.
    args.data_plane_config_path = "/proc/1/root/denied.yaml";
    args.signal_pid             = 88;

    int signals_sent = 0;
    auto fake_signal = [&](int, int) { ++signals_sent; return 0; };

    std::ostringstream out, err;
    int rc = runActivate(cli, args, out, err, fake_signal);
    EXPECT_NE(rc, 0);
    EXPECT_EQ(signals_sent, 0);
}

}  // namespace
}  // namespace aegisgate::cli
