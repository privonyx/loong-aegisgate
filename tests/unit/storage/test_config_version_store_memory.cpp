// Phase 9.3 Epic 2 Task 2.2-2.3 — MemoryPersistentStore ConfigVersion tests.
//
// Covers:
//   * insert + getConfigVersion roundtrip
//   * duplicate version_id rejected
//   * listConfigVersions — default ordering (submitted_at DESC), status
//     filter, since_millis filter, paging
//   * getActiveConfig
//   * activateConfig — atomic transition of previous ACTIVE to SUPERSEDED,
//     target to ACTIVE, with deactivated_at / activated_at bookkeeping
//   * updateConfigStatus — used by approve/reject flows
//
// Tests stay in OFF-path (no grpc dependency).

#include "control_plane/config_version_record.h"
#include "storage/memory_persistent_store.h"
#include <algorithm>
#include <gtest/gtest.h>

namespace aegisgate {
namespace {

static ConfigVersionRecord makeRec(const std::string& id,
                                    ConfigStatus status,
                                    std::int64_t submitted_at,
                                    const std::string& submitter = "alice",
                                    const std::string& sha = "sha-default") {
    ConfigVersionRecord r{};
    r.version_id = id;
    r.content_sha256 = sha;
    r.yaml_content = "server: { port: 8080 }\n";
    r.size_bytes = static_cast<std::int64_t>(r.yaml_content.size());
    r.status = status;
    r.submitter = submitter;
    r.submitter_comment = "initial";
    r.submitted_at = submitted_at;
    return r;
}

class MemoryConfigVersionTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryPersistentStore>();
        ASSERT_TRUE(store_->initialize());
    }
    std::unique_ptr<MemoryPersistentStore> store_;
};

TEST_F(MemoryConfigVersionTest, InsertAndGetRoundtripAllFields) {
    ConfigVersionRecord r = makeRec("01J8A00000000000000000001",
                                     ConfigStatus::PENDING, 1000);
    r.submitter_comment = "initial bundle";
    r.reviewer = "bob";
    r.reviewer_comment = "";
    r.chain_hash = "abcdef";
    ASSERT_TRUE(store_->insertConfigVersion(r));
    auto got = store_->getConfigVersion("01J8A00000000000000000001");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->version_id, r.version_id);
    EXPECT_EQ(got->yaml_content, r.yaml_content);
    EXPECT_EQ(got->content_sha256, r.content_sha256);
    EXPECT_EQ(got->size_bytes, r.size_bytes);
    EXPECT_EQ(got->status, ConfigStatus::PENDING);
    EXPECT_EQ(got->submitter, "alice");
    EXPECT_EQ(got->submitter_comment, "initial bundle");
    EXPECT_EQ(got->submitted_at, 1000);
    EXPECT_EQ(got->reviewer, "bob");
    EXPECT_EQ(got->chain_hash, "abcdef");
}

TEST_F(MemoryConfigVersionTest, DuplicateVersionIdRejected) {
    ConfigVersionRecord r = makeRec("01J8A00000000000000000001",
                                     ConfigStatus::PENDING, 1000);
    ASSERT_TRUE(store_->insertConfigVersion(r));
    r.submitter = "mallory";
    EXPECT_FALSE(store_->insertConfigVersion(r));
    // First submitter should still be recorded
    auto got = store_->getConfigVersion("01J8A00000000000000000001");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->submitter, "alice");
}

TEST_F(MemoryConfigVersionTest, GetMissingReturnsNullopt) {
    EXPECT_FALSE(store_->getConfigVersion("01J8A00000000000000000099").has_value());
}

TEST_F(MemoryConfigVersionTest, ListDefaultOrderBySubmittedAtDesc) {
    store_->insertConfigVersion(makeRec("01J8A00000000000000000001",
                                         ConfigStatus::PENDING, 1000));
    store_->insertConfigVersion(makeRec("01J8A00000000000000000002",
                                         ConfigStatus::PENDING, 3000));
    store_->insertConfigVersion(makeRec("01J8A00000000000000000003",
                                         ConfigStatus::PENDING, 2000));
    auto out = store_->listConfigVersions({});
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].version_id, "01J8A00000000000000000002"); // 3000
    EXPECT_EQ(out[1].version_id, "01J8A00000000000000000003"); // 2000
    EXPECT_EQ(out[2].version_id, "01J8A00000000000000000001"); // 1000
}

