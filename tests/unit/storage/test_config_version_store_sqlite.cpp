// Phase 9.3 Epic 2 Task 2.4 — SQLitePersistentStore ConfigVersion tests.
//
// Mirrors the MemoryPersistentStore suite but exercises the SQLite
// backend to confirm:
//   * schema creates cleanly (initialize OK)
//   * CRUD + W3 reviewer updates + list ordering/filter/paging
//   * activateConfig is atomic — on failure or concurrent retry the row
//     set still has at most one ACTIVE
//   * yaml_content round-trips as BLOB (preserves embedded NULs + UTF-8)
//
// Runs in the OFF build path (no gRPC needed).

#include "control_plane/config_version_record.h"
#include "storage/sqlite_persistent_store.h"
#include <gtest/gtest.h>
#include <string>

namespace aegisgate {
namespace {

static ConfigVersionRecord makeRec(const std::string& id,
                                    ConfigStatus status,
                                    std::int64_t submitted_at,
                                    const std::string& submitter = "alice") {
    ConfigVersionRecord r{};
    r.version_id = id;
    r.content_sha256 = "sha-" + id;
    r.yaml_content = "server:\n  port: 8080\n";
    r.size_bytes = static_cast<std::int64_t>(r.yaml_content.size());
    r.status = status;
    r.submitter = submitter;
    r.submitter_comment = "initial";
    r.submitted_at = submitted_at;
    return r;
}

class SqliteConfigVersionTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<SQLitePersistentStore>(":memory:");
        ASSERT_TRUE(store_->initialize());
    }
    void TearDown() override { store_->close(); }
    std::unique_ptr<SQLitePersistentStore> store_;
};

TEST_F(SqliteConfigVersionTest, InsertAndGetRoundtripAllFields) {
    ConfigVersionRecord r = makeRec("01J8A00000000000000000001",
                                     ConfigStatus::PENDING, 1000);
    r.submitter_comment = "initial bundle";
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
    EXPECT_EQ(got->chain_hash, "abcdef");
}

TEST_F(SqliteConfigVersionTest, YamlContentPreservesBinaryBytes) {
    // Embedded NUL + high-bit bytes must survive a BLOB roundtrip.
    ConfigVersionRecord r = makeRec("01J8A00000000000000000001",
                                     ConfigStatus::PENDING, 1000);
    r.yaml_content = std::string("key: \x00value\xff\xfe", 14);
    r.size_bytes = 14;
    ASSERT_TRUE(store_->insertConfigVersion(r));
    auto got = store_->getConfigVersion("01J8A00000000000000000001");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->yaml_content.size(), 14u);
    EXPECT_EQ(got->yaml_content, r.yaml_content);
}

TEST_F(SqliteConfigVersionTest, DuplicateVersionIdRejected) {
    auto r = makeRec("01J8A00000000000000000001", ConfigStatus::PENDING, 1000);
    ASSERT_TRUE(store_->insertConfigVersion(r));
    r.submitter = "mallory";
    EXPECT_FALSE(store_->insertConfigVersion(r));
    auto got = store_->getConfigVersion("01J8A00000000000000000001");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->submitter, "alice");
}

TEST_F(SqliteConfigVersionTest, GetMissingReturnsNullopt) {
    EXPECT_FALSE(store_->getConfigVersion("none").has_value());
}

TEST_F(SqliteConfigVersionTest, ListDefaultOrderBySubmittedAtDesc) {
    store_->insertConfigVersion(makeRec("01J8A00000000000000000001",
                                         ConfigStatus::PENDING, 1000));
    store_->insertConfigVersion(makeRec("01J8A00000000000000000002",
                                         ConfigStatus::PENDING, 3000));
    store_->insertConfigVersion(makeRec("01J8A00000000000000000003",
                                         ConfigStatus::PENDING, 2000));
    auto out = store_->listConfigVersions({});
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].version_id, "01J8A00000000000000000002");
    EXPECT_EQ(out[1].version_id, "01J8A00000000000000000003");
    EXPECT_EQ(out[2].version_id, "01J8A00000000000000000001");
}

TEST_F(SqliteConfigVersionTest, ListFilterByStatus) {
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
    EXPECT_EQ(out[0].version_id, "01J8A00000000000000000003");
    EXPECT_EQ(out[1].version_id, "01J8A00000000000000000001");
}

TEST_F(SqliteConfigVersionTest, ListFilterBySinceMillis) {
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
    EXPECT_EQ(out[0].version_id, "01J8A00000000000000000003");
    EXPECT_EQ(out[1].version_id, "01J8A00000000000000000002");
}

TEST_F(SqliteConfigVersionTest, ListPaging) {
    for (int i = 1; i <= 5; ++i) {
        char id[32];
        snprintf(id, sizeof(id), "01J8A0000000000000000000%d", i);
        store_->insertConfigVersion(makeRec(id, ConfigStatus::PENDING, i * 1000));
    }
    ConfigVersionQuery q;
    q.limit = 2;
    auto p1 = store_->listConfigVersions(q);
    ASSERT_EQ(p1.size(), 2u);
    EXPECT_EQ(p1[0].version_id, "01J8A00000000000000000005");
    EXPECT_EQ(p1[1].version_id, "01J8A00000000000000000004");
    q.page_token = p1.back().version_id;
    auto p2 = store_->listConfigVersions(q);
    ASSERT_EQ(p2.size(), 2u);
    EXPECT_EQ(p2[0].version_id, "01J8A00000000000000000003");
    EXPECT_EQ(p2[1].version_id, "01J8A00000000000000000002");
}

