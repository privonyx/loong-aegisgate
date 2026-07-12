// TASK-20260617-02 Epic 1 — savings_events persistence (memory + sqlite).
// Verifies insert / date-range query (ordered) / count / prune across the
// two durable-capable backends so the dashboard-persistence reload path has a
// faithful, cross-backend-consistent contract.
#include "storage/memory_persistent_store.h"
#include "storage/sqlite_persistent_store.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

using namespace aegisgate;
using SavingsEventRecord = PersistentStore::SavingsEventRecord;

namespace {

SavingsEventRecord mkEv(int type, const std::string& model,
                        const std::string& tenant, int toks, double cost,
                        const std::string& ts, bool fallback = false) {
    SavingsEventRecord e;
    e.type = type;
    e.model = model;
    e.tenant_id = tenant;
    e.tokens_saved = toks;
    e.cost_saved = cost;
    e.fallback_pricing = fallback;
    e.timestamp = ts;
    return e;
}

// Shared roundtrip body so memory + sqlite are held to the same contract.
void runRoundtrip(PersistentStore& s) {
    ASSERT_TRUE(s.insertSavingsEvent(
        mkEv(0, "gpt-4", "t1", 100, 0.50, "2026-06-10T00:00:00Z")));
    ASSERT_TRUE(s.insertSavingsEvent(
        mkEv(1, "gpt-4", "t2", 50, 0.20, "2026-06-15T00:00:00Z", true)));
    ASSERT_TRUE(s.insertSavingsEvent(
        mkEv(2, "gpt-3.5", "t1", 10, 0.05, "2026-05-01T00:00:00Z")));

    EXPECT_EQ(s.savingsEventCount(), 3);

    // June window excludes the May event → 2 rows, ascending by timestamp.
    auto rows = s.querySavingsEventsByDateRange(
        "2026-06-01T00:00:00Z", "2026-06-30T23:59:59Z", 100000);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].timestamp, "2026-06-10T00:00:00Z");
    EXPECT_EQ(rows[0].type, 0);
    EXPECT_EQ(rows[0].model, "gpt-4");
    EXPECT_EQ(rows[0].tenant_id, "t1");
    EXPECT_EQ(rows[0].tokens_saved, 100);
    EXPECT_DOUBLE_EQ(rows[0].cost_saved, 0.50);
    EXPECT_FALSE(rows[0].fallback_pricing);
    EXPECT_EQ(rows[1].timestamp, "2026-06-15T00:00:00Z");
    EXPECT_EQ(rows[1].type, 1);
    EXPECT_TRUE(rows[1].fallback_pricing);

    // Limit is honored.
    auto capped = s.querySavingsEventsByDateRange(
        "2026-01-01T00:00:00Z", "2026-12-31T23:59:59Z", 1);
    EXPECT_EQ(capped.size(), 1u);
}

// Prune body: an ancient row must be pruned by a 30-day retention; a far-future
// row must survive.
void runPrune(PersistentStore& s) {
    ASSERT_TRUE(s.insertSavingsEvent(
        mkEv(0, "gpt-4", "t1", 100, 0.50, "2020-01-01T00:00:00Z")));
    ASSERT_TRUE(s.insertSavingsEvent(
        mkEv(0, "gpt-4", "t1", 100, 0.50, "2099-01-01T00:00:00Z")));
    int64_t removed = s.pruneSavingsEvents(30);
    EXPECT_EQ(removed, 1);
    EXPECT_EQ(s.savingsEventCount(), 1);
}

std::string tempDbPath(const char* name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove(p);
    return p.string();
}

}  // namespace

TEST(SavingsEventsStore, MemoryRoundtrip) {
    MemoryPersistentStore s;
    ASSERT_TRUE(s.initialize());
    runRoundtrip(s);
}

TEST(SavingsEventsStore, SqliteRoundtrip) {
    auto path = tempDbPath("savings_events_roundtrip.db");
    SQLitePersistentStore s(path, true);
    ASSERT_TRUE(s.initialize());
    runRoundtrip(s);
    s.close();
    std::filesystem::remove(path);
}

TEST(SavingsEventsStore, MemoryPrune) {
    MemoryPersistentStore s;
    ASSERT_TRUE(s.initialize());
    runPrune(s);
}

TEST(SavingsEventsStore, SqlitePrune) {
    auto path = tempDbPath("savings_events_prune.db");
    SQLitePersistentStore s(path, true);
    ASSERT_TRUE(s.initialize());
    runPrune(s);
    s.close();
    std::filesystem::remove(path);
}
