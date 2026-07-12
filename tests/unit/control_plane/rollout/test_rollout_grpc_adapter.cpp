// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic C.1.
//
// Tests for RolloutService gRPC adapter: pure converters + RolloutServiceImpl.
//
// Coverage:
//   * rolloutStatusToProto / rolloutStatusFromProto roundtrip (incl. UNSPECIFIED)
//   * pauseReasonToProto / pauseReasonFromProto roundtrip
//   * rolloutToProto / rolloutFromProto field preservation
//   * rolloutSpecToProto / rolloutSpecFromProto roundtrip (multi-stage)
//   * 8 RPC handlers × (happy + UNAUTHENTICATED) + selected error paths

#include "common/clock.h"
#include "control_plane/config_service_core.h"
#include "control_plane/grpc/rollout_service_grpc_adapter.h"
#include "control_plane/rollout/rollout_audit_bridge.h"
#include "control_plane/rollout/rollout_controller.h"
#include "control_plane/rollout/rollout_metrics_provider.h"
#include "guardrail/audit.h"
#include "storage/memory_persistent_store.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace aegisgate {
namespace pb = aegisgate::controlplane::v1;
using control_plane::grpc_adapter::RolloutServiceImpl;
using control_plane::grpc_adapter::UserExtractor;

namespace {

// ---------------------------------------------------------------------------
// Converter tests (no gRPC machinery)
// ---------------------------------------------------------------------------

TEST(RolloutGrpcConverters, StatusRoundtripSixValues) {
    using control_plane::grpc_adapter::rolloutStatusToProto;
    using control_plane::grpc_adapter::rolloutStatusFromProto;

    struct Pair { RolloutStatus cpp; pb::RolloutStatus proto; };
    Pair pairs[] = {
        {RolloutStatus::PENDING,     pb::ROLLOUT_STATUS_PENDING},
        {RolloutStatus::PROGRESSING, pb::ROLLOUT_STATUS_PROGRESSING},
        {RolloutStatus::PAUSED,      pb::ROLLOUT_STATUS_PAUSED},
        {RolloutStatus::COMPLETED,   pb::ROLLOUT_STATUS_COMPLETED},
        {RolloutStatus::FAILED,      pb::ROLLOUT_STATUS_FAILED},
        {RolloutStatus::ABORTED,     pb::ROLLOUT_STATUS_ABORTED},
    };
    for (const auto& p : pairs) {
        EXPECT_EQ(rolloutStatusToProto(p.cpp), p.proto);
        ASSERT_TRUE(rolloutStatusFromProto(p.proto).has_value());
        EXPECT_EQ(*rolloutStatusFromProto(p.proto), p.cpp);
    }
}

TEST(RolloutGrpcConverters, StatusFromProtoUnspecifiedReturnsNullopt) {
    using control_plane::grpc_adapter::rolloutStatusFromProto;
    EXPECT_FALSE(rolloutStatusFromProto(pb::ROLLOUT_STATUS_UNSPECIFIED).has_value());
}

TEST(RolloutGrpcConverters, PauseReasonRoundtripFiveValues) {
    using control_plane::grpc_adapter::pauseReasonToProto;
    using control_plane::grpc_adapter::pauseReasonFromProto;

    struct Pair { PauseReason cpp; pb::PauseReason proto; };
    Pair pairs[] = {
        {PauseReason::UNSPECIFIED,   pb::PAUSE_REASON_UNSPECIFIED},
        {PauseReason::MANUAL,        pb::PAUSE_REASON_MANUAL},
        {PauseReason::ERROR_RATE,    pb::PAUSE_REASON_ERROR_RATE},
        {PauseReason::LATENCY_RATIO, pb::PAUSE_REASON_LATENCY_RATIO},
        {PauseReason::AUTO_ROLLBACK, pb::PAUSE_REASON_AUTO_ROLLBACK},
    };
    for (const auto& p : pairs) {
        EXPECT_EQ(pauseReasonToProto(p.cpp), p.proto);
        EXPECT_EQ(*pauseReasonFromProto(p.proto), p.cpp);
    }
}

TEST(RolloutGrpcConverters, SpecRoundtripPreservesMultiStage) {
    using control_plane::grpc_adapter::rolloutSpecToProto;
    using control_plane::grpc_adapter::rolloutSpecFromProto;

    RolloutSpec spec;
    spec.target_version_id = "01VER00000000000000000NEW";
    spec.sticky_key = "user_id";
    spec.auto_rollback_on_pause = true;
    spec.auto_rollback_grace_seconds = 600;
    spec.creator_comment = "testing round trip";

    RolloutStageRecord s1;
    s1.name = "canary";
    s1.scope.tenant_globs = {"tenant-a*", "tenant-b"};
    s1.scope.regions = {"us-east-1", "eu-west-1"};
    s1.scope.percentage = 10;
    s1.observation.min_duration_seconds = 30;
    s1.observation.min_sample_count = 100;
    s1.auto_pause.error_rate_gt = 0.02;
    s1.auto_pause.p99_latency_ratio_gt = 1.5;
    s1.auto_pause.absolute_error_rate_gt = 0.05;
    s1.auto_pause.absolute_p99_latency_ms_gt = 500.0;
    spec.stages.push_back(s1);

    RolloutStageRecord s2;
    s2.name = "full";
    s2.scope.percentage = 100;
    s2.observation.min_duration_seconds = 60;
    s2.observation.min_sample_count = 500;
    spec.stages.push_back(s2);

    auto pb_spec = rolloutSpecToProto(spec);
    auto parsed = rolloutSpecFromProto(pb_spec);

    EXPECT_EQ(parsed.target_version_id, spec.target_version_id);
    EXPECT_EQ(parsed.sticky_key, "user_id");
    EXPECT_EQ(parsed.auto_rollback_on_pause, true);
    EXPECT_EQ(parsed.auto_rollback_grace_seconds, 600);
    EXPECT_EQ(parsed.creator_comment, "testing round trip");

    ASSERT_EQ(parsed.stages.size(), 2u);
    EXPECT_EQ(parsed.stages[0].name, "canary");
    EXPECT_EQ(parsed.stages[0].scope.tenant_globs.size(), 2u);
    EXPECT_EQ(parsed.stages[0].scope.tenant_globs[0], "tenant-a*");
    EXPECT_EQ(parsed.stages[0].scope.regions.size(), 2u);
    EXPECT_EQ(parsed.stages[0].scope.percentage, 10);
    EXPECT_EQ(parsed.stages[0].observation.min_duration_seconds, 30);
    EXPECT_EQ(parsed.stages[0].observation.min_sample_count, 100);
    EXPECT_DOUBLE_EQ(parsed.stages[0].auto_pause.error_rate_gt, 0.02);
    EXPECT_DOUBLE_EQ(parsed.stages[0].auto_pause.p99_latency_ratio_gt, 1.5);
    EXPECT_DOUBLE_EQ(parsed.stages[0].auto_pause.absolute_error_rate_gt, 0.05);
    EXPECT_DOUBLE_EQ(parsed.stages[0].auto_pause.absolute_p99_latency_ms_gt, 500.0);

    EXPECT_EQ(parsed.stages[1].name, "full");
    EXPECT_EQ(parsed.stages[1].scope.percentage, 100);
}

TEST(RolloutGrpcConverters, RolloutRecordRoundtripPreservesAllFields) {
    using control_plane::grpc_adapter::rolloutToProto;
    using control_plane::grpc_adapter::rolloutFromProto;

    RolloutRecord rec;
    rec.rollout_id = "01RL0000000000000000000001";
    rec.target_version_id = "01VER00000000000000000NEW";
    rec.previous_active_version_id = "01VER00000000000000000OLD";
    rec.status = RolloutStatus::PAUSED;
    rec.current_stage_index = 1;
    rec.started_at = 1'700'000'000'000LL;
    rec.stage_started_at = 1'700'000'100'000LL;
    rec.paused_at = 1'700'000'200'000LL;
    rec.pause_reason = PauseReason::ERROR_RATE;
    rec.pause_detail = "error_rate 0.10 > 0.05";
    rec.creator = "alice";
    rec.last_actor = "bob";
    rec.completed_at = 0;
    rec.chain_hash = "ch-deadbeef";
    rec.spec.target_version_id = rec.target_version_id;
    RolloutStageRecord stg;
    stg.name = "canary";
    stg.scope.percentage = 10;
    rec.spec.stages.push_back(stg);

    auto msg = rolloutToProto(rec);
    auto parsed = rolloutFromProto(msg);

    EXPECT_EQ(parsed.rollout_id, rec.rollout_id);
    EXPECT_EQ(parsed.target_version_id, rec.target_version_id);
    EXPECT_EQ(parsed.previous_active_version_id, rec.previous_active_version_id);
    EXPECT_EQ(parsed.status, RolloutStatus::PAUSED);
    EXPECT_EQ(parsed.current_stage_index, 1);
    EXPECT_EQ(parsed.started_at, rec.started_at);
    EXPECT_EQ(parsed.stage_started_at, rec.stage_started_at);
    EXPECT_EQ(parsed.paused_at, rec.paused_at);
    EXPECT_EQ(parsed.pause_reason, PauseReason::ERROR_RATE);
    EXPECT_EQ(parsed.pause_detail, "error_rate 0.10 > 0.05");
    EXPECT_EQ(parsed.creator, "alice");
    EXPECT_EQ(parsed.last_actor, "bob");
    EXPECT_EQ(parsed.completed_at, 0);
    EXPECT_EQ(parsed.chain_hash, "ch-deadbeef");
    ASSERT_EQ(parsed.spec.stages.size(), 1u);
    EXPECT_EQ(parsed.spec.stages[0].name, "canary");
}

// ---------------------------------------------------------------------------
// RolloutServiceImpl integration — real ConfigServiceCore + RolloutController
// ---------------------------------------------------------------------------

class FakeMetricsProvider : public RolloutMetricsProvider {
public:
    VersionMetrics forVersion(std::string_view, std::chrono::seconds) override {
        return {};
    }
};

class RolloutServiceImplTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryPersistentStore>();
        ASSERT_TRUE(store_->initialize());
        audit_ = std::make_unique<AuditLogger>();
        audit_->setPersistentStore(store_.get());
        clock_ = std::make_unique<common::FakeClock>(1'700'000'000'000LL);

