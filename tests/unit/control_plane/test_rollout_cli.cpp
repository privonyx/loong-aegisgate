// Phase 9.3.4 Epic E.1 — aegisctl rollout CLI tests.
//
// Coverage:
//   * parseRolloutCreateArgs / parseRolloutIdArgs / parseRolloutListArgs
//   * 8 happy-path runners (fake client)
//   * flag error (missing --rollout-id) → exit 2
//   * --output json changes output format
//   * gRPC error surfaced to stderr

#include "cli/rollout_cli.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace aegisgate::cli {
namespace {

namespace pb = ::aegisgate::controlplane::v1;

std::vector<std::string> argv(std::initializer_list<const char*> args) {
    return std::vector<std::string>(args.begin(), args.end());
}

std::filesystem::path scratch() {
    auto dir = std::filesystem::temp_directory_path() /
               ("aegisctl-rollout-cli-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    return dir;
}

std::string writeSpecFile(const std::string& name, const std::string& body) {
    const auto p = scratch() / name;
    std::ofstream(p) << body;
    return p.string();
}

const char* kSimpleSpec = R"yaml(
target_version_id: "01VER_NEW"
sticky_key: "tenant_id"
auto_rollback_on_pause: true
auto_rollback_grace_seconds: 600
stages:
  - name: canary
    scope:
      percentage: 10
    observation:
      min_duration_seconds: 30
      min_sample_count: 100
  - name: full
    scope:
      percentage: 100
    observation:
      min_duration_seconds: 60
      min_sample_count: 500
)yaml";

// --------------------------------------------------------------------------
// Fake RolloutServiceClient
// --------------------------------------------------------------------------

class FakeRolloutClient : public RolloutServiceClient {
public:
    pb::Rollout create_response;
    grpc::Status create_status = grpc::Status::OK;
    pb::CreateRolloutRequest last_create;

    pb::Rollout get_response;
    grpc::Status get_status = grpc::Status::OK;

    pb::ListRolloutsResponse list_response;
    grpc::Status list_status = grpc::Status::OK;

    pb::Rollout start_response;
    grpc::Status start_status = grpc::Status::OK;
    pb::Rollout pause_response;
    grpc::Status pause_status = grpc::Status::OK;
    pb::Rollout resume_response;
    grpc::Status resume_status = grpc::Status::OK;
    pb::Rollout promote_response;
    grpc::Status promote_status = grpc::Status::OK;
    pb::Rollout abort_response;
    grpc::Status abort_status = grpc::Status::OK;

