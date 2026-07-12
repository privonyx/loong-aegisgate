// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic A.4 + A.5 —
// rollout CRUD coverage for every persistent backend that participates in
// the MVP (Memory + SQLite; PG stays on no-op defaults by design).
//
// The shared surface is verified via a TYPED_TEST_SUITE instantiated for
// both backends so invariants (active-at-most-one, ordering, pagination,
// event isolation) cannot diverge between implementations. A small number
// of backend-specific TESTs sit at the bottom (SQLite schema migration
// and DB-level UNIQUE INDEX).

#include "control_plane/rollout/rollout_record.h"
#include "storage/memory_persistent_store.h"
#include "storage/sqlite_persistent_store.h"
#include <gtest/gtest.h>
#include <sqlite3.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

#if defined(AEGISGATE_TEST_PG)
#include "storage/pg_persistent_store.h"
#include <libpq-fe.h>
#endif

namespace aegisgate {
namespace {

// --------- Helpers ---------------------------------------------------------

static RolloutRecord makeRollout(const std::string& id,
                                   const std::string& target,
                                   RolloutStatus status = RolloutStatus::PENDING,
                                   std::int64_t started_at = 0) {
    RolloutRecord r{};
    r.rollout_id = id;
    r.target_version_id = target;
    r.previous_active_version_id = "";
    r.spec.target_version_id = target;
    r.spec.sticky_key = "tenant_id";
    r.spec.auto_rollback_on_pause = true;
    r.spec.auto_rollback_grace_seconds = 1800;
    RolloutStageRecord s;
    s.name = "1pct";
    s.scope.percentage = 1;
    r.spec.stages.push_back(std::move(s));
    r.status = status;
    r.started_at = started_at;
    r.creator = "alice";
    r.last_actor = "alice";
    return r;
}

static RolloutStageEvent makeEvent(const std::string& id,
                                    const std::string& rollout_id,
                                    int stage_index,
                                    const std::string& type,
                                    std::int64_t at_millis) {
    RolloutStageEvent e{};
    e.event_id = id;
    e.rollout_id = rollout_id;
    e.stage_index = stage_index;
    e.event_type = type;
    e.at_millis = at_millis;
    e.actor = "alice";
    return e;
}

// --------- Backend factories -----------------------------------------------

struct MemoryFactory {
    using StoreType = MemoryPersistentStore;
    static std::unique_ptr<PersistentStore> create(std::string& out_path) {
        out_path.clear();
        auto m = std::make_unique<MemoryPersistentStore>();
        EXPECT_TRUE(m->initialize());
        return m;
    }
    static void teardown(const std::string&) {}
};

struct SqliteFactory {
    using StoreType = SQLitePersistentStore;
    static std::unique_ptr<PersistentStore> create(std::string& out_path) {
        // Use unique temp file per fixture instance so parallel ctest is safe.
        auto tmpdir = std::filesystem::temp_directory_path();
        char name[] = "rollout_test_XXXXXX";
        // mkstemp requires a writable buffer with trailing XXXXXX in cwd;
        // build an absolute path manually to avoid tmp dir ownership issues.
        std::string path = (tmpdir / name).string();
        int fd = ::mkstemp(path.data());
        if (fd >= 0) ::close(fd);
        ::unlink(path.c_str());
        out_path = path;
        auto s = std::make_unique<SQLitePersistentStore>(path, /*wal=*/false);
        EXPECT_TRUE(s->initialize());
        return s;
    }
    static void teardown(const std::string& path) {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            std::filesystem::remove(path + "-wal", ec);
            std::filesystem::remove(path + "-shm", ec);
            std::filesystem::remove(path + "-journal", ec);
        }
    }
};

#if defined(AEGISGATE_TEST_PG)
// PG factory — activated by the CMake `if(ENABLE_PG)` block setting
// AEGISGATE_TEST_PG=1. Requires the AEGISGATE_PG_URL env var; create()
// returns nullptr when it's missing so SetUp() can call GTEST_SKIP().
//
// Cleanup is best-effort and bound to a fixed id prefix ('R'/'V_*') used by
// makeRollout()/makeEvent() so other tests sharing the database aren't
// disturbed.
struct PgFactory {
    using StoreType = PgPersistentStore;
    static std::unique_ptr<PersistentStore> create(std::string& out_path) {
        out_path.clear();
        const char* url = std::getenv("AEGISGATE_PG_URL");
        if (!url) return nullptr;  // SetUp will GTEST_SKIP()
        // Best-effort cleanup of leftover rows from previous test runs.
        // Ids match makeRollout()'s 'R<n>' / V<n> conventions plus a few
        // typed-test-specific patterns ("V_other", "V_RACE", "V_BIG").
        PGconn* conn = PQconnectdb(url);
        if (PQstatus(conn) == CONNECTION_OK) {
            PQclear(PQexec(conn,
                "DELETE FROM rollout_stage_events WHERE "
                "rollout_id ~ '^R[0-9]+$' OR rollout_id LIKE 'pg-typed-%'"));
            PQclear(PQexec(conn,
                "DELETE FROM rollouts WHERE "
                "rollout_id ~ '^R[0-9]+$' OR rollout_id LIKE 'pg-typed-%'"));
        }
        PQfinish(conn);

        PgConfig cfg;
        cfg.url = url;
        cfg.pool_size = 2;
        auto pg = std::make_unique<PgPersistentStore>(cfg);
        if (!pg->initialize()) return nullptr;
        return pg;
    }
    static void teardown(const std::string&) {
        // No-op; cleanup runs at the next create() so failing tests still
        // leave debugging traces in the table.
    }
};
#endif

template <typename Factory>
class RolloutStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = Factory::create(db_path_);
        if (!store_) {
            GTEST_SKIP() << "backend not available "
                            "(AEGISGATE_PG_URL unset or PG unreachable)";
        }
    }
    void TearDown() override {
        store_.reset();
        Factory::teardown(db_path_);
    }
    std::unique_ptr<PersistentStore> store_;
    std::string db_path_;
};

