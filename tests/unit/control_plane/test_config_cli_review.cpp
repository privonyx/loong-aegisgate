// Phase 9.3 Epic 7 Task 7.2 — aegisctl config apply / approve / reject.
//
// Argument parsers are tested as pure functions. The runners
// (runApply / runApprove / runReject) are driven through a fake
// ConfigServiceClient that captures the request and returns a scripted
// response/status so we can assert:
//   * the request fields wired from argv are correct
//   * stdout renders version_id / status / sha256 / size_bytes
//   * non-OK status is surfaced to stderr with the server-supplied message
//   * the api_key is never echoed (SR8 client-side hygiene)

#include "cli/config_cli.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace aegisgate::cli {
namespace {

namespace pb = ::aegisgate::controlplane::v1;

// --------------------------------------------------------------------------
// Test helpers
// --------------------------------------------------------------------------

std::filesystem::path scratch() {
    auto dir = std::filesystem::temp_directory_path() /
               ("aegisctl-cli-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    return dir;
}

std::string writeTempYaml(const std::string& name, const std::string& body) {
    const auto p = scratch() / name;
    std::ofstream(p) << body;
    return p.string();
}

std::vector<std::string> argv(std::initializer_list<const char*> args) {
    return std::vector<std::string>(args.begin(), args.end());
}

// Capture-only fake. Records the last request of each type; returns the
// scripted status + response from setters.
class FakeClient : public ConfigServiceClient {
public:
    grpc::Status Submit(const pb::SubmitVersionRequest& req,
                        pb::ConfigVersion* out) override {
        last_submit = req;
        *out = submit_response;
        return submit_status;
    }
    grpc::Status Approve(const pb::ApproveVersionRequest& req,
                         pb::ConfigVersion* out) override {
        last_approve = req;
        *out = approve_response;
        return approve_status;
    }
    grpc::Status Reject(const pb::RejectVersionRequest& req,
                        pb::ConfigVersion* out) override {
        last_reject = req;
        *out = reject_response;
        return reject_status;
    }
    grpc::Status Activate(const pb::ActivateVersionRequest&,
                          pb::ConfigVersion*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "no activate");
    }
    grpc::Status Rollback(const pb::RollbackVersionRequest&,
                          pb::ConfigVersion*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "no rollback");
    }
    grpc::Status List(const pb::ListVersionsRequest&,
                      pb::ListVersionsResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "no list");
    }
    grpc::Status GetVersion(const pb::GetVersionRequest&,
                             pb::ConfigVersion*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "no get");
    }
    grpc::Status GetActive(const pb::GetActiveRequest&,
                            pb::ConfigVersion*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "no active");
    }
    grpc::Status Diff(const pb::DiffVersionsRequest&,
                      pb::DiffVersionsResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "no diff");
    }

    pb::SubmitVersionRequest  last_submit;
    pb::ApproveVersionRequest last_approve;
    pb::RejectVersionRequest  last_reject;

    pb::ConfigVersion submit_response;
    pb::ConfigVersion approve_response;
    pb::ConfigVersion reject_response;

    grpc::Status submit_status  = grpc::Status::OK;
    grpc::Status approve_status = grpc::Status::OK;
    grpc::Status reject_status  = grpc::Status::OK;
};