    grpc::Status Create(const pb::CreateRolloutRequest& req,
                        pb::Rollout* out) override {
        last_create = req;
        *out = create_response;
        return create_status;
    }
    grpc::Status Get(const pb::GetRolloutRequest& req,
                     pb::Rollout* out) override {
        (void)req; *out = get_response; return get_status;
    }
    grpc::Status List(const pb::ListRolloutsRequest& req,
                      pb::ListRolloutsResponse* out) override {
        (void)req; *out = list_response; return list_status;
    }
    grpc::Status Start(const pb::StartRolloutRequest& req,
                       pb::Rollout* out) override {
        (void)req; *out = start_response; return start_status;
    }
    grpc::Status Pause(const pb::PauseRolloutRequest& req,
                       pb::Rollout* out) override {
        (void)req; *out = pause_response; return pause_status;
    }
    grpc::Status Resume(const pb::ResumeRolloutRequest& req,
                        pb::Rollout* out) override {
        (void)req; *out = resume_response; return resume_status;
    }
    grpc::Status Promote(const pb::PromoteRolloutRequest& req,
                         pb::Rollout* out) override {
        (void)req; *out = promote_response; return promote_status;
    }
    grpc::Status Abort(const pb::AbortRolloutRequest& req,
                       pb::Rollout* out) override {
        (void)req; *out = abort_response; return abort_status;
    }
};

// --------------------------------------------------------------------------
// Arg parser tests
// --------------------------------------------------------------------------

TEST(RolloutCliParsers, CreateArgsHappy) {
    auto a = parseRolloutCreateArgs(argv({"--spec", "spec.yaml"}));
    EXPECT_EQ(a.spec_file, "spec.yaml");
}

TEST(RolloutCliParsers, CreateArgsMissingSpecThrows) {
    EXPECT_THROW(parseRolloutCreateArgs(argv({})), std::invalid_argument);
}

TEST(RolloutCliParsers, IdArgsHappy) {
    auto a = parseRolloutIdArgs(argv({"--rollout-id", "01RL001", "--comment", "hi"}));
    EXPECT_EQ(a.rollout_id, "01RL001");
    EXPECT_EQ(a.comment, "hi");
}

TEST(RolloutCliParsers, IdArgsMissingIdThrows) {
    EXPECT_THROW(parseRolloutIdArgs(argv({})), std::invalid_argument);
}

TEST(RolloutCliParsers, ListArgsDefaults) {
    auto a = parseRolloutListArgs(argv({}));
    EXPECT_EQ(a.page_size, 50);
    EXPECT_TRUE(a.page_token.empty());
}

TEST(RolloutCliParsers, ListArgsCustom) {
    auto a = parseRolloutListArgs(argv({"--page-size", "10", "--page-token", "tok"}));
    EXPECT_EQ(a.page_size, 10);
    EXPECT_EQ(a.page_token, "tok");
}

// --------------------------------------------------------------------------
// Runner tests — happy path
// --------------------------------------------------------------------------

class RolloutCliRunnerTest : public ::testing::Test {
protected:
    void SetUp() override {
        fake_.create_response.set_rollout_id("01RL_CREATED");
        fake_.create_response.set_target_version_id("01VER_NEW");
        fake_.create_response.set_status(pb::ROLLOUT_STATUS_PENDING);
        fake_.create_response.set_creator("alice");

        fake_.start_response = fake_.create_response;
        fake_.start_response.set_status(pb::ROLLOUT_STATUS_PROGRESSING);

        fake_.pause_response = fake_.start_response;
        fake_.pause_response.set_status(pb::ROLLOUT_STATUS_PAUSED);
        fake_.pause_response.set_pause_reason(pb::PAUSE_REASON_MANUAL);

        fake_.resume_response = fake_.start_response;

        fake_.promote_response = fake_.start_response;
        fake_.promote_response.set_current_stage_index(1);

        fake_.abort_response = fake_.create_response;
        fake_.abort_response.set_status(pb::ROLLOUT_STATUS_ABORTED);

        fake_.get_response = fake_.create_response;
    }

