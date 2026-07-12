// Phase 9.3 Epic 3 Task 3.8 — ConfigServiceCore::activate.
//
// Activation is an atomic swap: previous ACTIVE becomes SUPERSEDED, target
// becomes ACTIVE. The underlying PersistentStore enforces atomicity; this
// test focuses on the ConfigServiceCore orchestration (status precondition,
// audit event, carry of previous_active in detail).

#include "control_plane/config_service_core.h"
#include "guardrail/audit.h"
#include "storage/memory_persistent_store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

namespace aegisgate {
namespace {

constexpr std::int64_t kT0 = 1'700'000'000'000LL;

class ConfigServiceActivateTest : public ::testing::Test {
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

    std::string submitAndApprove(const std::string& submitter,
                                  const std::string& reviewer,
                                  const std::string& yaml) {
        auto sub = svc_->submit(yaml, submitter, "", false);
        EXPECT_EQ(sub.error_code, "");
        auto app = svc_->approve(sub.record.version_id, reviewer, "");
        EXPECT_EQ(app.error_code, "");
        return sub.record.version_id;
    }

    std::unique_ptr<MemoryPersistentStore> store_;
    std::unique_ptr<AuditLogger>           audit_;
    std::unique_ptr<ConfigServiceCore>     svc_;
};

TEST_F(ConfigServiceActivateTest, ActivateApprovedSucceeds) {
    auto id = submitAndApprove("alice", "bob", "v: 1\n");
    auto r = svc_->activate(id, "carol");
    EXPECT_EQ(r.error_code, "");
    EXPECT_EQ(r.record.status, ConfigStatus::ACTIVE);
    EXPECT_EQ(r.record.activator, "carol");
    EXPECT_EQ(r.record.activated_at, kT0);

    auto active = store_->getActiveConfig();
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->version_id, id);
}

TEST_F(ConfigServiceActivateTest, ActivateSupersedesPrevious) {
    auto v1 = submitAndApprove("alice", "bob", "v: 1\n");
    ASSERT_EQ(svc_->activate(v1, "carol").error_code, "");

    auto v2 = submitAndApprove("alice", "bob", "v: 2\n");
    auto r  = svc_->activate(v2, "carol");
    EXPECT_EQ(r.error_code, "");

    auto prev = store_->getConfigVersion(v1);
    ASSERT_TRUE(prev.has_value());
    EXPECT_EQ(prev->status, ConfigStatus::SUPERSEDED);

    auto active = store_->getActiveConfig();
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->version_id, v2);
}

TEST_F(ConfigServiceActivateTest, ActivatePendingFailsPrecondition) {
    auto sub = svc_->submit("v: 1\n", "alice", "", false);
    ASSERT_EQ(sub.error_code, "");
    auto r = svc_->activate(sub.record.version_id, "bob");
    EXPECT_EQ(r.error_code, "FAILED_PRECONDITION");
}

TEST_F(ConfigServiceActivateTest, ActivateMissingReturnsNotFound) {
    auto r = svc_->activate("01HXXXDOESNOTEXIST00000000", "bob");
    EXPECT_EQ(r.error_code, "NOT_FOUND");
}

TEST_F(ConfigServiceActivateTest, ActivateAlreadyActiveFailsPrecondition) {
    auto id = submitAndApprove("alice", "bob", "v: 1\n");
    ASSERT_EQ(svc_->activate(id, "carol").error_code, "");
    auto again = svc_->activate(id, "dave");
    EXPECT_EQ(again.error_code, "FAILED_PRECONDITION");
}

TEST_F(ConfigServiceActivateTest, AuditEmittedWithPreviousActive) {
    auto v1 = submitAndApprove("alice", "bob", "v: 1\n");
    ASSERT_EQ(svc_->activate(v1, "carol").error_code, "");
    auto v2 = submitAndApprove("alice", "bob", "v: 2\n");
    ASSERT_EQ(svc_->activate(v2, "dave").error_code, "");

    ASSERT_TRUE(audit_->flush(std::chrono::seconds{1}));
    int seen = 0;
    for (const auto& e : audit_->entries()) {
        if (e.action == "config.activate") {
            ++seen;
            if (e.detail.find(v2) != std::string::npos) {
                // The second activation must record v1 as previous_active.
                EXPECT_NE(e.detail.find(v1), std::string::npos)
                    << "second activate must mention previous v1: " << e.detail;
            }
        }
    }
    EXPECT_EQ(seen, 2);
}

} // namespace
} // namespace aegisgate
