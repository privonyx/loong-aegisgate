// Phase 9.3 Epic 7 Task 7.5 — aegisctl config list / get / show / current / diff.
//
// Key concerns:
//   * list/get/current wire the request correctly and render a metadata
//     table; yaml_content is never echoed (SR11 + SR14 hygiene).
//   * `show --redact` runs the bundle through `redactYaml()` before
//     printing so operators don't spill secrets when inspecting versions.
//   * `diff` happily passes both ids to the server, and falls back to the
//     current ACTIVE when only one id is provided.

#include "cli/config_cli.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

namespace aegisgate::cli {
namespace {

namespace pb = ::aegisgate::controlplane::v1;

std::vector<std::string> argv(std::initializer_list<const char*> args) {
    return std::vector<std::string>(args.begin(), args.end());
}

class ReadFake : public ConfigServiceClient {
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
    grpc::Status Rollback(const pb::RollbackVersionRequest&, pb::ConfigVersion*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "x");
    }
    grpc::Status List(const pb::ListVersionsRequest& req,
                      pb::ListVersionsResponse* out) override {
        last_list = req;
        *out = list_response;
        return list_status;
    }
    grpc::Status GetVersion(const pb::GetVersionRequest& req,
                             pb::ConfigVersion* out) override {
        last_get = req;
        *out = get_response;
        return get_status;
    }
    grpc::Status GetActive(const pb::GetActiveRequest& req,
                            pb::ConfigVersion* out) override {
        last_active_req = req;
        *out = active_response;
        return active_status;
    }
    grpc::Status Diff(const pb::DiffVersionsRequest& req,
                      pb::DiffVersionsResponse* out) override {
        last_diff = req;
        *out = diff_response;
        return diff_status;
    }

    pb::ListVersionsRequest   last_list;
    pb::GetVersionRequest     last_get;
    pb::GetActiveRequest      last_active_req;
    pb::DiffVersionsRequest   last_diff;

    pb::ListVersionsResponse  list_response;
    pb::ConfigVersion         get_response;
    pb::ConfigVersion         active_response;
    pb::DiffVersionsResponse  diff_response;