        ConfigServiceCore::Deps cdeps;
        cdeps.store = store_.get();
        cdeps.audit = audit_.get();
        cdeps.clock = [this]() { return clock_->wallClockMillis(); };
        cdeps.validator = [](const std::string&) {
            return std::vector<Config::ValidationIssue>{};
        };
        config_core_ = std::make_unique<ConfigServiceCore>(std::move(cdeps));

        bridge_ = std::make_unique<RolloutAuditBridge>(audit_.get());
        metrics_ = std::make_unique<FakeMetricsProvider>();

        RolloutController::Deps d;
        d.store = store_.get();
        d.config_core = config_core_.get();
        d.metrics = metrics_.get();
        d.audit = bridge_.get();
        d.clock = clock_.get();
        ctrl_ = std::make_unique<RolloutController>(std::move(d));

        authed_extractor_ = [](grpc::ServerContext*) { return std::string("admin"); };
        unauthed_extractor_ = [](grpc::ServerContext*) { return std::string(); };

        svc_ = std::make_unique<RolloutServiceImpl>(ctrl_.get(), authed_extractor_);
        svc_unauthed_ = std::make_unique<RolloutServiceImpl>(ctrl_.get(), unauthed_extractor_);
    }

    std::string createApprovedVersion(const std::string& yaml = "v: 1\n") {
        auto sub = config_core_->submit(yaml, "alice", "", false);
        EXPECT_EQ(sub.error_code, "");
        auto app = config_core_->approve(sub.record.version_id, "bob", "");
        EXPECT_EQ(app.error_code, "");
        return sub.record.version_id;
    }

    pb::RolloutSpec makeProtoSpec(const std::string& target) {
        pb::RolloutSpec s;
        s.set_target_version_id(target);
        auto* stg = s.add_stages();
        stg->set_name("canary");
        stg->mutable_scope()->set_percentage(10);
        stg->mutable_observation()->set_min_duration_seconds(30);
        stg->mutable_observation()->set_min_sample_count(10);
        auto* stg2 = s.add_stages();
        stg2->set_name("full");
        stg2->mutable_scope()->set_percentage(100);
        stg2->mutable_observation()->set_min_duration_seconds(60);
        stg2->mutable_observation()->set_min_sample_count(50);
        return s;
    }

    std::unique_ptr<MemoryPersistentStore> store_;
    std::unique_ptr<AuditLogger>           audit_;
    std::unique_ptr<common::FakeClock>     clock_;
    std::unique_ptr<ConfigServiceCore>     config_core_;
    std::unique_ptr<RolloutAuditBridge>    bridge_;
    std::unique_ptr<FakeMetricsProvider>   metrics_;
    std::unique_ptr<RolloutController>     ctrl_;
    UserExtractor authed_extractor_;
    UserExtractor unauthed_extractor_;
    std::unique_ptr<RolloutServiceImpl>    svc_;
    std::unique_ptr<RolloutServiceImpl>    svc_unauthed_;
};