#if defined(AEGISGATE_TEST_PG)
using Backends = ::testing::Types<MemoryFactory, SqliteFactory, PgFactory>;
#else
using Backends = ::testing::Types<MemoryFactory, SqliteFactory>;
#endif
TYPED_TEST_SUITE(RolloutStoreTest, Backends);

// --------- Core CRUD -------------------------------------------------------

TYPED_TEST(RolloutStoreTest, InsertAndGetRolloutRoundtripAllFields) {
    auto r = makeRollout("R1", "V1", RolloutStatus::PENDING, 0);
    r.spec.creator_comment = "canary launch";
    r.chain_hash = "deadbeef";
    r.spec.stages[0].scope.tenant_globs = {"internal-*", "staff-*"};
    r.spec.stages[0].scope.regions = {"ap-east-1"};
    r.spec.stages[0].observation.min_duration_seconds = 600;
    r.spec.stages[0].observation.min_sample_count = 1000;
    r.spec.stages[0].auto_pause.error_rate_gt = 0.05;
    r.spec.stages[0].auto_pause.absolute_p99_latency_ms_gt = 5000.0;
    ASSERT_TRUE(this->store_->insertRollout(r));

    auto got = this->store_->getRollout("R1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->rollout_id, "R1");
    EXPECT_EQ(got->target_version_id, "V1");
    EXPECT_EQ(got->status, RolloutStatus::PENDING);
    EXPECT_EQ(got->spec.sticky_key, "tenant_id");
    EXPECT_EQ(got->spec.auto_rollback_grace_seconds, 1800);
    ASSERT_EQ(got->spec.stages.size(), 1u);
    EXPECT_EQ(got->spec.stages[0].name, "1pct");
    EXPECT_EQ(got->spec.stages[0].scope.percentage, 1);
    ASSERT_EQ(got->spec.stages[0].scope.tenant_globs.size(), 2u);
    EXPECT_EQ(got->spec.stages[0].scope.tenant_globs[0], "internal-*");
    EXPECT_EQ(got->spec.stages[0].scope.regions[0], "ap-east-1");
    EXPECT_EQ(got->spec.stages[0].observation.min_duration_seconds, 600);
    EXPECT_EQ(got->spec.stages[0].observation.min_sample_count, 1000);
    EXPECT_DOUBLE_EQ(got->spec.stages[0].auto_pause.error_rate_gt, 0.05);
    EXPECT_DOUBLE_EQ(got->spec.stages[0].auto_pause.absolute_p99_latency_ms_gt, 5000.0);
    EXPECT_EQ(got->spec.creator_comment, "canary launch");
    EXPECT_EQ(got->chain_hash, "deadbeef");
}

TYPED_TEST(RolloutStoreTest, GetMissingRolloutReturnsNullopt) {
    EXPECT_FALSE(this->store_->getRollout("nope").has_value());
}

TYPED_TEST(RolloutStoreTest, InsertDuplicateRolloutIdRejected) {
    ASSERT_TRUE(this->store_->insertRollout(
        makeRollout("R1", "V1", RolloutStatus::PENDING)));
    EXPECT_FALSE(this->store_->insertRollout(
        makeRollout("R1", "V2", RolloutStatus::PENDING)));
    auto got = this->store_->getRollout("R1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->target_version_id, "V1");
}

TYPED_TEST(RolloutStoreTest, UpdateRolloutPersistsNewFields) {
    ASSERT_TRUE(this->store_->insertRollout(
        makeRollout("R1", "V1", RolloutStatus::PENDING)));

    auto mutated = this->store_->getRollout("R1").value();
    mutated.status = RolloutStatus::PROGRESSING;
    mutated.current_stage_index = 1;
    mutated.started_at = 12345;
    mutated.stage_started_at = 12345;
    mutated.last_actor = "bob";
    mutated.pause_reason = PauseReason::UNSPECIFIED;
    ASSERT_TRUE(this->store_->updateRollout(mutated));

    auto got = this->store_->getRollout("R1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, RolloutStatus::PROGRESSING);
    EXPECT_EQ(got->current_stage_index, 1);
    EXPECT_EQ(got->started_at, 12345);
    EXPECT_EQ(got->last_actor, "bob");
}

TYPED_TEST(RolloutStoreTest, UpdateMissingRolloutFails) {
    auto r = makeRollout("nope", "V1");
    EXPECT_FALSE(this->store_->updateRollout(r));
}

// --------- Active rollout invariant ----------------------------------------

TYPED_TEST(RolloutStoreTest, FindActiveRolloutByTargetReturnsActive) {
    auto r1 = makeRollout("R1", "V1", RolloutStatus::PROGRESSING, 1000);
    ASSERT_TRUE(this->store_->insertRollout(r1));
    auto found = this->store_->findActiveRolloutByTarget("V1");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->rollout_id, "R1");
    EXPECT_FALSE(this->store_->findActiveRolloutByTarget("V2").has_value());
}

TYPED_TEST(RolloutStoreTest, InsertSecondActiveOnSameTargetRejected) {
    ASSERT_TRUE(this->store_->insertRollout(
        makeRollout("R1", "V1", RolloutStatus::PROGRESSING)));
    EXPECT_FALSE(this->store_->insertRollout(
        makeRollout("R2", "V1", RolloutStatus::PROGRESSING)));
    EXPECT_FALSE(this->store_->insertRollout(
        makeRollout("R3", "V1", RolloutStatus::PENDING)));
    EXPECT_FALSE(this->store_->insertRollout(
        makeRollout("R4", "V1", RolloutStatus::PAUSED)));
}

TYPED_TEST(RolloutStoreTest, TerminalRolloutsDoNotBlockNewActiveOnSameTarget) {
    for (auto status : {RolloutStatus::COMPLETED,
                        RolloutStatus::FAILED,
                        RolloutStatus::ABORTED}) {
        // Fresh store per iteration so previous R_old terminal records don't
        // accumulate on the SQLite tmp file either.
        this->store_ = TypeParam::create(this->db_path_);
        auto old = makeRollout("R_old", "V1", status, 1000);
        ASSERT_TRUE(this->store_->insertRollout(old));
        EXPECT_TRUE(this->store_->insertRollout(
            makeRollout("R_new", "V1", RolloutStatus::PENDING, 2000)))
            << "Terminal status " << rolloutStatusToString(status)
            << " should not block new active rollout on same target";
    }
}

TYPED_TEST(RolloutStoreTest, UpdateToTerminalReleasesActiveSlot) {
    ASSERT_TRUE(this->store_->insertRollout(
        makeRollout("R1", "V1", RolloutStatus::PROGRESSING)));
    auto r1 = this->store_->getRollout("R1").value();
    r1.status = RolloutStatus::COMPLETED;
    r1.completed_at = 5000;
    ASSERT_TRUE(this->store_->updateRollout(r1));
    EXPECT_FALSE(this->store_->findActiveRolloutByTarget("V1").has_value());

    EXPECT_TRUE(this->store_->insertRollout(
        makeRollout("R2", "V1", RolloutStatus::PENDING, 6000)));
}

// --------- Listing ---------------------------------------------------------

TYPED_TEST(RolloutStoreTest, ListRolloutsOrderedByStartedAtDesc) {
    ASSERT_TRUE(this->store_->insertRollout(
        makeRollout("R1", "V1", RolloutStatus::COMPLETED, 1000)));
    ASSERT_TRUE(this->store_->insertRollout(
        makeRollout("R2", "V2", RolloutStatus::COMPLETED, 3000)));
    ASSERT_TRUE(this->store_->insertRollout(
        makeRollout("R3", "V3", RolloutStatus::COMPLETED, 2000)));

    auto out = this->store_->listRollouts({});
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].rollout_id, "R2");
    EXPECT_EQ(out[1].rollout_id, "R3");
    EXPECT_EQ(out[2].rollout_id, "R1");
}