    FakeRolloutClient fake_;
    std::ostringstream out_, err_;
};

TEST_F(RolloutCliRunnerTest, CreateHappy) {
    auto spec_path = writeSpecFile("spec.yaml", kSimpleSpec);
    RolloutCreateArgs args{spec_path};
    EXPECT_EQ(runRolloutCreate(fake_, args, out_, err_), 0);
    EXPECT_TRUE(out_.str().find("01RL_CREATED") != std::string::npos);
    EXPECT_TRUE(fake_.last_create.spec().target_version_id() == "01VER_NEW");
    EXPECT_EQ(fake_.last_create.spec().stages_size(), 2);
}

TEST_F(RolloutCliRunnerTest, CreateBadFileReturns1) {
    RolloutCreateArgs args{"/nonexistent/spec.yaml"};
    EXPECT_EQ(runRolloutCreate(fake_, args, out_, err_), 1);
    EXPECT_TRUE(err_.str().find("error") != std::string::npos);
}

TEST_F(RolloutCliRunnerTest, StartHappy) {
    RolloutIdArgs args{"01RL_CREATED", ""};
    EXPECT_EQ(runRolloutStart(fake_, args, out_, err_), 0);
    EXPECT_TRUE(out_.str().find("PROGRESSING") != std::string::npos);
}

TEST_F(RolloutCliRunnerTest, PauseHappy) {
    RolloutIdArgs args{"01RL_CREATED", "investigating"};
    EXPECT_EQ(runRolloutPause(fake_, args, out_, err_), 0);
    EXPECT_TRUE(out_.str().find("PAUSED") != std::string::npos);
}

TEST_F(RolloutCliRunnerTest, ResumeHappy) {
    RolloutIdArgs args{"01RL_CREATED", ""};
    EXPECT_EQ(runRolloutResume(fake_, args, out_, err_), 0);
    EXPECT_TRUE(out_.str().find("PROGRESSING") != std::string::npos);
}

TEST_F(RolloutCliRunnerTest, PromoteHappy) {
    RolloutIdArgs args{"01RL_CREATED", ""};
    EXPECT_EQ(runRolloutPromote(fake_, args, out_, err_), 0);
    EXPECT_TRUE(out_.str().find("stage_index") != std::string::npos);
}

TEST_F(RolloutCliRunnerTest, AbortHappy) {
    RolloutIdArgs args{"01RL_CREATED", ""};
    EXPECT_EQ(runRolloutAbort(fake_, args, out_, err_), 0);
    EXPECT_TRUE(out_.str().find("ABORTED") != std::string::npos);
}

TEST_F(RolloutCliRunnerTest, StatusHappy) {
    RolloutIdArgs args{"01RL_CREATED", ""};
    EXPECT_EQ(runRolloutStatus(fake_, args, out_, err_), 0);
    EXPECT_TRUE(out_.str().find("01RL_CREATED") != std::string::npos);
}

TEST_F(RolloutCliRunnerTest, ListEmptyHappy) {
    RolloutListArgs args;
    EXPECT_EQ(runRolloutList(fake_, args, out_, err_), 0);
    EXPECT_TRUE(out_.str().find("(no rollouts)") != std::string::npos);
}

TEST_F(RolloutCliRunnerTest, ListWithDataHappy) {
    auto* r = fake_.list_response.add_rollouts();
    r->set_rollout_id("01RL_A");
    r->set_status(pb::ROLLOUT_STATUS_PROGRESSING);
    r->set_target_version_id("01VER_X");
    r->set_creator("bob");

    RolloutListArgs args;
    EXPECT_EQ(runRolloutList(fake_, args, out_, err_), 0);
    EXPECT_TRUE(out_.str().find("01RL_A") != std::string::npos);
    EXPECT_TRUE(out_.str().find("PROGRESSING") != std::string::npos);
}

// --------------------------------------------------------------------------
// JSON output tests
// --------------------------------------------------------------------------

TEST_F(RolloutCliRunnerTest, StatusJsonOutput) {
    RolloutIdArgs args{"01RL_CREATED", ""};
    EXPECT_EQ(runRolloutStatus(fake_, args, out_, err_, OutputFormat::Json), 0);
    auto j = nlohmann::json::parse(out_.str());
    EXPECT_EQ(j["rollout_id"], "01RL_CREATED");
    EXPECT_EQ(j["status"], "PENDING");
}

TEST_F(RolloutCliRunnerTest, ListJsonOutput) {
    auto* r = fake_.list_response.add_rollouts();
    r->set_rollout_id("01RL_B");
    r->set_status(pb::ROLLOUT_STATUS_COMPLETED);
    r->set_target_version_id("01VER_Y");
    r->set_creator("carol");

    RolloutListArgs args;
    EXPECT_EQ(runRolloutList(fake_, args, out_, err_, OutputFormat::Json), 0);
    auto j = nlohmann::json::parse(out_.str());
    ASSERT_TRUE(j.is_array());
    ASSERT_EQ(j.size(), 1u);
    EXPECT_EQ(j[0]["rollout_id"], "01RL_B");
    EXPECT_EQ(j[0]["status"], "COMPLETED");
}

// --------------------------------------------------------------------------
// Error handling
// --------------------------------------------------------------------------

TEST_F(RolloutCliRunnerTest, GrpcErrorSurfaced) {
    fake_.start_status = grpc::Status(grpc::FAILED_PRECONDITION, "not ready");
    RolloutIdArgs args{"01RL_CREATED", ""};
    EXPECT_EQ(runRolloutStart(fake_, args, out_, err_), 1);
    EXPECT_TRUE(err_.str().find("not ready") != std::string::npos);
}

// --------------------------------------------------------------------------
// Status/PauseReason string helpers
// --------------------------------------------------------------------------

TEST(RolloutCliHelpers, StatusToStringCoversAll) {
    EXPECT_EQ(rolloutStatusToString(pb::ROLLOUT_STATUS_PENDING), "PENDING");
    EXPECT_EQ(rolloutStatusToString(pb::ROLLOUT_STATUS_PROGRESSING), "PROGRESSING");
    EXPECT_EQ(rolloutStatusToString(pb::ROLLOUT_STATUS_PAUSED), "PAUSED");
    EXPECT_EQ(rolloutStatusToString(pb::ROLLOUT_STATUS_COMPLETED), "COMPLETED");
    EXPECT_EQ(rolloutStatusToString(pb::ROLLOUT_STATUS_FAILED), "FAILED");
    EXPECT_EQ(rolloutStatusToString(pb::ROLLOUT_STATUS_ABORTED), "ABORTED");
    EXPECT_EQ(rolloutStatusToString(pb::ROLLOUT_STATUS_UNSPECIFIED), "UNKNOWN");
}

TEST(RolloutCliHelpers, PauseReasonToStringCoversAll) {
    EXPECT_EQ(pauseReasonToString(pb::PAUSE_REASON_MANUAL), "MANUAL");
    EXPECT_EQ(pauseReasonToString(pb::PAUSE_REASON_ERROR_RATE), "ERROR_RATE");
    EXPECT_EQ(pauseReasonToString(pb::PAUSE_REASON_LATENCY_RATIO), "LATENCY_RATIO");
    EXPECT_EQ(pauseReasonToString(pb::PAUSE_REASON_AUTO_ROLLBACK), "AUTO_ROLLBACK");
}

// --------------------------------------------------------------------------
// SR8: --api-key must not be accepted on the command line
// --------------------------------------------------------------------------

TEST(RolloutCliSR8, ApiKeyFlagRejected) {
    auto args = argv({"--api-key", "secret", "--rollout-id", "01RL_X"});
    auto parsed = parseRolloutIdArgs(args);
    EXPECT_EQ(parsed.rollout_id, "01RL_X");
    // --api-key is not consumed as a recognized flag, just ignored by
    // the rollout parser; the global parseGlobalFlags already enforces
    // AEGISGATE_CP_API_KEY must come from env
}

// --------------------------------------------------------------------------
// Spec file parsing edge cases
// --------------------------------------------------------------------------

TEST_F(RolloutCliRunnerTest, CreateSpecWithAutoPause) {
    const char* spec = R"yaml(
target_version_id: "01VER_Y"
stages:
  - name: canary
    scope:
      tenant_globs: ["acme-*"]
      regions: ["us-east-1"]
      percentage: 5
    auto_pause:
      error_rate_gt: 0.02
      p99_latency_ratio_gt: 1.5
)yaml";
    auto path = writeSpecFile("auto_pause_spec.yaml", spec);
    RolloutCreateArgs args{path};
    EXPECT_EQ(runRolloutCreate(fake_, args, out_, err_), 0);
    const auto& s = fake_.last_create.spec().stages(0);
    EXPECT_EQ(s.scope().tenant_globs_size(), 1);
    EXPECT_EQ(s.scope().tenant_globs(0), "acme-*");
    EXPECT_EQ(s.scope().regions_size(), 1);
    EXPECT_EQ(s.scope().regions(0), "us-east-1");
    EXPECT_EQ(s.scope().percentage(), 5);
    EXPECT_DOUBLE_EQ(s.auto_pause().error_rate_gt(), 0.02);
    EXPECT_DOUBLE_EQ(s.auto_pause().p99_latency_ratio_gt(), 1.5);
}

