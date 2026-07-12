// Phase 9.3 Epic 3 Tasks 3.6 + 3.7 — Approve / Reject paths.
//
// Enforces the W3 contract end-to-end:
//   * Reviewer must not equal submitter (SR5, threat T14).
//   * State-machine legality via StateMachine::next() (§5.2).
//   * Audit events emitted only on success.
//   * Idempotency: re-approving a PENDING record twice is rejected with
//     FAILED_PRECONDITION because the second call sees APPROVED.

#include "control_plane/config_service_core.h"
#include "guardrail/audit.h"
#include "storage/memory_persistent_store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

namespace aegisgate {
namespace {

constexpr std::int64_t kT0 = 1'700'000'000'000LL;

class ConfigServiceReviewTest : public ::testing::Test {
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

    std::string submitPendingAs(const std::string& submitter,
                                 const std::string& yaml = "key: value\n") {
        auto r = svc_->submit(yaml, submitter, "comment", false);
        EXPECT_EQ(r.error_code, "");
        return r.record.version_id;
    }

    int countAuditActions(const std::string& action) const {
        int n = 0;
        for (const auto& e : audit_->entries()) {
            if (e.action == action) ++n;
        }
        return n;
    }

    std::unique_ptr<MemoryPersistentStore> store_;
    std::unique_ptr<AuditLogger>           audit_;
    std::unique_ptr<ConfigServiceCore>     svc_;
};

// --------- Approve ---------

TEST_F(ConfigServiceReviewTest, ApproveHappyPath) {
    auto id = submitPendingAs("alice");
    auto r = svc_->approve(id, "bob", "LGTM");
    EXPECT_EQ(r.error_code, "");
    EXPECT_EQ(r.record.status, ConfigStatus::APPROVED);
    EXPECT_EQ(r.record.reviewer, "bob");
    EXPECT_EQ(r.record.reviewer_comment, "LGTM");
    EXPECT_EQ(r.record.reviewed_at, kT0);

    ASSERT_TRUE(audit_->flush(std::chrono::seconds{1}));
    EXPECT_EQ(countAuditActions("config.approve"), 1);
}

TEST_F(ConfigServiceReviewTest, ApproveBySubmitterRejected) {
    // SR5 / T14: same user cannot be both submitter and reviewer.
    auto id = submitPendingAs("alice");
    auto r = svc_->approve(id, "alice", "self-approve");
    EXPECT_EQ(r.error_code, "PERMISSION_DENIED");

    ASSERT_TRUE(audit_->flush(std::chrono::seconds{1}));
    EXPECT_EQ(countAuditActions("config.approve"), 0);

    auto loaded = store_->getConfigVersion(id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->status, ConfigStatus::PENDING);
}

TEST_F(ConfigServiceReviewTest, ApproveMissingIdReturnsNotFound) {
    auto r = svc_->approve("01HXXXDOESNOTEXIST00000000", "bob", "");
    EXPECT_EQ(r.error_code, "NOT_FOUND");
}

TEST_F(ConfigServiceReviewTest, ApproveAlreadyApprovedFailsPrecondition) {
    auto id = submitPendingAs("alice");
    ASSERT_EQ(svc_->approve(id, "bob", "").error_code, "");
    auto again = svc_->approve(id, "carol", "double");
    EXPECT_EQ(again.error_code, "FAILED_PRECONDITION");
}

TEST_F(ConfigServiceReviewTest, ApproveRejectedFailsPrecondition) {
    auto id = submitPendingAs("alice");
    ASSERT_EQ(svc_->reject(id, "bob", "").error_code, "");
    auto r = svc_->approve(id, "carol", "");
    EXPECT_EQ(r.error_code, "FAILED_PRECONDITION");
}

// --------- Reject ---------

TEST_F(ConfigServiceReviewTest, RejectFromPending) {
    auto id = submitPendingAs("alice");
    auto r = svc_->reject(id, "bob", "bad config");
    EXPECT_EQ(r.error_code, "");
    EXPECT_EQ(r.record.status, ConfigStatus::REJECTED);
    EXPECT_EQ(r.record.reviewer, "bob");

    ASSERT_TRUE(audit_->flush(std::chrono::seconds{1}));
    EXPECT_EQ(countAuditActions("config.reject"), 1);
}

TEST_F(ConfigServiceReviewTest, RejectFromApproved) {
    // §5.2 matrix allows APPROVED -> REJECTED.
    auto id = submitPendingAs("alice");
    ASSERT_EQ(svc_->approve(id, "bob", "").error_code, "");
    auto r = svc_->reject(id, "carol", "changed mind");
    EXPECT_EQ(r.error_code, "");
    EXPECT_EQ(r.record.status, ConfigStatus::REJECTED);
}

TEST_F(ConfigServiceReviewTest, RejectBySubmitterDenied) {
    // SR5 extends to reject too — a submitter shouldn't be able to withdraw
    // their own pending version by posing as a reviewer.
    auto id = submitPendingAs("alice");
    auto r = svc_->reject(id, "alice", "nvm");
    EXPECT_EQ(r.error_code, "PERMISSION_DENIED");
}

TEST_F(ConfigServiceReviewTest, RejectActiveFailsPrecondition) {
    // Once a version is ACTIVE the only way out is rollback to a previous
    // SUPERSEDED/ACTIVE version.
    auto id = submitPendingAs("alice");
    ASSERT_EQ(svc_->approve(id, "bob", "").error_code, "");
    ASSERT_EQ(svc_->activate(id, "carol").error_code, "");
    auto r = svc_->reject(id, "dave", "too late");
    EXPECT_EQ(r.error_code, "FAILED_PRECONDITION");
}

TEST_F(ConfigServiceReviewTest, RejectMissingIdReturnsNotFound) {
    auto r = svc_->reject("01HXXXDOESNOTEXIST00000000", "bob", "");
    EXPECT_EQ(r.error_code, "NOT_FOUND");
}

} // namespace
} // namespace aegisgate