TEST_F(SqliteConfigVersionTest, UpdateStatusWritesReviewerFields) {
    store_->insertConfigVersion(makeRec("01J8A00000000000000000001",
                                         ConfigStatus::PENDING, 1000));
    ASSERT_TRUE(store_->updateConfigStatus(
        "01J8A00000000000000000001", ConfigStatus::APPROVED,
        "bob", "LGTM", 2500));
    auto got = store_->getConfigVersion("01J8A00000000000000000001");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, ConfigStatus::APPROVED);
    EXPECT_EQ(got->reviewer, "bob");
    EXPECT_EQ(got->reviewer_comment, "LGTM");
    EXPECT_EQ(got->reviewed_at, 2500);
}

TEST_F(SqliteConfigVersionTest, UpdateStatusMissingReturnsFalse) {
    EXPECT_FALSE(store_->updateConfigStatus(
        "missing", ConfigStatus::APPROVED, "bob", "", 0));
}

TEST_F(SqliteConfigVersionTest, ActivateApprovedBecomesActive) {
    store_->insertConfigVersion(makeRec("01J8A00000000000000000001",
                                         ConfigStatus::APPROVED, 1000));
    EXPECT_FALSE(store_->getActiveConfig().has_value());
    ASSERT_TRUE(store_->activateConfig(
        "01J8A00000000000000000001", "carol", 2000));
    auto a = store_->getActiveConfig();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->status, ConfigStatus::ACTIVE);
    EXPECT_EQ(a->activator, "carol");
    EXPECT_EQ(a->activated_at, 2000);
}

TEST_F(SqliteConfigVersionTest, ActivateSupersedesPrevious) {
    store_->insertConfigVersion(makeRec("01J8A00000000000000000001",
                                         ConfigStatus::APPROVED, 1000));
    store_->activateConfig("01J8A00000000000000000001", "carol", 1500);
    store_->insertConfigVersion(makeRec("01J8A00000000000000000002",
                                         ConfigStatus::APPROVED, 2000));
    ASSERT_TRUE(store_->activateConfig(
        "01J8A00000000000000000002", "dave", 2500));

    auto prev = store_->getConfigVersion("01J8A00000000000000000001");
    ASSERT_TRUE(prev.has_value());
    EXPECT_EQ(prev->status, ConfigStatus::SUPERSEDED);
    EXPECT_EQ(prev->deactivated_at, 2500);

    auto a = store_->getActiveConfig();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->version_id, "01J8A00000000000000000002");
}

TEST_F(SqliteConfigVersionTest, AtMostOneActiveAfterMultipleActivations) {
    for (int i = 1; i <= 4; ++i) {
        char id[32];
        snprintf(id, sizeof(id), "01J8A0000000000000000000%d", i);
        store_->insertConfigVersion(
            makeRec(id, ConfigStatus::APPROVED, i * 1000));
    }
    store_->activateConfig("01J8A00000000000000000001", "a", 1500);
    store_->activateConfig("01J8A00000000000000000002", "a", 2500);
    store_->activateConfig("01J8A00000000000000000003", "a", 3500);
    store_->activateConfig("01J8A00000000000000000004", "a", 4500);

    ConfigVersionQuery q;
    q.statuses = {ConfigStatus::ACTIVE};
    auto active_rows = store_->listConfigVersions(q);
    EXPECT_EQ(active_rows.size(), 1u);
    EXPECT_EQ(active_rows[0].version_id, "01J8A00000000000000000004");

    q.statuses = {ConfigStatus::SUPERSEDED};
    auto superseded_rows = store_->listConfigVersions(q);
    EXPECT_EQ(superseded_rows.size(), 3u);
}

TEST_F(SqliteConfigVersionTest, R2RollbackFromSupersededToActive) {
    store_->insertConfigVersion(makeRec("01J8A00000000000000000001",
                                         ConfigStatus::APPROVED, 1000));
    store_->activateConfig("01J8A00000000000000000001", "carol", 1500);
    store_->insertConfigVersion(makeRec("01J8A00000000000000000002",
                                         ConfigStatus::APPROVED, 2000));
    store_->activateConfig("01J8A00000000000000000002", "dave", 2500);
    // v1 is SUPERSEDED, roll back.
    ASSERT_TRUE(store_->activateConfig(
        "01J8A00000000000000000001", "erin", 3000));
    auto a = store_->getActiveConfig();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->version_id, "01J8A00000000000000000001");
}

TEST_F(SqliteConfigVersionTest, ActivateRejectedFails) {
    store_->insertConfigVersion(makeRec("01J8A00000000000000000001",
                                         ConfigStatus::REJECTED, 1000));
    EXPECT_FALSE(store_->activateConfig(
        "01J8A00000000000000000001", "carol", 2000));
    EXPECT_FALSE(store_->getActiveConfig().has_value());
}

TEST_F(SqliteConfigVersionTest, ActivateMissingFails) {
    EXPECT_FALSE(store_->activateConfig("missing", "carol", 0));
}

TEST_F(SqliteConfigVersionTest, ActivateCurrentActiveIdempotent) {
    store_->insertConfigVersion(makeRec("01J8A00000000000000000001",
                                         ConfigStatus::APPROVED, 1000));
    store_->activateConfig("01J8A00000000000000000001", "carol", 1500);
    EXPECT_TRUE(store_->activateConfig(
        "01J8A00000000000000000001", "carol", 9999));
    auto got = store_->getConfigVersion("01J8A00000000000000000001");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, ConfigStatus::ACTIVE);
    EXPECT_EQ(got->activated_at, 1500);  // original activation preserved
}

} // namespace
} // namespace aegisgate
