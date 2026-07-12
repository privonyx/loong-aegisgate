// Phase 9.3 Epic 1 Task 1.2 + 1.3 — control_plane.proto generated code
// roundtrip smoke test. Ensures protoc + grpc_cpp_plugin produced code that
// links, serializes, and exposes the gRPC service stub.

#include "control_plane/v1/control_plane.grpc.pb.h"
#include "control_plane/v1/control_plane.pb.h"
#include <gtest/gtest.h>

namespace {

using aegisgate::controlplane::v1::ConfigService;
using aegisgate::controlplane::v1::ConfigStatus;
using aegisgate::controlplane::v1::ConfigVersion;
using aegisgate::controlplane::v1::ListVersionsRequest;
using aegisgate::controlplane::v1::ListVersionsResponse;

// Phase 9.3.4 (TASK-20260422-01) — RolloutService contract types.
using aegisgate::controlplane::v1::AbortRolloutRequest;
using aegisgate::controlplane::v1::AutoPausePolicy;
using aegisgate::controlplane::v1::CreateRolloutRequest;
using aegisgate::controlplane::v1::GetRolloutRequest;
using aegisgate::controlplane::v1::ListRolloutsRequest;
using aegisgate::controlplane::v1::ListRolloutsResponse;
using aegisgate::controlplane::v1::ObservationPolicy;
using aegisgate::controlplane::v1::PauseReason;
using aegisgate::controlplane::v1::PauseRolloutRequest;
using aegisgate::controlplane::v1::PromoteRolloutRequest;
using aegisgate::controlplane::v1::ResumeRolloutRequest;
using aegisgate::controlplane::v1::Rollout;
using aegisgate::controlplane::v1::RolloutService;
using aegisgate::controlplane::v1::RolloutSpec;
using aegisgate::controlplane::v1::RolloutStage;
using aegisgate::controlplane::v1::RolloutStatus;
using aegisgate::controlplane::v1::ScopeSelector;
using aegisgate::controlplane::v1::StartRolloutRequest;

TEST(ControlPlaneProto, ConfigVersionFieldsRoundtrip) {
    ConfigVersion v;
    v.set_version_id("01J8A00000000000000000001");
    v.set_content_sha256("abc123");
    v.set_yaml_content("server:\n  port: 8080\n");
    v.set_size_bytes(42);
    v.set_status(ConfigStatus::CONFIG_STATUS_PENDING);
    v.set_submitter("alice");
    v.set_submitter_comment("initial");
    v.set_submitted_at(1713600000000LL);
    v.set_chain_hash("deadbeef");

    std::string bytes;
    ASSERT_TRUE(v.SerializeToString(&bytes));
    EXPECT_FALSE(bytes.empty());

    ConfigVersion parsed;
    ASSERT_TRUE(parsed.ParseFromString(bytes));
    EXPECT_EQ(parsed.version_id(), "01J8A00000000000000000001");
    EXPECT_EQ(parsed.content_sha256(), "abc123");
    EXPECT_EQ(parsed.yaml_content(), "server:\n  port: 8080\n");
    EXPECT_EQ(parsed.size_bytes(), 42);
    EXPECT_EQ(parsed.status(), ConfigStatus::CONFIG_STATUS_PENDING);
    EXPECT_EQ(parsed.submitter(), "alice");
    EXPECT_EQ(parsed.submitter_comment(), "initial");
    EXPECT_EQ(parsed.submitted_at(), 1713600000000LL);
    EXPECT_EQ(parsed.chain_hash(), "deadbeef");
}

TEST(ControlPlaneProto, ListVersionsResponseRepeatedRoundtrip) {
    ListVersionsResponse resp;
    for (int i = 0; i < 3; ++i) {
        auto* v = resp.add_versions();
        v->set_version_id("01J8A0000000000000000000" + std::to_string(i));
        v->set_submitted_at(1000 + i);
        v->set_status(ConfigStatus::CONFIG_STATUS_APPROVED);
    }
    resp.set_next_page_token("cursor-" + std::string("xyz"));

    std::string bytes;
    ASSERT_TRUE(resp.SerializeToString(&bytes));

    ListVersionsResponse parsed;
    ASSERT_TRUE(parsed.ParseFromString(bytes));
    ASSERT_EQ(parsed.versions_size(), 3);
    EXPECT_EQ(parsed.versions(0).version_id(), "01J8A00000000000000000000");
    EXPECT_EQ(parsed.versions(2).submitted_at(), 1002);
    EXPECT_EQ(parsed.next_page_token(), "cursor-xyz");
}

TEST(ControlPlaneProto, EnumValuesAreStable) {
    // SR12 / audit: wire values must not shift silently.
    EXPECT_EQ(ConfigStatus::CONFIG_STATUS_UNSPECIFIED, 0);
    EXPECT_EQ(ConfigStatus::CONFIG_STATUS_PENDING,     1);
    EXPECT_EQ(ConfigStatus::CONFIG_STATUS_APPROVED,    2);
    EXPECT_EQ(ConfigStatus::CONFIG_STATUS_REJECTED,    3);
    EXPECT_EQ(ConfigStatus::CONFIG_STATUS_ACTIVE,      4);
    EXPECT_EQ(ConfigStatus::CONFIG_STATUS_SUPERSEDED,  5);
}

TEST(ControlPlaneGrpc, ConfigServiceAsyncServiceInstantiable) {
    // Smoke: service stub class must be defined and instantiable so the
    // server skeleton in Task 5.x can attach. If protoc-gen-grpc_cpp ran
    // successfully this compiles and links; otherwise this whole TU fails.
    ConfigService::AsyncService svc;
    (void)svc;
    SUCCEED();
}

TEST(ControlPlaneGrpc, ListVersionsRequestUnchanged) {
    ListVersionsRequest req;
    req.set_page_size(25);
    req.set_page_token("tok");
    req.add_statuses(ConfigStatus::CONFIG_STATUS_PENDING);
    std::string bytes;
    ASSERT_TRUE(req.SerializeToString(&bytes));
    ListVersionsRequest parsed;
    ASSERT_TRUE(parsed.ParseFromString(bytes));
    EXPECT_EQ(parsed.page_size(), 25);
    EXPECT_EQ(parsed.page_token(), "tok");
    ASSERT_EQ(parsed.statuses_size(), 1);
    EXPECT_EQ(parsed.statuses(0), ConfigStatus::CONFIG_STATUS_PENDING);
}

// ---------------------------------------------------------------------------
// Phase 9.3.4 RolloutService contract (TASK-20260422-01 Epic A.1)
// ---------------------------------------------------------------------------

TEST(RolloutProto, RolloutStatusEnumWireValuesStable) {
    // SR12 / audit: rollout state machine wire values must not shift silently.
    EXPECT_EQ(RolloutStatus::ROLLOUT_STATUS_UNSPECIFIED, 0);
    EXPECT_EQ(RolloutStatus::ROLLOUT_STATUS_PENDING,     1);
    EXPECT_EQ(RolloutStatus::ROLLOUT_STATUS_PROGRESSING, 2);
    EXPECT_EQ(RolloutStatus::ROLLOUT_STATUS_PAUSED,      3);
    EXPECT_EQ(RolloutStatus::ROLLOUT_STATUS_COMPLETED,   4);
    EXPECT_EQ(RolloutStatus::ROLLOUT_STATUS_FAILED,      5);
    EXPECT_EQ(RolloutStatus::ROLLOUT_STATUS_ABORTED,     6);
}

TEST(RolloutProto, PauseReasonEnumWireValuesStable) {
    EXPECT_EQ(PauseReason::PAUSE_REASON_UNSPECIFIED,   0);
    EXPECT_EQ(PauseReason::PAUSE_REASON_MANUAL,        1);
    EXPECT_EQ(PauseReason::PAUSE_REASON_ERROR_RATE,    2);
    EXPECT_EQ(PauseReason::PAUSE_REASON_LATENCY_RATIO, 3);
    EXPECT_EQ(PauseReason::PAUSE_REASON_AUTO_ROLLBACK, 4);
}

TEST(RolloutProto, ScopeSelectorRoundtrip) {
    ScopeSelector s;
    s.add_tenant_globs("internal-*");
    s.add_tenant_globs("beta-cust-*");
    s.add_regions("us-east");
    s.add_regions("eu-west");
    s.set_percentage(25);

    std::string bytes;
    ASSERT_TRUE(s.SerializeToString(&bytes));

    ScopeSelector parsed;
    ASSERT_TRUE(parsed.ParseFromString(bytes));
    ASSERT_EQ(parsed.tenant_globs_size(), 2);
    EXPECT_EQ(parsed.tenant_globs(0), "internal-*");
    EXPECT_EQ(parsed.tenant_globs(1), "beta-cust-*");
    ASSERT_EQ(parsed.regions_size(), 2);
    EXPECT_EQ(parsed.regions(0), "us-east");
    EXPECT_EQ(parsed.regions(1), "eu-west");
    EXPECT_EQ(parsed.percentage(), 25);
}

TEST(RolloutProto, AutoPausePolicyDoublePrecisionRoundtrip) {
    AutoPausePolicy p;
    p.set_error_rate_gt(0.02);
    p.set_p99_latency_ratio_gt(1.5);
    p.set_absolute_error_rate_gt(0.10);
    p.set_absolute_p99_latency_ms_gt(2000.0);

    std::string bytes;
    ASSERT_TRUE(p.SerializeToString(&bytes));

    AutoPausePolicy parsed;
    ASSERT_TRUE(parsed.ParseFromString(bytes));
    EXPECT_DOUBLE_EQ(parsed.error_rate_gt(), 0.02);
    EXPECT_DOUBLE_EQ(parsed.p99_latency_ratio_gt(), 1.5);
    EXPECT_DOUBLE_EQ(parsed.absolute_error_rate_gt(), 0.10);
    EXPECT_DOUBLE_EQ(parsed.absolute_p99_latency_ms_gt(), 2000.0);
}

TEST(RolloutProto, ObservationPolicyRoundtrip) {
    ObservationPolicy o;
    o.set_min_duration_seconds(300);
    o.set_min_sample_count(1000);

    std::string bytes;
    ASSERT_TRUE(o.SerializeToString(&bytes));

    ObservationPolicy parsed;
    ASSERT_TRUE(parsed.ParseFromString(bytes));
    EXPECT_EQ(parsed.min_duration_seconds(), 300);
    EXPECT_EQ(parsed.min_sample_count(), 1000);
}

TEST(RolloutProto, RolloutStageNestedRoundtrip) {
    RolloutStage st;
    st.set_name("canary-1pct");
    st.mutable_scope()->set_percentage(1);
    st.mutable_scope()->add_tenant_globs("beta-*");
    st.mutable_observation()->set_min_duration_seconds(120);
    st.mutable_observation()->set_min_sample_count(100);
    st.mutable_auto_pause()->set_error_rate_gt(0.02);

    std::string bytes;
    ASSERT_TRUE(st.SerializeToString(&bytes));

    RolloutStage parsed;
    ASSERT_TRUE(parsed.ParseFromString(bytes));
    EXPECT_EQ(parsed.name(), "canary-1pct");
    EXPECT_EQ(parsed.scope().percentage(), 1);
    ASSERT_EQ(parsed.scope().tenant_globs_size(), 1);
    EXPECT_EQ(parsed.scope().tenant_globs(0), "beta-*");
    EXPECT_EQ(parsed.observation().min_duration_seconds(), 120);
    EXPECT_EQ(parsed.observation().min_sample_count(), 100);
    EXPECT_DOUBLE_EQ(parsed.auto_pause().error_rate_gt(), 0.02);
}

TEST(RolloutProto, RolloutSpecWithMultipleStagesRoundtrip) {
    RolloutSpec spec;
    spec.set_target_version_id("01HXNEW");
    spec.set_sticky_key("tenant_id");
    spec.set_auto_rollback_on_pause(true);
    spec.set_auto_rollback_grace_seconds(1800);
    spec.set_creator_comment("canary launch");

    auto* s1 = spec.add_stages();
    s1->set_name("1pct");
    s1->mutable_scope()->set_percentage(1);

    auto* s2 = spec.add_stages();
    s2->set_name("10pct");
    s2->mutable_scope()->set_percentage(10);

    auto* s3 = spec.add_stages();
    s3->set_name("100pct");
    s3->mutable_scope()->set_percentage(100);

    std::string bytes;
    ASSERT_TRUE(spec.SerializeToString(&bytes));

    RolloutSpec parsed;
    ASSERT_TRUE(parsed.ParseFromString(bytes));
    EXPECT_EQ(parsed.target_version_id(), "01HXNEW");
    EXPECT_EQ(parsed.sticky_key(), "tenant_id");
    EXPECT_TRUE(parsed.auto_rollback_on_pause());
    EXPECT_EQ(parsed.auto_rollback_grace_seconds(), 1800);
    ASSERT_EQ(parsed.stages_size(), 3);
    EXPECT_EQ(parsed.stages(0).name(), "1pct");
    EXPECT_EQ(parsed.stages(1).scope().percentage(), 10);
    EXPECT_EQ(parsed.stages(2).scope().percentage(), 100);
}

TEST(RolloutProto, RolloutMessageFullFieldsRoundtrip) {
    Rollout r;
    r.set_rollout_id("01HXY_ROLLOUT");
    r.set_target_version_id("01HXNEW");
    r.set_previous_active_version_id("01HXPREV");
    r.mutable_spec()->set_target_version_id("01HXNEW");
    r.mutable_spec()->set_sticky_key("tenant_id");
    r.set_status(RolloutStatus::ROLLOUT_STATUS_PROGRESSING);
    r.set_current_stage_index(1);
    r.set_started_at(1713600000000LL);
    r.set_stage_started_at(1713600060000LL);
    r.set_paused_at(0);
    r.set_pause_reason(PauseReason::PAUSE_REASON_UNSPECIFIED);
    r.set_creator("alice");
    r.set_last_actor("alice");
    r.set_completed_at(0);
    r.set_chain_hash("cafebabe");

    std::string bytes;
    ASSERT_TRUE(r.SerializeToString(&bytes));

    Rollout parsed;
    ASSERT_TRUE(parsed.ParseFromString(bytes));
    EXPECT_EQ(parsed.rollout_id(), "01HXY_ROLLOUT");
    EXPECT_EQ(parsed.target_version_id(), "01HXNEW");
    EXPECT_EQ(parsed.previous_active_version_id(), "01HXPREV");
    EXPECT_EQ(parsed.spec().target_version_id(), "01HXNEW");
    EXPECT_EQ(parsed.status(), RolloutStatus::ROLLOUT_STATUS_PROGRESSING);
    EXPECT_EQ(parsed.current_stage_index(), 1);
    EXPECT_EQ(parsed.started_at(), 1713600000000LL);
    EXPECT_EQ(parsed.stage_started_at(), 1713600060000LL);
    EXPECT_EQ(parsed.creator(), "alice");
    EXPECT_EQ(parsed.chain_hash(), "cafebabe");
}

TEST(RolloutProto, CreateAndListRequestsRoundtrip) {
    CreateRolloutRequest cr;
    cr.mutable_spec()->set_target_version_id("V1");
    cr.mutable_spec()->add_stages()->set_name("first");

    std::string bytes;
    ASSERT_TRUE(cr.SerializeToString(&bytes));
    CreateRolloutRequest cr_parsed;
    ASSERT_TRUE(cr_parsed.ParseFromString(bytes));
    EXPECT_EQ(cr_parsed.spec().target_version_id(), "V1");
    ASSERT_EQ(cr_parsed.spec().stages_size(), 1);

    ListRolloutsRequest lr;
    lr.add_statuses(RolloutStatus::ROLLOUT_STATUS_PROGRESSING);
    lr.add_statuses(RolloutStatus::ROLLOUT_STATUS_PAUSED);
    lr.set_page_size(25);
    lr.set_page_token("cursor");
    ASSERT_TRUE(lr.SerializeToString(&bytes));
    ListRolloutsRequest lr_parsed;
    ASSERT_TRUE(lr_parsed.ParseFromString(bytes));
    ASSERT_EQ(lr_parsed.statuses_size(), 2);
    EXPECT_EQ(lr_parsed.statuses(0), RolloutStatus::ROLLOUT_STATUS_PROGRESSING);
    EXPECT_EQ(lr_parsed.statuses(1), RolloutStatus::ROLLOUT_STATUS_PAUSED);
    EXPECT_EQ(lr_parsed.page_size(), 25);
    EXPECT_EQ(lr_parsed.page_token(), "cursor");
}

TEST(RolloutProto, StartPauseResumePromoteAbortRequestsUniformShape) {
    // All five state-transition requests share {rollout_id, comment} layout.
    auto test_one = [](auto req) {
        req.set_rollout_id("R1");
        req.set_comment("ok");
        std::string bytes;
        ASSERT_TRUE(req.SerializeToString(&bytes));
        decltype(req) parsed;
        ASSERT_TRUE(parsed.ParseFromString(bytes));
        EXPECT_EQ(parsed.rollout_id(), "R1");
        EXPECT_EQ(parsed.comment(), "ok");
    };
    test_one(StartRolloutRequest{});
    test_one(PauseRolloutRequest{});
    test_one(ResumeRolloutRequest{});
    test_one(PromoteRolloutRequest{});
    test_one(AbortRolloutRequest{});

    // GetRollout takes rollout_id only.
    GetRolloutRequest g;
    g.set_rollout_id("R2");
    std::string bytes;
    ASSERT_TRUE(g.SerializeToString(&bytes));
    GetRolloutRequest g_parsed;
    ASSERT_TRUE(g_parsed.ParseFromString(bytes));
    EXPECT_EQ(g_parsed.rollout_id(), "R2");
}

TEST(RolloutProto, ListRolloutsResponseRepeatedRoundtrip) {
    ListRolloutsResponse resp;
    for (int i = 0; i < 3; ++i) {
        auto* r = resp.add_rollouts();
        r->set_rollout_id("01HXROLLOUT" + std::to_string(i));
        r->set_status(RolloutStatus::ROLLOUT_STATUS_PROGRESSING);
        r->set_current_stage_index(i);
    }
    resp.set_next_page_token("next");

    std::string bytes;
    ASSERT_TRUE(resp.SerializeToString(&bytes));
    ListRolloutsResponse parsed;
    ASSERT_TRUE(parsed.ParseFromString(bytes));
    ASSERT_EQ(parsed.rollouts_size(), 3);
    EXPECT_EQ(parsed.rollouts(0).rollout_id(), "01HXROLLOUT0");
    EXPECT_EQ(parsed.rollouts(2).current_stage_index(), 2);
    EXPECT_EQ(parsed.next_page_token(), "next");
}

TEST(RolloutGrpc, RolloutServiceAsyncServiceInstantiable) {
    // Link-time smoke: generated gRPC service stub must exist and be
    // instantiable, proving protoc + grpc_cpp_plugin produced valid code
    // for all 8 RolloutService RPC methods.
    RolloutService::AsyncService svc;
    (void)svc;
    SUCCEED();
}

} // namespace
