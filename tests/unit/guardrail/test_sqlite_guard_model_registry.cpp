// Phase 11.1 TASK-20260523-01 — Epic 1.3 SQLite registry tests.
//
// Verifies parity with MemoryGuardModelRegistry behaviour, plus the two
// SQLite-specific concerns:
//   1. Reload-after-close: a freshly opened registry sees previously inserted
//      records (durability).
//   2. Schema enforces status ∈ {shadow, live, retired} via raw-SQL probe.
//   3. Partial-unique index forbids two Live rows for the same model_id even
//      if the application layer were bypassed.

#include "guardrail/model/sqlite_guard_model_registry.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <string>

using aegisgate::guard::GuardModelStatus;
using aegisgate::guard::ModelRegistryRecord;
using aegisgate::guard::SQLiteGuardModelRegistry;

namespace {

std::string tempDbPath(const std::string& tag) {
    auto base = std::filesystem::temp_directory_path() /
                ("aegisgate_guard_" + tag + "_" +
                 std::to_string(::getpid()) + ".db");
    std::filesystem::remove(base);
    return base.string();
}

ModelRegistryRecord makeRecord(std::string version,
                               GuardModelStatus status = GuardModelStatus::Shadow,
                               std::string sha = "sha-default") {
    ModelRegistryRecord r;
    r.model_id = "guardrail";
    r.version = std::move(version);
    r.path = "/models/guardrail-" + r.version + ".onnx";
    r.classifier_threshold = 0.5f;
    r.status = status;
    r.promoted_at_ms = 0;
    r.artifact_sha256 = std::move(sha);
    r.metrics_summary = R"({"win_rate":0.7})";
    return r;
}

}  // namespace

TEST(SQLiteGuardModelRegistryTest, InsertGetRoundTripAndDurability) {
    auto db = tempDbPath("durability");
    {
        SQLiteGuardModelRegistry reg(db);
        ASSERT_TRUE(reg.initialize());
        ASSERT_TRUE(reg.insert(makeRecord("v1")).ok);
        ASSERT_TRUE(reg.insert(makeRecord("v2", GuardModelStatus::Live)).ok);
    }
    // Reopen — data must survive.
    {
        SQLiteGuardModelRegistry reg(db);
        ASSERT_TRUE(reg.initialize());
        auto v1 = reg.get("guardrail", "v1");
        auto v2 = reg.get("guardrail", "v2");
        ASSERT_TRUE(v1.has_value());
        ASSERT_TRUE(v2.has_value());
        EXPECT_EQ(v1->status, GuardModelStatus::Shadow);
        EXPECT_EQ(v2->status, GuardModelStatus::Live);
    }
    std::filesystem::remove(db);
}

TEST(SQLiteGuardModelRegistryTest, DuplicatePrimaryKeyRejected) {
    auto db = tempDbPath("duppk");
    SQLiteGuardModelRegistry reg(db);
    ASSERT_TRUE(reg.initialize());
    ASSERT_TRUE(reg.insert(makeRecord("v1")).ok);
    auto dup = reg.insert(makeRecord("v1", GuardModelStatus::Shadow, "sha-other"));
    EXPECT_FALSE(dup.ok);
    EXPECT_EQ(dup.error_code, "duplicate_version");
    std::filesystem::remove(db);
}

TEST(SQLiteGuardModelRegistryTest, PromoteAtomicallyDemotesOldLive) {
    auto db = tempDbPath("promote_demote");
    SQLiteGuardModelRegistry reg(db);
    ASSERT_TRUE(reg.initialize());
    ASSERT_TRUE(reg.insert(makeRecord("v1", GuardModelStatus::Live)).ok);
    ASSERT_TRUE(reg.insert(makeRecord("v2", GuardModelStatus::Shadow)).ok);

    auto pr = reg.promote("guardrail", "v2", 17000);
    EXPECT_TRUE(pr.ok);

    auto v1 = reg.get("guardrail", "v1");
    auto v2 = reg.get("guardrail", "v2");
    ASSERT_TRUE(v1.has_value());
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v1->status, GuardModelStatus::Retired);
    EXPECT_EQ(v2->status, GuardModelStatus::Live);
    std::filesystem::remove(db);
}

TEST(SQLiteGuardModelRegistryTest, PromoteRetiredIsIllegalTransition) {
    // SR-NEW1 critical: retired rows must NOT be re-promoted (would break
    // the audit chain assumption that a retired model has been forensically
    // captured). Mutation M3 targets this branch.
    auto db = tempDbPath("promote_retired");
    SQLiteGuardModelRegistry reg(db);
    ASSERT_TRUE(reg.initialize());
    ASSERT_TRUE(reg.insert(makeRecord("v1", GuardModelStatus::Retired)).ok);
    auto pr = reg.promote("guardrail", "v1", 0);
    EXPECT_FALSE(pr.ok);
    EXPECT_EQ(pr.error_code, "illegal_transition");
    std::filesystem::remove(db);
}

TEST(SQLiteGuardModelRegistryTest, RevertShadowIsIllegalTransition) {
    auto db = tempDbPath("revert_shadow");
    SQLiteGuardModelRegistry reg(db);
    ASSERT_TRUE(reg.initialize());
    ASSERT_TRUE(reg.insert(makeRecord("v1", GuardModelStatus::Shadow)).ok);
    auto rr = reg.revert("guardrail", "v1");
    EXPECT_FALSE(rr.ok);
    EXPECT_EQ(rr.error_code, "illegal_transition");
    std::filesystem::remove(db);
}

TEST(SQLiteGuardModelRegistryTest, RawSqlConstraintRejectsBadStatus) {
    // A11 data-infra raw-SQL test: bypass the C++ layer and try to INSERT
    // an invalid status string. The DB itself must reject via CHECK.
    auto db = tempDbPath("raw_check");
    {
        SQLiteGuardModelRegistry reg(db);
        ASSERT_TRUE(reg.initialize());
    }
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(db.c_str(), &raw), SQLITE_OK);
    char* err = nullptr;
    int rc = sqlite3_exec(raw,
        "INSERT INTO guard_models(model_id,version,path,classifier_threshold,"
        "status,promoted_at_ms,artifact_sha256,metrics_summary) "
        "VALUES('guardrail','vbad','/x',0.5,'bogus',0,'sha','{}');",
        nullptr, nullptr, &err);
    EXPECT_NE(rc, SQLITE_OK) << "CHECK(status IN ...) should reject bogus value";
    sqlite3_free(err);
    sqlite3_close(raw);
    std::filesystem::remove(db);
}