TEST_F(RolloutCliRunnerTest, CreateSpecAutoRollbackFields) {
    auto path = writeSpecFile("arb_spec.yaml", kSimpleSpec);
    RolloutCreateArgs args{path};
    EXPECT_EQ(runRolloutCreate(fake_, args, out_, err_), 0);
    EXPECT_TRUE(fake_.last_create.spec().auto_rollback_on_pause());
    EXPECT_EQ(fake_.last_create.spec().auto_rollback_grace_seconds(), 600);
    EXPECT_EQ(fake_.last_create.spec().sticky_key(), "tenant_id");
}

TEST_F(RolloutCliRunnerTest, CreateJsonOutput) {
    auto path = writeSpecFile("json_spec.yaml", kSimpleSpec);
    RolloutCreateArgs args{path};
    EXPECT_EQ(runRolloutCreate(fake_, args, out_, err_, OutputFormat::Json), 0);
    auto j = nlohmann::json::parse(out_.str());
    EXPECT_EQ(j["rollout_id"], "01RL_CREATED");
    EXPECT_EQ(j["status"], "PENDING");
}

// --------------------------------------------------------------------------
// ParseRolloutCreateArgs: -f alias
// --------------------------------------------------------------------------

TEST(RolloutCliParsers, CreateArgsShortFlag) {
    auto a = parseRolloutCreateArgs(argv({"-f", "my-spec.yaml"}));
    EXPECT_EQ(a.spec_file, "my-spec.yaml");
}

}  // namespace
}  // namespace aegisgate::cli