    grpc::Status list_status   = grpc::Status::OK;
    grpc::Status get_status    = grpc::Status::OK;
    grpc::Status active_status = grpc::Status::OK;
    grpc::Status diff_status   = grpc::Status::OK;
};

pb::ConfigVersion makeVersion(const std::string& id,
                                pb::ConfigStatus st,
                                const std::string& yaml = "") {
    pb::ConfigVersion v;
    v.set_version_id(id);
    v.set_status(st);
    v.set_size_bytes(static_cast<int64_t>(yaml.size()));
    v.set_content_sha256("sha_" + id);
    if (!yaml.empty()) v.set_yaml_content(yaml);
    return v;
}

// --------------------------------------------------------------------------
// parseListArgs
// --------------------------------------------------------------------------

TEST(ParseListArgs, DefaultsWhenNoFlags) {
    auto a = parseListArgs(argv({}));
    EXPECT_TRUE(a.statuses.empty());
    EXPECT_EQ(a.since_millis, 0);
    EXPECT_EQ(a.page_size, 50);
    EXPECT_TRUE(a.page_token.empty());
}

TEST(ParseListArgs, AcceptsCommaSeparatedStatuses) {
    auto a = parseListArgs(argv({"--statuses", "PENDING,APPROVED"}));
    ASSERT_EQ(a.statuses.size(), 2u);
    EXPECT_EQ(a.statuses[0], pb::CONFIG_STATUS_PENDING);
    EXPECT_EQ(a.statuses[1], pb::CONFIG_STATUS_APPROVED);
}

TEST(ParseListArgs, StatusParsingIsCaseInsensitive) {
    auto a = parseListArgs(argv({"--statuses", "active,superseded"}));
    ASSERT_EQ(a.statuses.size(), 2u);
    EXPECT_EQ(a.statuses[0], pb::CONFIG_STATUS_ACTIVE);
    EXPECT_EQ(a.statuses[1], pb::CONFIG_STATUS_SUPERSEDED);
}

TEST(ParseListArgs, RejectsUnknownStatus) {
    EXPECT_THROW(parseListArgs(argv({"--statuses", "NONSENSE"})),
                 std::invalid_argument);
}

TEST(ParseListArgs, AcceptsSinceAndPageSize) {
    auto a = parseListArgs(argv({"--since", "1700000000000",
                                  "--page-size", "100"}));
    EXPECT_EQ(a.since_millis, 1700000000000LL);
    EXPECT_EQ(a.page_size, 100);
}

TEST(ParseListArgs, RejectsNonPositivePageSize) {
    EXPECT_THROW(parseListArgs(argv({"--page-size", "0"})),
                 std::invalid_argument);
}

TEST(ParseListArgs, AcceptsPageToken) {
    auto a = parseListArgs(argv({"--page-token", "abc=="}));
    EXPECT_EQ(a.page_token, "abc==");
}

// --------------------------------------------------------------------------
// parseGetArgs / parseShowArgs / parseDiffArgs
// --------------------------------------------------------------------------

TEST(ParseGetArgs, HappyPath) {
    auto a = parseGetArgs(argv({"01H-ID"}));
    EXPECT_EQ(a.version_id, "01H-ID");
}

TEST(ParseGetArgs, RejectsEmpty) {
    EXPECT_THROW(parseGetArgs(argv({})), std::invalid_argument);
}

TEST(ParseShowArgs, HappyPathNoRedact) {
    auto a = parseShowArgs(argv({"01H-ID"}));
    EXPECT_EQ(a.version_id, "01H-ID");
    EXPECT_FALSE(a.redact);
}

TEST(ParseShowArgs, HappyPathWithRedact) {
    auto a = parseShowArgs(argv({"01H-ID", "--redact"}));
    EXPECT_TRUE(a.redact);
}

TEST(ParseShowArgs, OrderInsensitive) {
    auto a = parseShowArgs(argv({"--redact", "01H-ID"}));
    EXPECT_EQ(a.version_id, "01H-ID");
    EXPECT_TRUE(a.redact);
}

TEST(ParseDiffArgs, OneArg) {
    auto a = parseDiffArgs(argv({"01H-A"}));
    EXPECT_EQ(a.from_version_id, "01H-A");
    EXPECT_TRUE(a.to_version_id.empty());
}

TEST(ParseDiffArgs, TwoArgs) {
    auto a = parseDiffArgs(argv({"01H-A", "01H-B"}));
    EXPECT_EQ(a.from_version_id, "01H-A");
    EXPECT_EQ(a.to_version_id,   "01H-B");
}

TEST(ParseDiffArgs, RejectsZeroArgs) {
    EXPECT_THROW(parseDiffArgs(argv({})), std::invalid_argument);
}

TEST(ParseDiffArgs, RejectsTooManyArgs) {
    EXPECT_THROW(parseDiffArgs(argv({"a", "b", "c"})),
                 std::invalid_argument);
}

// --------------------------------------------------------------------------
// redactYaml
// --------------------------------------------------------------------------

TEST(RedactYaml, ReplacesApiKeyValue) {
    const std::string in =
        "upstream:\n"
        "  api_key: sk-ABCDEFGHIJKLMNOPQRSTUVWXYZ\n"
        "  timeout: 30\n";
    const auto out = redactYaml(in);
    EXPECT_EQ(out.find("sk-ABCDEFGHIJKLMNOPQRSTUVWXYZ"), std::string::npos);
    EXPECT_NE(out.find("<redacted>"), std::string::npos);
    EXPECT_NE(out.find("timeout: 30"), std::string::npos);
}

TEST(RedactYaml, IdempotentWhenNoSecrets) {
    const std::string in = "gateway:\n  port: 8080\n  workers: 4\n";
    EXPECT_EQ(redactYaml(in), in);
}

TEST(RedactYaml, RedactsPassword) {
    const std::string in = "db:\n  password: s3cr3tP4ss\n";
    const auto out = redactYaml(in);
    EXPECT_EQ(out.find("s3cr3tP4ss"), std::string::npos);
    EXPECT_NE(out.find("<redacted>"), std::string::npos);
}

// --------------------------------------------------------------------------
// runList
// --------------------------------------------------------------------------

TEST(RunList, WiresStatusesAndPagingAndRendersRows) {
    ReadFake cli;
    *cli.list_response.add_versions() = makeVersion("01H-A", pb::CONFIG_STATUS_PENDING);
    *cli.list_response.add_versions() = makeVersion("01H-B", pb::CONFIG_STATUS_ACTIVE);
    cli.list_response.set_next_page_token("next-tok");

    ListArgs args;
    args.statuses = {pb::CONFIG_STATUS_PENDING, pb::CONFIG_STATUS_ACTIVE};
    args.since_millis = 1234;
    args.page_size = 25;

    std::ostringstream out, err;
    int rc = runList(cli, args, out, err);
    EXPECT_EQ(rc, 0);

    EXPECT_EQ(cli.last_list.statuses_size(), 2);
    EXPECT_EQ(cli.last_list.since_millis(), 1234);
    EXPECT_EQ(cli.last_list.page_size(), 25);

    EXPECT_NE(out.str().find("01H-A"), std::string::npos);
    EXPECT_NE(out.str().find("01H-B"), std::string::npos);
    EXPECT_NE(out.str().find("PENDING"), std::string::npos);
    EXPECT_NE(out.str().find("ACTIVE"), std::string::npos);
    EXPECT_NE(out.str().find("next-tok"), std::string::npos);
}

TEST(RunList, EmptyResponsePrintsMessage) {
    ReadFake cli;  // empty list_response
    std::ostringstream out, err;
    int rc = runList(cli, {}, out, err);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.str().find("no versions"), std::string::npos);
}

