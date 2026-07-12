// Phase 9.3 Epic 3 Task 3.9 — ConfigServiceCore::rollback (R2 + R3 reserved).
//
// Verifies:
//   * R2 exemption: rollback target must be ACTIVE or SUPERSEDED; PENDING,
//     APPROVED or REJECTED return FAILED_PRECONDITION with a caller-friendly
//     message that nudges them to ActivateVersion.
//   * R3 reservation: emergency=true always returns EMERGENCY_NOT_IMPLEMENTED
//     even when the target state is otherwise legal.
//   * Idempotency: rollback to the current ACTIVE succeeds without mutating.
//   * Audit: `config.rollback` carries target_version_id and previous_active.

#include "control_plane/config_service_core.h"
#include "guardrail/audit.h"
#include "storage/memory_persistent_store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

namespace aegisgate {
namespace {

constexpr std::int64_t kT0 = 1'700'000'000'000LL;

class ConfigServiceRollbackTest : public ::testing::Test {
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
        svc_ = std::make_unique<ConfigServiceCore>(std::move(deps));
    }

    std::string submitApproveActivate(const std::string& yaml) {
        auto sub = svc_->submit(yaml, "alice", "", false);
        EXPECT_EQ(sub.error_code, "");
        auto app = svc_->approve(sub.record.version_id, "bob", "");
        EXPECT_EQ(app.error_code, "");
        auto act = svc_->activate(sub.record.version_id, "carol");
        EXPECT_EQ(act.error_code, "");
        return sub.record.version_id;
    }

    std::unique_ptr<MemoryPersistentStore> store_;
    std::unique_ptr<AuditLogger>           audit_;
    std::unique_ptr<ConfigServiceCore>     svc_;
};

TEST_F(ConfigServiceRollbackTest, RollbackToSupersededSucceeds) {
    auto v1 = submitApproveActivate("v: 1\n");
    auto v2 = submitApproveActivate("v: 2\n");
    // v1 is now SUPERSEDED, v2 is ACTIVE.
    auto r = svc_->rollback(v1, "dave", "revert", /*emergency=*/false);
    EXPECT_EQ(r.error_code, "");
    EXPECT_EQ(r.record.status, ConfigStatus::ACTIVE);

    auto v2_rec = store_->getConfigVersion(v2);
    ASSERT_TRUE(v2_rec.has_value());
    EXPECT_EQ(v2_rec->status, ConfigStatus::SUPERSEDED);
}

TEST_F(ConfigServiceRollbackTest, RollbackToCurrentActiveIsIdempotent) {
    auto v1 = submitApproveActivate("v: 1\n");
    auto r = svc_->rollback(v1, "dave", "", false);
    EXPECT_EQ(r.error_code, "");
    EXPECT_EQ(r.record.status, ConfigStatus::ACTIVE);
    EXPECT_EQ(r.record.version_id, v1);
}

TEST_F(ConfigServiceRollbackTest, RollbackToPendingRejected) {
    auto sub = svc_->submit("v: 1\n", "alice", "", false);
    auto r = svc_->rollback(sub.record.version_id, "dave", "", false);
    EXPECT_EQ(r.error_code, "FAILED_PRECONDITION");
    EXPECT_NE(r.error_message.find("ActivateVersion"), std::string::npos);
}

TEST_F(ConfigServiceRollbackTest, RollbackToApprovedRejected) {
    auto sub = svc_->submit("v: 1\n", "alice", "", false);
    ASSERT_EQ(svc_->approve(sub.record.version_id, "bob", "").error_code, "");
    auto r = svc_->rollback(sub.record.version_id, "dave", "", false);
    EXPECT_EQ(r.error_code, "FAILED_PRECONDITION");
}

TEST_F(ConfigServiceRollbackTest, RollbackToRejectedRejected) {
    auto sub = svc_->submit("v: 1\n", "alice", "", false);
    ASSERT_EQ(svc_->reject(sub.record.version_id, "bob", "").error_code, "");
    auto r = svc_->rollback(sub.record.version_id, "dave", "", false);
    EXPECT_EQ(r.error_code, "FAILED_PRECONDITION");
}

TEST_F(ConfigServiceRollbackTest, EmergencyFlagNotImplemented) {
    // R3 gate — even when target state is legal, emergency=true must fail
    // with EMERGENCY_NOT_IMPLEMENTED so CLI users discover the feature exists
    // but is disabled in Phase 9.3.
    auto v1 = submitApproveActivate("v: 1\n");
    auto v2 = submitApproveActivate("v: 2\n");
    auto r = svc_->rollback(v1, "dave", "urgent", /*emergency=*/true);
    EXPECT_EQ(r.error_code, "EMERGENCY_NOT_IMPLEMENTED");
    // And nothing changed.
    auto active = store_->getActiveConfig();
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->version_id, v2);
}

TEST_F(ConfigServiceRollbackTest, RollbackMissingReturnsNotFound) {
    auto r = svc_->rollback("01HXXXDOESNOTEXIST00000000", "dave", "", false);
    EXPECT_EQ(r.error_code, "NOT_FOUND");
}

TEST_F(ConfigServiceRollbackTest, AuditCarriesTargetAndPreviousActive) {
    auto v1 = submitApproveActivate("v: 1\n");
    auto v2 = submitApproveActivate("v: 2\n");
    ASSERT_EQ(svc_->rollback(v1, "dave", "revert", false).error_code, "");

    ASSERT_TRUE(audit_->flush(std::chrono::seconds{1}));
    bool seen = false;
    for (const auto& e : audit_->entries()) {
        if (e.action != "config.rollback") continue;
        EXPECT_NE(e.detail.find(v1), std::string::npos);
        EXPECT_NE(e.detail.find(v2), std::string::npos);  // previous_active
        seen = true;
    }
    EXPECT_TRUE(seen);
}

} // namespace
} // namespace aegisgate