TYPED_TEST(RolloutStoreTest, ListRolloutsFilterByStatus) {
    ASSERT_TRUE(this->store_->insertRollout(
        makeRollout("R1", "V1", RolloutStatus::PENDING, 1000)));
    ASSERT_TRUE(this->store_->insertRollout(
        makeRollout("R2", "V2", RolloutStatus::PROGRESSING, 2000)));
    ASSERT_TRUE(this->store_->insertRollout(
        makeRollout("R3", "V3", RolloutStatus::COMPLETED, 3000)));

    RolloutQuery q;
    q.statuses = {RolloutStatus::PENDING, RolloutStatus::PROGRESSING};
    auto out = this->store_->listRollouts(q);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].rollout_id, "R2");
    EXPECT_EQ(out[1].rollout_id, "R1");
}

TYPED_TEST(RolloutStoreTest, ListRolloutsRespectsLimitAndPageToken) {
    for (int i = 1; i <= 5; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "R%d", i);
        ASSERT_TRUE(this->store_->insertRollout(
            makeRollout(id, "V" + std::to_string(i),
                         RolloutStatus::COMPLETED,
                         static_cast<std::int64_t>(i) * 1000)));
    }
    RolloutQuery q;
    q.limit = 2;
    auto page1 = this->store_->listRollouts(q);
    ASSERT_EQ(page1.size(), 2u);
    EXPECT_EQ(page1[0].rollout_id, "R5");
    EXPECT_EQ(page1[1].rollout_id, "R4");

    q.page_token = page1.back().rollout_id;
    auto page2 = this->store_->listRollouts(q);
    ASSERT_EQ(page2.size(), 2u);
    EXPECT_EQ(page2[0].rollout_id, "R3");
    EXPECT_EQ(page2[1].rollout_id, "R2");
}