TEST_F(MemoryConfigVersionTest, ListFilterByStatus) {
    store_->insertConfigVersion(makeRec("01J8A00000000000000000001",
                                         ConfigStatus::PENDING, 1000));
    store_->insertConfigVersion(makeRec("01J8A00000000000000000002",
                                         ConfigStatus::REJECTED, 2000));
    store_->insertConfigVersion(makeRec("01J8A00000000000000000003",
                                         ConfigStatus::APPROVED, 3000));

    ConfigVersionQuery q;
    q.statuses = {ConfigStatus::PENDING, ConfigStatus::APPROVED};
    auto out = store_->listConfigVersions(q);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].version_id, "01J8A00000000000000000003"); // APPROVED, 3000
    EXPECT_EQ(out[1].version_id, "01J8A00000000000000000001"); // PENDING, 1000
}

TEST_F(MemoryConfigVersionTest, ListFilterBySinceMillis) {
    store_->insertConfigVersion(makeRec("01J8A00000000000000000001",
                                         ConfigStatus::PENDING, 1000));
    store_->insertConfigVersion(makeRec("01J8A00000000000000000002",
                                         ConfigStatus::PENDING, 2000));
    store_->insertConfigVersion(makeRec("01J8A00000000000000000003",
                                         ConfigStatus::PENDING, 3000));
    ConfigVersionQuery q;
    q.since_millis = 2000;
    auto out = store_->listConfigVersions(q);
    ASSERT_EQ(out.size(), 2u);
    // Only 3000 and 2000 pass the since filter.
    EXPECT_EQ(out[0].version_id, "01J8A00000000000000000003");
    EXPECT_EQ(out[1].version_id, "01J8A00000000000000000002");
}

TEST_F(MemoryConfigVersionTest, ListRespectsLimitAndPageToken) {
    for (int i = 1; i <= 5; ++i) {
        char id[32];
        snprintf(id, sizeof(id), "01J8A0000000000000000000%d", i);
        store_->insertConfigVersion(makeRec(id, ConfigStatus::PENDING, i * 1000));
    }
    ConfigVersionQuery q;
    q.limit = 2;
    auto page1 = store_->listConfigVersions(q);
    ASSERT_EQ(page1.size(), 2u);
    EXPECT_EQ(page1[0].version_id, "01J8A00000000000000000005");
    EXPECT_EQ(page1[1].version_id, "01J8A00000000000000000004");

    // Next page starts after last seen id
    q.page_token = page1.back().version_id;
    auto page2 = store_->listConfigVersions(q);
    ASSERT_EQ(page2.size(), 2u);
    EXPECT_EQ(page2[0].version_id, "01J8A00000000000000000003");
    EXPECT_EQ(page2[1].version_id, "01J8A00000000000000000002");
}

TEST_F(MemoryConfigVersionTest, UpdateConfigStatusWritesReviewerFields) {
    store_->insertConfigVersion(makeRec("01J8A00000000000000000001",
                                         ConfigStatus::PENDING, 1000));
    ASSERT_TRUE(store_->updateConfigStatus(
        "01J8A00000000000000000001",
        ConfigStatus::APPROVED,
        "bob", "LGTM", 2500));
    auto got = store_->getConfigVersion("01J8A00000000000000000001");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, ConfigStatus::APPROVED);
    EXPECT_EQ(got->reviewer, "bob");
    EXPECT_EQ(got->reviewer_comment, "LGTM");
    EXPECT_EQ(got->reviewed_at, 2500);
}

TEST_F(MemoryConfigVersionTest, UpdateConfigStatusForMissingReturnsFalse) {
    EXPECT_FALSE(store_->updateConfigStatus(
        "nope", ConfigStatus::APPROVED, "bob", "", 0));
}