pb::ConfigVersion makeVersion(const std::string& id, pb::ConfigStatus status) {
    pb::ConfigVersion v;
    v.set_version_id(id);
    v.set_status(status);
    v.set_content_sha256(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    v.set_size_bytes(123);
    v.set_submitter("super-admin-1");
    return v;
}

// --------------------------------------------------------------------------
// parseApplyArgs
// --------------------------------------------------------------------------

TEST(ParseApplyArgs, HappyPathMinimal) {
    auto a = parseApplyArgs(argv({"config.yaml", "--comment", "rollout v5"}));
    EXPECT_EQ(a.file_path, "config.yaml");
    EXPECT_EQ(a.comment, "rollout v5");
    EXPECT_FALSE(a.dry_run);
}

TEST(ParseApplyArgs, AcceptsDryRunFlag) {
    auto a = parseApplyArgs(argv({"c.yaml", "--comment", "x", "--dry-run"}));
    EXPECT_TRUE(a.dry_run);
}

TEST(ParseApplyArgs, DryRunFlagOrderInsensitive) {
    auto a = parseApplyArgs(argv({"c.yaml", "--dry-run", "--comment", "x"}));
    EXPECT_TRUE(a.dry_run);
    EXPECT_EQ(a.comment, "x");
}

TEST(ParseApplyArgs, ThrowsOnMissingFile) {
    EXPECT_THROW(parseApplyArgs(argv({"--comment", "x"})),
                 std::invalid_argument);
}

TEST(ParseApplyArgs, ThrowsOnCommentWithoutValue) {
    EXPECT_THROW(parseApplyArgs(argv({"c.yaml", "--comment"})),
                 std::invalid_argument);
}

TEST(ParseApplyArgs, ThrowsOnMissingComment) {
    // Policy: comment is required so every change carries reviewer context.
    EXPECT_THROW(parseApplyArgs(argv({"c.yaml"})), std::invalid_argument);
}

TEST(ParseApplyArgs, ThrowsOnEmpty) {
    EXPECT_THROW(parseApplyArgs(argv({})), std::invalid_argument);
}

// --------------------------------------------------------------------------
// parseApproveArgs / parseRejectArgs (share the ReviewArgs shape)
// --------------------------------------------------------------------------

TEST(ParseReviewArgs, ApproveHappyPath) {
    auto a = parseApproveArgs(argv({"01H...ABC", "--comment", "lgtm"}));
    EXPECT_EQ(a.version_id, "01H...ABC");
    EXPECT_EQ(a.comment, "lgtm");
}

TEST(ParseReviewArgs, ApproveThrowsOnMissingId) {
    EXPECT_THROW(parseApproveArgs(argv({"--comment", "lgtm"})),
                 std::invalid_argument);
}

TEST(ParseReviewArgs, ApproveThrowsOnMissingComment) {
    EXPECT_THROW(parseApproveArgs(argv({"01H...ABC"})),
                 std::invalid_argument);
}

TEST(ParseReviewArgs, RejectHappyPath) {
    auto a = parseRejectArgs(argv({"01H...XYZ", "--comment", "fails validation"}));
    EXPECT_EQ(a.version_id, "01H...XYZ");
    EXPECT_EQ(a.comment, "fails validation");
}

TEST(ParseReviewArgs, RejectThrowsOnMissingComment) {
    EXPECT_THROW(parseRejectArgs(argv({"id"})), std::invalid_argument);
}

// --------------------------------------------------------------------------
// runApply
// --------------------------------------------------------------------------

TEST(RunApply, HappyPathPrintsTable) {
    FakeClient cli;
    cli.submit_response = makeVersion("01H-ABC", pb::CONFIG_STATUS_PENDING);

    ApplyArgs args;
    args.file_path = writeTempYaml("happy.yaml", "key: value\n");
    args.comment = "initial rollout";

    std::ostringstream out, err;
    int rc = runApply(cli, args, out, err);

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(cli.last_submit.submitter_comment(), "initial rollout");
    EXPECT_EQ(cli.last_submit.yaml_content(), "key: value\n");
    EXPECT_FALSE(cli.last_submit.validate_only());

    const auto output = out.str();
    EXPECT_NE(output.find("01H-ABC"), std::string::npos);
    EXPECT_NE(output.find("PENDING"), std::string::npos);
    EXPECT_NE(output.find("123"), std::string::npos);          // size
    EXPECT_NE(output.find("0123456789abcdef"), std::string::npos);  // sha256 prefix
}

TEST(RunApply, DryRunSetsValidateOnly) {
    FakeClient cli;
    cli.submit_response = makeVersion("01H-DRY", pb::CONFIG_STATUS_PENDING);

    ApplyArgs args;
    args.file_path = writeTempYaml("dry.yaml", "ok: true\n");
    args.comment = "validate";
    args.dry_run = true;

    std::ostringstream out, err;
    EXPECT_EQ(runApply(cli, args, out, err), 0);
    EXPECT_TRUE(cli.last_submit.validate_only());
}

TEST(RunApply, ReportsMissingFile) {
    FakeClient cli;
    ApplyArgs args;
    args.file_path = "/no/such/file.yaml";
    args.comment = "x";

    std::ostringstream out, err;
    int rc = runApply(cli, args, out, err);
    EXPECT_NE(rc, 0);
    EXPECT_NE(err.str().find("/no/such/file.yaml"), std::string::npos);
    // Fake must never have been called.
    EXPECT_TRUE(cli.last_submit.submitter_comment().empty());
}

TEST(RunApply, SurfacesServerError) {
    FakeClient cli;
    cli.submit_status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                      "yaml_content exceeds 1 MiB");

    ApplyArgs args;
    args.file_path = writeTempYaml("big.yaml", "x: 1\n");
    args.comment = "too big";

    std::ostringstream out, err;
    int rc = runApply(cli, args, out, err);
    EXPECT_NE(rc, 0);
    EXPECT_NE(err.str().find("INVALID_ARGUMENT"), std::string::npos);
    EXPECT_NE(err.str().find("yaml_content exceeds 1 MiB"), std::string::npos);
}