// --------- Stage events ----------------------------------------------------

TYPED_TEST(RolloutStoreTest, AppendAndListStageEventsInAscendingOrder) {
    ASSERT_TRUE(this->store_->insertRollout(
        makeRollout("R1", "V1", RolloutStatus::PROGRESSING)));
    ASSERT_TRUE(this->store_->appendRolloutStageEvent(
        makeEvent("E1", "R1", 0, "entered",      1000)));
    ASSERT_TRUE(this->store_->appendRolloutStageEvent(
        makeEvent("E2", "R1", 1, "promoted",     3000)));
    ASSERT_TRUE(this->store_->appendRolloutStageEvent(
        makeEvent("E3", "R1", 1, "paused_auto",  2000)));

    auto out = this->store_->listRolloutStageEvents("R1");
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].event_id, "E1");
    EXPECT_EQ(out[1].event_id, "E3");
    EXPECT_EQ(out[2].event_id, "E2");
}

TYPED_TEST(RolloutStoreTest, StageEventsIsolatedPerRollout) {
    ASSERT_TRUE(this->store_->insertRollout(
        makeRollout("R1", "V1", RolloutStatus::PROGRESSING)));
    ASSERT_TRUE(this->store_->insertRollout(
        makeRollout("R2", "V2", RolloutStatus::PROGRESSING)));
    ASSERT_TRUE(this->store_->appendRolloutStageEvent(
        makeEvent("E1", "R1", 0, "entered", 1000)));
    ASSERT_TRUE(this->store_->appendRolloutStageEvent(
        makeEvent("E2", "R2", 0, "entered", 1000)));
    EXPECT_EQ(this->store_->listRolloutStageEvents("R1").size(), 1u);
    EXPECT_EQ(this->store_->listRolloutStageEvents("R2").size(), 1u);
    EXPECT_TRUE(this->store_->listRolloutStageEvents("RX").empty());
}

