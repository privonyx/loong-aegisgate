// Phase 9.3 Epic 5 Task 5.1 + 5.2 — gRPC adapter (pure converters + Impl).
//
// Two distinct suites share this translation unit because they exercise the
// same proto <-> core boundary:
//
//   * GrpcAdapterConvertersTest — pure functions; no gRPC machinery.
//   * ConfigServiceImplTest     — drives the full handler surface against a
//                                 real ConfigServiceCore + MemoryPersistentStore,
//                                 asserting proto roundtrip, SR8 error
//                                 collapse, SR11 yaml strip, SR1 extractor
//                                 gate, and W3 enforcement through gRPC.

#include "control_plane/config_service_core.h"
#include "control_plane/grpc/config_service_grpc_adapter.h"
#include "core/config.h"
#include "guardrail/audit.h"
#include "storage/memory_persistent_store.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace aegisgate {
namespace pb = aegisgate::controlplane::v1;
using control_plane::grpc_adapter::ConfigServiceImpl;
using control_plane::grpc_adapter::UserExtractor;

namespace {

// ---------------------------------------------------------------------------
// Task 5.1 — pure converters
// ---------------------------------------------------------------------------

TEST(GrpcAdapterConverters, StatusToProtoMapsAllFiveLifecycleStates) {
    using control_plane::grpc_adapter::statusToProto;
    EXPECT_EQ(statusToProto(ConfigStatus::PENDING),
              pb::CONFIG_STATUS_PENDING);
    EXPECT_EQ(statusToProto(ConfigStatus::APPROVED),
              pb::CONFIG_STATUS_APPROVED);
    EXPECT_EQ(statusToProto(ConfigStatus::REJECTED),
              pb::CONFIG_STATUS_REJECTED);
    EXPECT_EQ(statusToProto(ConfigStatus::ACTIVE),
              pb::CONFIG_STATUS_ACTIVE);
    EXPECT_EQ(statusToProto(ConfigStatus::SUPERSEDED),
              pb::CONFIG_STATUS_SUPERSEDED);
}

TEST(GrpcAdapterConverters, StatusFromProtoReturnsNulloptOnUnspecified) {
    using control_plane::grpc_adapter::statusFromProto;
    EXPECT_FALSE(statusFromProto(pb::CONFIG_STATUS_UNSPECIFIED).has_value());
    EXPECT_EQ(statusFromProto(pb::CONFIG_STATUS_PENDING).value(),
              ConfigStatus::PENDING);
    EXPECT_EQ(statusFromProto(pb::CONFIG_STATUS_SUPERSEDED).value(),
              ConfigStatus::SUPERSEDED);
}

TEST(GrpcAdapterConverters, ConfigVersionRoundtripPreservesAllFields) {
    ConfigVersionRecord rec;
    rec.version_id = "01J8A00000000000000000042";
    rec.content_sha256 = std::string(64, 'a');
    rec.yaml_content = "server:\n  port: 8080\n";
    rec.size_bytes = static_cast<std::int64_t>(rec.yaml_content.size());
    rec.status = ConfigStatus::ACTIVE;
    rec.submitter = "alice";
    rec.submitter_comment = "initial";
    rec.submitted_at = 1'700'000'000'000LL;
    rec.reviewer = "bob";
    rec.reviewer_comment = "LGTM";
    rec.reviewed_at = 1'700'000'001'000LL;
    rec.activator = "carol";
    rec.activated_at = 1'700'000'002'000LL;
    rec.deactivated_at = 0;
    rec.chain_hash = "deadbeef";

    auto msg = control_plane::grpc_adapter::toProto(rec);
    auto parsed = control_plane::grpc_adapter::fromProto(msg);

    EXPECT_EQ(parsed.version_id, rec.version_id);
    EXPECT_EQ(parsed.content_sha256, rec.content_sha256);
    EXPECT_EQ(parsed.yaml_content, rec.yaml_content);
    EXPECT_EQ(parsed.size_bytes, rec.size_bytes);
    EXPECT_EQ(parsed.status, ConfigStatus::ACTIVE);
    EXPECT_EQ(parsed.submitter, "alice");
    EXPECT_EQ(parsed.submitter_comment, "initial");
    EXPECT_EQ(parsed.submitted_at, 1'700'000'000'000LL);
    EXPECT_EQ(parsed.reviewer, "bob");
    EXPECT_EQ(parsed.reviewer_comment, "LGTM");
    EXPECT_EQ(parsed.reviewed_at, 1'700'000'001'000LL);
    EXPECT_EQ(parsed.activator, "carol");
    EXPECT_EQ(parsed.activated_at, 1'700'000'002'000LL);
    EXPECT_EQ(parsed.deactivated_at, 0);
    EXPECT_EQ(parsed.chain_hash, "deadbeef");
}

TEST(GrpcAdapterConverters, FromProtoUnspecifiedStatusFallsBackToPending) {
    // fromProto must stay total — upstream validates status with
    // statusFromProto() when proto strictness matters.
    pb::ConfigVersion msg;
    msg.set_version_id("01J");
    msg.set_status(pb::CONFIG_STATUS_UNSPECIFIED);
    auto r = control_plane::grpc_adapter::fromProto(msg);
    EXPECT_EQ(r.status, ConfigStatus::PENDING);
}

// ---------------------------------------------------------------------------
// Task 5.2 — error mapping
// ---------------------------------------------------------------------------

TEST(GrpcAdapterErrors, MapsKnownCodesToExpectedGrpcStatus) {
    using control_plane::grpc_adapter::toGrpcStatus;

    EXPECT_EQ(toGrpcStatus("", "").error_code(), grpc::OK);
    EXPECT_EQ(toGrpcStatus("PAYLOAD_TOO_LARGE", "m").error_code(),
              grpc::INVALID_ARGUMENT);
    EXPECT_EQ(toGrpcStatus("SENSITIVE_FIELD_DETECTED", "m").error_code(),
              grpc::INVALID_ARGUMENT);
    EXPECT_EQ(toGrpcStatus("INVALID_YAML", "m").error_code(),
              grpc::INVALID_ARGUMENT);
    EXPECT_EQ(toGrpcStatus("CONFIG_VALIDATION_FAILED", "m").error_code(),
              grpc::INVALID_ARGUMENT);
    EXPECT_EQ(toGrpcStatus("ALREADY_EXISTS", "m").error_code(),
              grpc::ALREADY_EXISTS);
    EXPECT_EQ(toGrpcStatus("NOT_FOUND", "m").error_code(),
              grpc::NOT_FOUND);
    EXPECT_EQ(toGrpcStatus("PERMISSION_DENIED", "m").error_code(),
              grpc::PERMISSION_DENIED);
    EXPECT_EQ(toGrpcStatus("FAILED_PRECONDITION", "m").error_code(),
              grpc::FAILED_PRECONDITION);
    EXPECT_EQ(toGrpcStatus("RATE_LIMITED", "m").error_code(),
              grpc::RESOURCE_EXHAUSTED);
    EXPECT_EQ(toGrpcStatus("EMERGENCY_NOT_IMPLEMENTED", "m").error_code(),
              grpc::UNIMPLEMENTED);
}

TEST(GrpcAdapterErrors, UnknownCodeCollapsesToInternalWithSafeMessage) {
    // SR8 defence: unknown / new codes must never expose their raw text or
    // any DB driver message on the wire.
    using control_plane::grpc_adapter::toGrpcStatus;
    auto s = toGrpcStatus("SQLITE_DISK_IO_ERR_AT_/tmp/secret.db", "raw error");
    EXPECT_EQ(s.error_code(), grpc::INTERNAL);
    EXPECT_EQ(s.error_message(), "internal error");
    EXPECT_EQ(s.error_message().find("secret"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Task 5.2 — ConfigServiceImpl driven through a real core
// ---------------------------------------------------------------------------

constexpr std::int64_t kT0 = 1'700'000'000'000LL;

class ConfigServiceImplTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryPersistentStore>();
        ASSERT_TRUE(store_->initialize());
        audit_ = std::make_unique<AuditLogger>();
        audit_->setPersistentStore(store_.get());

        ConfigServiceCore::Deps deps;
        deps.store = store_.get();
        deps.audit = audit_.get();
        deps.clock = []() { return kT0; };
        deps.validator = [](const std::string&) {
            return std::vector<Config::ValidationIssue>{};
        };
        core_ = std::make_unique<ConfigServiceCore>(std::move(deps));

        // The default extractor hands out `current_user_` so each test can
        // swap it (simulates an AuthInterceptor handing off different users).
        impl_ = std::make_unique<ConfigServiceImpl>(
            core_.get(),
            [this](grpc::ServerContext*) { return current_user_; });
    }

    // Helper: submit a YAML as `user` and return version_id.
    std::string submitAs(const std::string& user, const std::string& yaml) {
        current_user_ = user;
        pb::SubmitVersionRequest req;
        req.set_yaml_content(yaml);
        pb::ConfigVersion resp;
        grpc::ServerContext ctx;
        auto st = impl_->SubmitVersion(&ctx, &req, &resp);
        EXPECT_TRUE(st.ok()) << st.error_message();
        return resp.version_id();
    }

    std::string current_user_ = "alice";

    std::unique_ptr<MemoryPersistentStore> store_;
    std::unique_ptr<AuditLogger>           audit_;
    std::unique_ptr<ConfigServiceCore>     core_;
    std::unique_ptr<ConfigServiceImpl>     impl_;
};

TEST_F(ConfigServiceImplTest, SubmitHappyPathReturnsPendingProto) {
    current_user_ = "alice";
    pb::SubmitVersionRequest req;
    req.set_yaml_content("server:\n  port: 8080\n");
    req.set_submitter_comment("initial");
    pb::ConfigVersion resp;
    grpc::ServerContext ctx;
    auto st = impl_->SubmitVersion(&ctx, &req, &resp);
    ASSERT_TRUE(st.ok()) << st.error_message();
    EXPECT_EQ(resp.status(), pb::CONFIG_STATUS_PENDING);
    EXPECT_EQ(resp.submitter(), "alice");
    EXPECT_EQ(resp.size_bytes(),
              static_cast<std::int64_t>(req.yaml_content().size()));
    // Submit response is the full record, so it carries yaml_content back —
    // operator explicitly needs this to compute the local sha256.
    EXPECT_EQ(resp.yaml_content(), req.yaml_content());
}

TEST_F(ConfigServiceImplTest, SubmitMissingUserReturnsUnauthenticated) {
    current_user_ = "";  // simulate AuthInterceptor not resolving anyone
    pb::SubmitVersionRequest req;
    req.set_yaml_content("k: 1\n");
    pb::ConfigVersion resp;
    grpc::ServerContext ctx;
    auto st = impl_->SubmitVersion(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::UNAUTHENTICATED);
}

TEST_F(ConfigServiceImplTest, SubmitOversizedYamlMapsToInvalidArgument) {
    current_user_ = "alice";
    pb::SubmitVersionRequest req;
    req.set_yaml_content(std::string(1024 * 1024 + 1, 'x'));  // SR2: >1MiB
    pb::ConfigVersion resp;
    grpc::ServerContext ctx;
    auto st = impl_->SubmitVersion(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::INVALID_ARGUMENT);
}

TEST_F(ConfigServiceImplTest, SubmitDuplicateYamlMapsToAlreadyExists) {
    submitAs("alice", "k: dup\n");
    current_user_ = "alice";
    pb::SubmitVersionRequest req;
    req.set_yaml_content("k: dup\n");
    pb::ConfigVersion resp;
    grpc::ServerContext ctx;
    auto st = impl_->SubmitVersion(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::ALREADY_EXISTS);
}

TEST_F(ConfigServiceImplTest, ApproveEnforcesSR5SubmitterNotReviewer) {
    auto vid = submitAs("alice", "k: sr5\n");
    current_user_ = "alice";  // same user trying to approve themselves
    pb::ApproveVersionRequest req;
    req.set_version_id(vid);
    pb::ConfigVersion resp;
    grpc::ServerContext ctx;
    auto st = impl_->ApproveVersion(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::PERMISSION_DENIED);
}

TEST_F(ConfigServiceImplTest, ApproveHappyPathReturnsApproved) {
    auto vid = submitAs("alice", "k: approve\n");
    current_user_ = "bob";
    pb::ApproveVersionRequest req;
    req.set_version_id(vid);
    req.set_reviewer_comment("LGTM");
    pb::ConfigVersion resp;
    grpc::ServerContext ctx;
    ASSERT_TRUE(impl_->ApproveVersion(&ctx, &req, &resp).ok());
    EXPECT_EQ(resp.status(), pb::CONFIG_STATUS_APPROVED);
    EXPECT_EQ(resp.reviewer(), "bob");
    EXPECT_EQ(resp.reviewer_comment(), "LGTM");
}

TEST_F(ConfigServiceImplTest, RejectNonexistentVersionMapsToNotFound) {
    current_user_ = "bob";
    pb::RejectVersionRequest req;
    req.set_version_id("01J-does-not-exist");
    pb::ConfigVersion resp;
    grpc::ServerContext ctx;
    auto st = impl_->RejectVersion(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::NOT_FOUND);
}

TEST_F(ConfigServiceImplTest, ActivatePendingMapsToFailedPrecondition) {
    auto vid = submitAs("alice", "k: must-approve-first\n");
    current_user_ = "carol";
    pb::ActivateVersionRequest req;
    req.set_version_id(vid);
    pb::ConfigVersion resp;
    grpc::ServerContext ctx;
    auto st = impl_->ActivateVersion(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::FAILED_PRECONDITION);
}

TEST_F(ConfigServiceImplTest, ActivateApprovedTransitionsToActive) {
    auto vid = submitAs("alice", "k: activate-me\n");
    {
        current_user_ = "bob";
        pb::ApproveVersionRequest req;
        req.set_version_id(vid);
        pb::ConfigVersion tmp;
        grpc::ServerContext ctx;
        ASSERT_TRUE(impl_->ApproveVersion(&ctx, &req, &tmp).ok());
    }
    current_user_ = "carol";
    pb::ActivateVersionRequest req;
    req.set_version_id(vid);
    pb::ConfigVersion resp;
    grpc::ServerContext ctx;
    ASSERT_TRUE(impl_->ActivateVersion(&ctx, &req, &resp).ok());
    EXPECT_EQ(resp.status(), pb::CONFIG_STATUS_ACTIVE);
    EXPECT_EQ(resp.activator(), "carol");
}

TEST_F(ConfigServiceImplTest, RollbackEmergencyMapsToUnimplemented) {
    auto vid = submitAs("alice", "k: rollback\n");
    {
        current_user_ = "bob";
        pb::ApproveVersionRequest req;
        req.set_version_id(vid);
        pb::ConfigVersion tmp;
        grpc::ServerContext ctx;
        ASSERT_TRUE(impl_->ApproveVersion(&ctx, &req, &tmp).ok());
    }
    {
        current_user_ = "carol";
        pb::ActivateVersionRequest req;
        req.set_version_id(vid);
        pb::ConfigVersion tmp;
        grpc::ServerContext ctx;
        ASSERT_TRUE(impl_->ActivateVersion(&ctx, &req, &tmp).ok());
    }
    current_user_ = "carol";
    pb::RollbackVersionRequest req;
    req.set_target_version_id(vid);
    req.set_emergency(true);
    pb::ConfigVersion resp;
    grpc::ServerContext ctx;
    auto st = impl_->RollbackVersion(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::UNIMPLEMENTED);
}

TEST_F(ConfigServiceImplTest, ListVersionsStripsYamlContent_SR11) {
    submitAs("alice", "k: listed\n");
    submitAs("alice", "k: listed2\n");
    current_user_ = "bob";
    pb::ListVersionsRequest req;
    pb::ListVersionsResponse resp;
    grpc::ServerContext ctx;
    ASSERT_TRUE(impl_->ListVersions(&ctx, &req, &resp).ok());
    ASSERT_EQ(resp.versions_size(), 2);
    for (int i = 0; i < resp.versions_size(); ++i) {
        EXPECT_TRUE(resp.versions(i).yaml_content().empty())
            << "yaml_content leaked at index " << i;
        // size_bytes must still round-trip; operators need it for UI.
        EXPECT_GT(resp.versions(i).size_bytes(), 0);
    }
}

TEST_F(ConfigServiceImplTest, ListVersionsRejectsUnspecifiedStatusFilter) {
    current_user_ = "bob";
    pb::ListVersionsRequest req;
    req.add_statuses(pb::CONFIG_STATUS_UNSPECIFIED);
    pb::ListVersionsResponse resp;
    grpc::ServerContext ctx;
    auto st = impl_->ListVersions(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::INVALID_ARGUMENT);
}

TEST_F(ConfigServiceImplTest, GetVersionRetainsYamlContent) {
    auto vid = submitAs("alice", "full-bundle: yes\n");
    current_user_ = "bob";
    pb::GetVersionRequest req;
    req.set_version_id(vid);
    pb::ConfigVersion resp;
    grpc::ServerContext ctx;
    ASSERT_TRUE(impl_->GetVersion(&ctx, &req, &resp).ok());
    EXPECT_EQ(resp.yaml_content(), "full-bundle: yes\n");
}

TEST_F(ConfigServiceImplTest, GetVersionMissingIdMapsToInvalidArgument) {
    current_user_ = "bob";
    pb::GetVersionRequest req;  // empty version_id
    pb::ConfigVersion resp;
    grpc::ServerContext ctx;
    auto st = impl_->GetVersion(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::INVALID_ARGUMENT);
}

TEST_F(ConfigServiceImplTest, GetActiveReturnsNotFoundWhenNothingActive) {
    current_user_ = "bob";
    pb::GetActiveRequest req;
    pb::ConfigVersion resp;
    grpc::ServerContext ctx;
    auto st = impl_->GetActive(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::NOT_FOUND);
}

TEST_F(ConfigServiceImplTest, DiffVersionsRequiresBothIds) {
    current_user_ = "bob";
    pb::DiffVersionsRequest req;
    req.set_from_version_id("some-id");
    // to_version_id intentionally empty
    pb::DiffVersionsResponse resp;
    grpc::ServerContext ctx;
    auto st = impl_->DiffVersions(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::INVALID_ARGUMENT);
}

TEST_F(ConfigServiceImplTest, DiffVersionsReturnsUnifiedDiff) {
    auto v1 = submitAs("alice", "foo: 1\n");
    auto v2 = submitAs("alice", "foo: 2\n");
    current_user_ = "bob";
    pb::DiffVersionsRequest req;
    req.set_from_version_id(v1);
    req.set_to_version_id(v2);
    pb::DiffVersionsResponse resp;
    grpc::ServerContext ctx;
    ASSERT_TRUE(impl_->DiffVersions(&ctx, &req, &resp).ok());
    EXPECT_FALSE(resp.unified_diff().empty());
    // Diff must not leak temp paths.
    EXPECT_EQ(resp.unified_diff().find("/tmp/"), std::string::npos);
}

} // namespace
} // namespace aegisgate
