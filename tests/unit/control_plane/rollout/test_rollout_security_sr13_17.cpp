// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic C.2.
//
// SR13: reserved system user cannot authenticate externally.
// SR16: tenant 24-hour rollout quota.
// SR17: AEGISGATE_DISABLE_AUTO_ROLLBACK env-var kill switch.

#include "control_plane/rollout/rollout_controller.h"
#include "control_plane/rollout/rollout_record.h"
#include "control_plane/rollout/rollout_wiring.h"
#include "storage/memory_persistent_store.h"

#include <gtest/gtest.h>
#include <cstdlib>
#include <memory>

namespace aegisgate {
namespace {

// =========================================================================
// SR16 — rolloutQuotaCheck
// =========================================================================

class RolloutQuotaTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryPersistentStore>();
        ASSERT_TRUE(store_->initialize());
    }

    void insertRollout(const std::string& creator, std::int64_t started_at) {
        RolloutRecord r;
        r.rollout_id = "01RL" + std::to_string(seq_++);
        r.target_version_id = "01VER000000000000000000" + std::to_string(seq_);
        r.creator = creator;
        r.status = RolloutStatus::PROGRESSING;
        r.started_at = started_at;
        ASSERT_TRUE(store_->insertRollout(r));
    }

    std::unique_ptr<MemoryPersistentStore> store_;
    int seq_ = 0;
};

TEST_F(RolloutQuotaTest, EmptyStoreAllows) {
    EXPECT_TRUE(rolloutQuotaCheck(store_.get(), "alice", 10,
                                    1'700'000'000'000LL));
}

TEST_F(RolloutQuotaTest, BelowLimitAllows) {
    const std::int64_t now = 1'700'000'000'000LL;
    insertRollout("alice", now - 3600'000);  // 1h ago
    insertRollout("alice", now - 7200'000);  // 2h ago
    EXPECT_TRUE(rolloutQuotaCheck(store_.get(), "alice", 10, now));
}

TEST_F(RolloutQuotaTest, AtLimitRejects) {
    const std::int64_t now = 1'700'000'000'000LL;
    for (int i = 0; i < 3; ++i)
        insertRollout("alice", now - i * 3600'000);
    EXPECT_FALSE(rolloutQuotaCheck(store_.get(), "alice", 3, now));
}

TEST_F(RolloutQuotaTest, OlderThan24hIgnored) {
    const std::int64_t now = 1'700'000'000'000LL;
    insertRollout("alice", now - 25LL * 3600 * 1000);  // 25h ago
    EXPECT_TRUE(rolloutQuotaCheck(store_.get(), "alice", 1, now));
}

TEST_F(RolloutQuotaTest, DifferentCreatorIsolated) {
    const std::int64_t now = 1'700'000'000'000LL;
    for (int i = 0; i < 5; ++i)
        insertRollout("bob", now - i * 3600'000);
    EXPECT_TRUE(rolloutQuotaCheck(store_.get(), "alice", 1, now));
}

TEST_F(RolloutQuotaTest, NullStoreAlwaysAllows) {
    EXPECT_TRUE(rolloutQuotaCheck(nullptr, "alice", 1,
                                    1'700'000'000'000LL));
}

TEST_F(RolloutQuotaTest, NotStartedIgnored) {
    const std::int64_t now = 1'700'000'000'000LL;
    RolloutRecord r;
    r.rollout_id = "01RLPENDING";
    r.target_version_id = "01VERPENDING";
    r.creator = "alice";
    r.status = RolloutStatus::PENDING;
    r.started_at = 0;  // never started
    ASSERT_TRUE(store_->insertRollout(r));
    EXPECT_TRUE(rolloutQuotaCheck(store_.get(), "alice", 1, now));
}

// =========================================================================
// SR17 — autoRollbackEnabledFromEnv
// =========================================================================

TEST(AutoRollbackEnvTest, DefaultEnabled) {
    ::unsetenv("AEGISGATE_DISABLE_AUTO_ROLLBACK");
    EXPECT_TRUE(autoRollbackEnabledFromEnv());
}

TEST(AutoRollbackEnvTest, DisabledWhenSetTo1) {
    ::setenv("AEGISGATE_DISABLE_AUTO_ROLLBACK", "1", 1);
    EXPECT_FALSE(autoRollbackEnabledFromEnv());
    ::unsetenv("AEGISGATE_DISABLE_AUTO_ROLLBACK");
}

TEST(AutoRollbackEnvTest, EnabledWhenSetToOtherValue) {
    ::setenv("AEGISGATE_DISABLE_AUTO_ROLLBACK", "0", 1);
    EXPECT_TRUE(autoRollbackEnabledFromEnv());
    ::unsetenv("AEGISGATE_DISABLE_AUTO_ROLLBACK");
}

TEST(AutoRollbackEnvTest, EnabledWhenEmpty) {
    ::setenv("AEGISGATE_DISABLE_AUTO_ROLLBACK", "", 1);
    EXPECT_TRUE(autoRollbackEnabledFromEnv());
    ::unsetenv("AEGISGATE_DISABLE_AUTO_ROLLBACK");
}

// =========================================================================
// SR13 — reserved system user cannot authenticate externally
// (The actual AuthService integration is tested via existing auth tests;
//  here we validate the reserved-user policy via the RolloutController
//  Deps.system_user_id contract: "system.autorollback" must never appear
//  as a gRPC-authenticated user.)
// =========================================================================

TEST(ReservedSystemUser, SystemAutorollbackIsReserved) {
    // RolloutController::Deps default system_user_id matches the reserved
    // name that AuthService now rejects.
    RolloutController::Deps d;
    EXPECT_EQ(d.system_user_id, "system.autorollback");
}

}  // namespace
}  // namespace aegisgate