// --- CreateRollout ---------------------------------------------------------

TEST_F(RolloutServiceImplTest, CreateRolloutHappyPath) {
    auto vid = createApprovedVersion();
    pb::CreateRolloutRequest req;
    *req.mutable_spec() = makeProtoSpec(vid);
    pb::Rollout resp;
    auto st = svc_->CreateRollout(nullptr, &req, &resp);
    EXPECT_TRUE(st.ok()) << st.error_message();
    EXPECT_FALSE(resp.rollout_id().empty());
    EXPECT_EQ(resp.status(), pb::ROLLOUT_STATUS_PENDING);
    EXPECT_EQ(resp.target_version_id(), vid);
}

TEST_F(RolloutServiceImplTest, CreateRolloutUnauthenticated) {
    pb::CreateRolloutRequest req;
    pb::Rollout resp;
    auto st = svc_unauthed_->CreateRollout(nullptr, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::UNAUTHENTICATED);
}

TEST_F(RolloutServiceImplTest, CreateRolloutInvalidArgument) {
    pb::CreateRolloutRequest req;
    pb::Rollout resp;
    auto st = svc_->CreateRollout(nullptr, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::INVALID_ARGUMENT);
}

// --- GetRollout ------------------------------------------------------------

TEST_F(RolloutServiceImplTest, GetRolloutHappyPath) {
    auto vid = createApprovedVersion();
    pb::CreateRolloutRequest creq;
    *creq.mutable_spec() = makeProtoSpec(vid);
    pb::Rollout cresp;
    ASSERT_TRUE(svc_->CreateRollout(nullptr, &creq, &cresp).ok());

    pb::GetRolloutRequest greq;
    greq.set_rollout_id(cresp.rollout_id());
    pb::Rollout gresp;
    auto st = svc_->GetRollout(nullptr, &greq, &gresp);
    EXPECT_TRUE(st.ok());
    EXPECT_EQ(gresp.rollout_id(), cresp.rollout_id());
}