TEST(RunApply, NeverEchoesApiKey_SR8) {
    FakeClient cli;
    cli.submit_status = grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                      "invalid credentials");

    ApplyArgs args;
    args.file_path = writeTempYaml("auth.yaml", "x: 1\n");
    args.comment = "x";

    std::ostringstream out, err;
    (void)runApply(cli, args, out, err);
    // The runner must not print anything that looks like an api_key
    // (common prefix "sk-" used by tests and real operators).
    EXPECT_EQ(err.str().find("sk-"), std::string::npos);
    EXPECT_EQ(out.str().find("sk-"), std::string::npos);
}

// --------------------------------------------------------------------------
// runApprove / runReject
// --------------------------------------------------------------------------

TEST(RunApprove, WiresVersionIdAndCommentAndRenders) {
    FakeClient cli;
    cli.approve_response = makeVersion("01H-APPR", pb::CONFIG_STATUS_APPROVED);

    ReviewArgs args;
    args.version_id = "01H-APPR";
    args.comment = "lgtm";

    std::ostringstream out, err;
    int rc = runApprove(cli, args, out, err);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(cli.last_approve.version_id(), "01H-APPR");
    EXPECT_EQ(cli.last_approve.reviewer_comment(), "lgtm");
    EXPECT_NE(out.str().find("APPROVED"), std::string::npos);
    EXPECT_NE(out.str().find("01H-APPR"), std::string::npos);
}

TEST(RunApprove, SurfacesPermissionDenied) {
    FakeClient cli;
    cli.approve_status = grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                       "submitter cannot approve own version");

    ReviewArgs args;
    args.version_id = "01H-SELF";
    args.comment = "...";

    std::ostringstream out, err;
    int rc = runApprove(cli, args, out, err);
    EXPECT_NE(rc, 0);
    EXPECT_NE(err.str().find("PERMISSION_DENIED"), std::string::npos);
    EXPECT_NE(err.str().find("submitter cannot approve own version"),
              std::string::npos);
}

TEST(RunReject, WiresVersionIdAndCommentAndRenders) {
    FakeClient cli;
    cli.reject_response = makeVersion("01H-REJ", pb::CONFIG_STATUS_REJECTED);

    ReviewArgs args;
    args.version_id = "01H-REJ";
    args.comment = "fails invariant";

    std::ostringstream out, err;
    int rc = runReject(cli, args, out, err);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(cli.last_reject.version_id(), "01H-REJ");
    EXPECT_EQ(cli.last_reject.reviewer_comment(), "fails invariant");
    EXPECT_NE(out.str().find("REJECTED"), std::string::npos);
}

}  // namespace
}  // namespace aegisgate::cli