TEST(RunList, SurfacesServerError) {
    ReadFake cli;
    cli.list_status = grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "nope");
    std::ostringstream out, err;
    EXPECT_NE(runList(cli, {}, out, err), 0);
    EXPECT_NE(err.str().find("PERMISSION_DENIED"), std::string::npos);
}

// --------------------------------------------------------------------------
// runGet / runCurrent
// --------------------------------------------------------------------------

TEST(RunGet, WiresIdAndRendersMetadataOnly_SR14) {
    ReadFake cli;
    cli.get_response = makeVersion("01H-X", pb::CONFIG_STATUS_APPROVED,
                                    "api_key: sk-SHOULDNEVERAPPEAR1234567890\n");

    GetArgs args;
    args.version_id = "01H-X";

    std::ostringstream out, err;
    int rc = runGet(cli, args, out, err);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(cli.last_get.version_id(), "01H-X");
    EXPECT_NE(out.str().find("01H-X"), std::string::npos);
    EXPECT_NE(out.str().find("APPROVED"), std::string::npos);
    // SR14: yaml_content must not leak through the metadata-only table.
    EXPECT_EQ(out.str().find("sk-SHOULDNEVERAPPEAR"), std::string::npos);
    EXPECT_EQ(out.str().find("api_key:"), std::string::npos);
}

TEST(RunCurrent, RendersActiveMetadata) {
    ReadFake cli;
    cli.active_response = makeVersion("01H-LIVE", pb::CONFIG_STATUS_ACTIVE);

    std::ostringstream out, err;
    int rc = runCurrent(cli, out, err);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.str().find("01H-LIVE"), std::string::npos);
    EXPECT_NE(out.str().find("ACTIVE"), std::string::npos);
}

TEST(RunCurrent, SurfacesNotFound) {
    ReadFake cli;
    cli.active_status = grpc::Status(grpc::StatusCode::NOT_FOUND,
                                      "no active version");
    std::ostringstream out, err;
    EXPECT_NE(runCurrent(cli, out, err), 0);
    EXPECT_NE(err.str().find("NOT_FOUND"), std::string::npos);
}

// --------------------------------------------------------------------------
// runShow — secret redaction (SR14)
// --------------------------------------------------------------------------

TEST(RunShow, PrintsRawYamlWhenNotRedacted) {
    ReadFake cli;
    const std::string yaml = "gateway:\n  port: 8080\n  name: prod\n";
    cli.get_response = makeVersion("01H-SHOW", pb::CONFIG_STATUS_ACTIVE, yaml);

    ShowArgs args;
    args.version_id = "01H-SHOW";

    std::ostringstream out, err;
    EXPECT_EQ(runShow(cli, args, out, err), 0);
    EXPECT_NE(out.str().find("port: 8080"), std::string::npos);
    EXPECT_NE(out.str().find("name: prod"), std::string::npos);
}