TEST_F(RolloutServiceImplTest, GetRolloutNotFound) {
    pb::GetRolloutRequest req;
    req.set_rollout_id("01DOESNOTEXIST000000000000");
    pb::Rollout resp;
    auto st = svc_->GetRollout(nullptr, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::NOT_FOUND);
}

TEST_F(RolloutServiceImplTest, GetRolloutUnauthenticated) {
    pb::GetRolloutRequest req;
    req.set_rollout_id("01RL0000000000000000000001");
    pb::Rollout resp;
    auto st = svc_unauthed_->GetRollout(nullptr, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::UNAUTHENTICATED);
}

TEST_F(RolloutServiceImplTest, GetRolloutEmptyIdInvalidArgument) {
    pb::GetRolloutRequest req;
    pb::Rollout resp;
    auto st = svc_->GetRollout(nullptr, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::INVALID_ARGUMENT);
}

// --- ListRollouts ----------------------------------------------------------

TEST_F(RolloutServiceImplTest, ListRolloutsEmpty) {
    pb::ListRolloutsRequest req;
    pb::ListRolloutsResponse resp;
    auto st = svc_->ListRollouts(nullptr, &req, &resp);
    EXPECT_TRUE(st.ok());
    EXPECT_EQ(resp.rollouts_size(), 0);
}

TEST_F(RolloutServiceImplTest, ListRolloutsUnauthenticated) {
    pb::ListRolloutsRequest req;
    pb::ListRolloutsResponse resp;
    auto st = svc_unauthed_->ListRollouts(nullptr, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::UNAUTHENTICATED);
}

TEST_F(RolloutServiceImplTest, ListRolloutsReturnsCreated) {
    auto vid = createApprovedVersion();
    pb::CreateRolloutRequest creq;
    *creq.mutable_spec() = makeProtoSpec(vid);
    pb::Rollout cresp;
    ASSERT_TRUE(svc_->CreateRollout(nullptr, &creq, &cresp).ok());

    pb::ListRolloutsRequest lreq;
    pb::ListRolloutsResponse lresp;
    auto st = svc_->ListRollouts(nullptr, &lreq, &lresp);
    EXPECT_TRUE(st.ok());
    EXPECT_GE(lresp.rollouts_size(), 1);
}