// --------- SQLite-specific tests (schema / DB-level constraints) -----------

// Tests the partial UNIQUE INDEX is actually enforced by SQLite (defense in
// depth even if application-level logic regresses). The DB must reject a
// raw INSERT of a second active rollout on the same target_version_id.
TEST(SQLiteRolloutStore, UniqueActiveRolloutIndexEnforcedAtSchemaLevel) {
    std::string path;
    auto tmpdir = std::filesystem::temp_directory_path();
    char name[] = "rollout_unique_XXXXXX";
    path = (tmpdir / name).string();
    int fd = ::mkstemp(path.data());
    if (fd >= 0) ::close(fd);
    ::unlink(path.c_str());

    {
        SQLitePersistentStore s(path, /*wal=*/false);
        ASSERT_TRUE(s.initialize());
        ASSERT_TRUE(s.insertRollout(
            makeRollout("R1", "V1", RolloutStatus::PROGRESSING, 1000)));

        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
        // Bypass the application-level invariant check; go straight to SQL.
        const char* sql =
            "INSERT INTO rollouts (rollout_id, target_version_id, "
            "previous_active_version_id, spec_json, status, "
            "current_stage_index, started_at, stage_started_at, paused_at, "
            "pause_reason, pause_detail, creator, last_actor, completed_at, "
            "chain_hash) VALUES ('R2', 'V1', '', '{}', 2, 0, 2000, 0, 0, "
            "0, '', 'mallory', '', 0, '')";
        char* err = nullptr;
        int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
        EXPECT_NE(rc, SQLITE_OK) << "schema-level UNIQUE INDEX must reject "
                                    "second active rollout on same target";
        if (err) sqlite3_free(err);
        sqlite3_close(db);
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + "-journal", ec);
}

TEST(SQLiteRolloutStore, SchemaMigrationIsIdempotent) {
    std::string path;
    auto tmpdir = std::filesystem::temp_directory_path();
    char name[] = "rollout_idem_XXXXXX";
    path = (tmpdir / name).string();
    int fd = ::mkstemp(path.data());
    if (fd >= 0) ::close(fd);
    ::unlink(path.c_str());

    {
        SQLitePersistentStore s1(path, /*wal=*/false);
        ASSERT_TRUE(s1.initialize());
        ASSERT_TRUE(s1.insertRollout(
            makeRollout("R1", "V1", RolloutStatus::PENDING, 1000)));
    }
    {
        // Re-open: createTables() must be IF NOT EXISTS and preserve data.
        SQLitePersistentStore s2(path, /*wal=*/false);
        ASSERT_TRUE(s2.initialize());
        auto got = s2.getRollout("R1");
        ASSERT_TRUE(got.has_value());
        EXPECT_EQ(got->target_version_id, "V1");
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + "-journal", ec);
}

} // namespace
} // namespace aegisgate