TEST_F(MemoryConfigVersionTest, ActivateApprovedVersionMakesActive) {
    store_->insertConfigVersion(makeRec("01J8A00000000000000000001",
                                         ConfigStatus::APPROVED, 1000));
    EXPECT_FALSE(store_->getActiveConfig().has_value());

    ASSERT_TRUE(store_->activateConfig(
        "01J8A00000000000000000001", "carol", 2000));

    auto active = store_->getActiveConfig();
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->version_id, "01J8A00000000000000000001");
    EXPECT_EQ(active->status, ConfigStatus::ACTIVE);
    EXPECT_EQ(active->activator, "carol");
    EXPECT_EQ(active->activated_at, 2000);
    EXPECT_EQ(active->deactivated_at, 0);
}

TEST_F(MemoryConfigVersionTest, ActivateSupersedesPreviousActive) {
    store_->insertConfigVersion(makeRec("01J8A00000000000000000001",
                                         ConfigStatus::APPROVED, 1000));
    ASSERT_TRUE(store_->activateConfig(
        "01J8A00000000000000000001", "carol", 1500));

    store_->insertConfigVersion(makeRec("01J8A00000000000000000002",
                                         ConfigStatus::APPROVED, 2000));
    ASSERT_TRUE(store_->activateConfig(
        "01J8A00000000000000000002", "dave", 2500));

    auto prev = store_->getConfigVersion("01J8A00000000000000000001");
    ASSERT_TRUE(prev.has_value());
    EXPECT_EQ(prev->status, ConfigStatus::SUPERSEDED);
    EXPECT_EQ(prev->deactivated_at, 2500);

    auto active = store_->getActiveConfig();
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->version_id, "01J8A00000000000000000002");
}

TEST_F(MemoryConfigVersionTest, ActivateRejectedVersionFails) {
    store_->insertConfigVersion(makeRec("01J8A00000000000000000001",
                                         ConfigStatus::REJECTED, 1000));
    EXPECT_FALSE(store_->activateConfig(
        "01J8A00000000000000000001", "carol", 2000));
    EXPECT_FALSE(store_->getActiveConfig().has_value());
}

TEST_F(MemoryConfigVersionTest, ActivateMissingFails) {
    EXPECT_FALSE(store_->activateConfig("missing-id", "carol", 0));
}

TEST_F(MemoryConfigVersionTest, ActivateSupersededEnablesR2Rollback) {
    // v1 is the previously active bundle
    store_->insertConfigVersion(makeRec("01J8A00000000000000000001",
                                         ConfigStatus::APPROVED, 1000));
    ASSERT_TRUE(store_->activateConfig(
        "01J8A00000000000000000001", "carol", 1500));
    // v2 supersedes v1
    store_->insertConfigVersion(makeRec("01J8A00000000000000000002",
                                         ConfigStatus::APPROVED, 2000));
    ASSERT_TRUE(store_->activateConfig(
        "01J8A00000000000000000002", "dave", 2500));
    auto v1_after = store_->getConfigVersion("01J8A00000000000000000001");
    ASSERT_EQ(v1_after->status, ConfigStatus::SUPERSEDED);

    // R2: roll back to v1 (SUPERSEDED -> ACTIVE)
    ASSERT_TRUE(store_->activateConfig(
        "01J8A00000000000000000001", "erin", 3000));
    auto active_now = store_->getActiveConfig();
    ASSERT_TRUE(active_now.has_value());
    EXPECT_EQ(active_now->version_id, "01J8A00000000000000000001");

    auto v2_after = store_->getConfigVersion("01J8A00000000000000000002");
    ASSERT_EQ(v2_after->status, ConfigStatus::SUPERSEDED);
    EXPECT_EQ(v2_after->deactivated_at, 3000);
}

TEST_F(MemoryConfigVersionTest, ActivateCurrentActiveIsIdempotent) {
    store_->insertConfigVersion(makeRec("01J8A00000000000000000001",
                                         ConfigStatus::APPROVED, 1000));
    ASSERT_TRUE(store_->activateConfig(
        "01J8A00000000000000000001", "carol", 1500));
    // Activating the current ACTIVE must succeed without side effects.
    EXPECT_TRUE(store_->activateConfig(
        "01J8A00000000000000000001", "carol", 9999));
    auto got = store_->getConfigVersion("01J8A00000000000000000001");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, ConfigStatus::ACTIVE);
    // Activated_at from the first activation is preserved (idempotent).
    EXPECT_EQ(got->activated_at, 1500);
}

} // namespace
} // namespace aegisgate