// --- StartRollout ----------------------------------------------------------

TEST_F(RolloutServiceImplTest, StartRolloutHappyPath) {
    auto vid = createApprovedVersion();
    pb::CreateRolloutRequest creq;
    *creq.mutable_spec() = makeProtoSpec(vid);
    pb::Rollout cresp;
    ASSERT_TRUE(svc_->CreateRollout(nullptr, &creq, &cresp).ok());

    pb::StartRolloutRequest sreq;
    sreq.set_rollout_id(cresp.rollout_id());
    pb::Rollout sresp;
    auto st = svc_->StartRollout(nullptr, &sreq, &sresp);
    EXPECT_TRUE(st.ok()) << st.error_message();
    EXPECT_EQ(sresp.status(), pb::ROLLOUT_STATUS_PROGRESSING);
}

TEST_F(RolloutServiceImplTest, StartRolloutUnauthenticated) {
    pb::StartRolloutRequest req;
    req.set_rollout_id("01RL0000000000000000000001");
    pb::Rollout resp;
    auto st = svc_unauthed_->StartRollout(nullptr, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::UNAUTHENTICATED);
}

TEST_F(RolloutServiceImplTest, StartRolloutFailedPrecondition) {
    auto vid = createApprovedVersion();
    pb::CreateRolloutRequest creq;
    *creq.mutable_spec() = makeProtoSpec(vid);
    pb::Rollout cresp;
    ASSERT_TRUE(svc_->CreateRollout(nullptr, &creq, &cresp).ok());

    pb::StartRolloutRequest sreq;
    sreq.set_rollout_id(cresp.rollout_id());
    pb::Rollout sresp;
    ASSERT_TRUE(svc_->StartRollout(nullptr, &sreq, &sresp).ok());
    auto again = svc_->StartRollout(nullptr, &sreq, &sresp);
    EXPECT_EQ(again.error_code(), grpc::FAILED_PRECONDITION);
}

// --- PauseRollout ----------------------------------------------------------

TEST_F(RolloutServiceImplTest, PauseRolloutHappyPath) {
    auto vid = createApprovedVersion();
    pb::CreateRolloutRequest creq;
    *creq.mutable_spec() = makeProtoSpec(vid);
    pb::Rollout cresp;
    ASSERT_TRUE(svc_->CreateRollout(nullptr, &creq, &cresp).ok());
    pb::StartRolloutRequest sreq;
    sreq.set_rollout_id(cresp.rollout_id());
    pb::Rollout sresp;
    ASSERT_TRUE(svc_->StartRollout(nullptr, &sreq, &sresp).ok());

    pb::PauseRolloutRequest preq;
    preq.set_rollout_id(cresp.rollout_id());
    preq.set_comment("investigating");
    pb::Rollout presp;
    auto st = svc_->PauseRollout(nullptr, &preq, &presp);
    EXPECT_TRUE(st.ok());
    EXPECT_EQ(presp.status(), pb::ROLLOUT_STATUS_PAUSED);
    EXPECT_EQ(presp.pause_reason(), pb::PAUSE_REASON_MANUAL);
}

TEST_F(RolloutServiceImplTest, PauseRolloutUnauthenticated) {
    pb::PauseRolloutRequest req;
    req.set_rollout_id("01RL0000000000000000000001");
    pb::Rollout resp;
    auto st = svc_unauthed_->PauseRollout(nullptr, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::UNAUTHENTICATED);
}

// --- ResumeRollout ---------------------------------------------------------