TEST(RunShow, RedactsSecretsWhenRedactFlag_SR14) {
    ReadFake cli;
    const std::string yaml =
        "upstream:\n"
        "  api_key: sk-ABCDEFGHIJKLMNOPQRSTUVWXYZ\n"
        "  timeout: 30\n";
    cli.get_response = makeVersion("01H-RED", pb::CONFIG_STATUS_ACTIVE, yaml);

    ShowArgs args;
    args.version_id = "01H-RED";
    args.redact     = true;

    std::ostringstream out, err;
    EXPECT_EQ(runShow(cli, args, out, err), 0);
    EXPECT_EQ(out.str().find("sk-ABCDEFGHIJKLMNOPQRSTUVWXYZ"),
              std::string::npos);
    EXPECT_NE(out.str().find("<redacted>"), std::string::npos);
    EXPECT_NE(out.str().find("timeout: 30"), std::string::npos);
}

TEST(RunShow, SurfacesNotFound) {
    ReadFake cli;
    cli.get_status = grpc::Status(grpc::StatusCode::NOT_FOUND,
                                   "version unknown");
    ShowArgs args; args.version_id = "nope";
    std::ostringstream out, err;
    EXPECT_NE(runShow(cli, args, out, err), 0);
    EXPECT_NE(err.str().find("NOT_FOUND"), std::string::npos);
}

// --------------------------------------------------------------------------
// runDiff
// --------------------------------------------------------------------------

TEST(RunDiff, WiresBothIdsAndPrintsUnifiedText) {
    ReadFake cli;
    cli.diff_response.set_unified_diff(
        "--- a\n+++ b\n@@ -1,1 +1,1 @@\n-foo\n+bar\n");

    DiffArgs args;
    args.from_version_id = "01H-A";
    args.to_version_id   = "01H-B";

    std::ostringstream out, err;
    EXPECT_EQ(runDiff(cli, args, out, err), 0);
    EXPECT_EQ(cli.last_diff.from_version_id(), "01H-A");
    EXPECT_EQ(cli.last_diff.to_version_id(),   "01H-B");
    EXPECT_NE(out.str().find("-foo"), std::string::npos);
    EXPECT_NE(out.str().find("+bar"), std::string::npos);
}

TEST(RunDiff, SingleArgResolvesToCurrentActive) {
    ReadFake cli;
    cli.active_response = makeVersion("01H-LIVE", pb::CONFIG_STATUS_ACTIVE);
    cli.diff_response.set_unified_diff("diff-body\n");

    DiffArgs args;
    args.from_version_id = "01H-A";
    // to intentionally empty.

    std::ostringstream out, err;
    EXPECT_EQ(runDiff(cli, args, out, err), 0);
    EXPECT_EQ(cli.last_diff.from_version_id(), "01H-A");
    EXPECT_EQ(cli.last_diff.to_version_id(),   "01H-LIVE");
    EXPECT_NE(out.str().find("diff-body"), std::string::npos);
}

TEST(RunDiff, SingleArgSurfacesGetActiveFailure) {
    ReadFake cli;
    cli.active_status = grpc::Status(grpc::StatusCode::NOT_FOUND,
                                      "no active version yet");

    DiffArgs args;
    args.from_version_id = "01H-A";

    std::ostringstream out, err;
    EXPECT_NE(runDiff(cli, args, out, err), 0);
    EXPECT_NE(err.str().find("NOT_FOUND"), std::string::npos);
}

TEST(RunDiff, SurfacesServerError) {
    ReadFake cli;
    cli.diff_status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                    "unknown from id");
    DiffArgs args;
    args.from_version_id = "01H-A";
    args.to_version_id   = "01H-B";

    std::ostringstream out, err;
    EXPECT_NE(runDiff(cli, args, out, err), 0);
    EXPECT_NE(err.str().find("INVALID_ARGUMENT"), std::string::npos);
}

}  // namespace
}  // namespace aegisgate::cli