TEST_F(RolloutServiceImplTest, ResumeRolloutHappyPath) {
    auto vid = createApprovedVersion();
    pb::CreateRolloutRequest creq;
    *creq.mutable_spec() = makeProtoSpec(vid);
    pb::Rollout cresp;
    ASSERT_TRUE(svc_->CreateRollout(nullptr, &creq, &cresp).ok());
    pb::StartRolloutRequest sreq;
    sreq.set_rollout_id(cresp.rollout_id());
    pb::Rollout t;
    ASSERT_TRUE(svc_->StartRollout(nullptr, &sreq, &t).ok());
    pb::PauseRolloutRequest preq;
    preq.set_rollout_id(cresp.rollout_id());
    ASSERT_TRUE(svc_->PauseRollout(nullptr, &preq, &t).ok());

    pb::ResumeRolloutRequest rreq;
    rreq.set_rollout_id(cresp.rollout_id());
    pb::Rollout rresp;
    auto st = svc_->ResumeRollout(nullptr, &rreq, &rresp);
    EXPECT_TRUE(st.ok());
    EXPECT_EQ(rresp.status(), pb::ROLLOUT_STATUS_PROGRESSING);
}

TEST_F(RolloutServiceImplTest, ResumeRolloutUnauthenticated) {
    pb::ResumeRolloutRequest req;
    req.set_rollout_id("x");
    pb::Rollout resp;
    EXPECT_EQ(svc_unauthed_->ResumeRollout(nullptr, &req, &resp).error_code(),
              grpc::UNAUTHENTICATED);
}

// --- PromoteRollout --------------------------------------------------------

TEST_F(RolloutServiceImplTest, PromoteRolloutAdvancesStage) {
    auto vid = createApprovedVersion();
    pb::CreateRolloutRequest creq;
    *creq.mutable_spec() = makeProtoSpec(vid);
    pb::Rollout cresp;
    ASSERT_TRUE(svc_->CreateRollout(nullptr, &creq, &cresp).ok());
    pb::StartRolloutRequest sreq;
    sreq.set_rollout_id(cresp.rollout_id());
    pb::Rollout t;
    ASSERT_TRUE(svc_->StartRollout(nullptr, &sreq, &t).ok());

    pb::PromoteRolloutRequest preq;
    preq.set_rollout_id(cresp.rollout_id());
    pb::Rollout presp;
    auto st = svc_->PromoteRollout(nullptr, &preq, &presp);
    EXPECT_TRUE(st.ok());
    EXPECT_EQ(presp.current_stage_index(), 1);
}

TEST_F(RolloutServiceImplTest, PromoteRolloutUnauthenticated) {
    pb::PromoteRolloutRequest req;
    req.set_rollout_id("x");
    pb::Rollout resp;
    EXPECT_EQ(svc_unauthed_->PromoteRollout(nullptr, &req, &resp).error_code(),
              grpc::UNAUTHENTICATED);
}

// --- AbortRollout ----------------------------------------------------------

TEST_F(RolloutServiceImplTest, AbortRolloutHappyPath) {
    auto vid = createApprovedVersion();
    pb::CreateRolloutRequest creq;
    *creq.mutable_spec() = makeProtoSpec(vid);
    pb::Rollout cresp;
    ASSERT_TRUE(svc_->CreateRollout(nullptr, &creq, &cresp).ok());

    pb::AbortRolloutRequest areq;
    areq.set_rollout_id(cresp.rollout_id());
    pb::Rollout aresp;
    auto st = svc_->AbortRollout(nullptr, &areq, &aresp);
    EXPECT_TRUE(st.ok());
    EXPECT_EQ(aresp.status(), pb::ROLLOUT_STATUS_ABORTED);
}

TEST_F(RolloutServiceImplTest, AbortRolloutUnauthenticated) {
    pb::AbortRolloutRequest req;
    req.set_rollout_id("x");
    pb::Rollout resp;
    EXPECT_EQ(svc_unauthed_->AbortRollout(nullptr, &req, &resp).error_code(),
              grpc::UNAUTHENTICATED);
}

TEST_F(RolloutServiceImplTest, AbortTerminalRejected) {
    auto vid = createApprovedVersion();
    pb::CreateRolloutRequest creq;
    *creq.mutable_spec() = makeProtoSpec(vid);
    pb::Rollout cresp;
    ASSERT_TRUE(svc_->CreateRollout(nullptr, &creq, &cresp).ok());
    pb::AbortRolloutRequest areq;
    areq.set_rollout_id(cresp.rollout_id());
    pb::Rollout t;
    ASSERT_TRUE(svc_->AbortRollout(nullptr, &areq, &t).ok());
    auto again = svc_->AbortRollout(nullptr, &areq, &t);
    EXPECT_EQ(again.error_code(), grpc::FAILED_PRECONDITION);
}

}  // namespace
}  // namespace aegisgate
