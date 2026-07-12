// TASK-20260617-02 Epic 3 — CostTracker startup reload from persistent store.
// Verifies loadFromStore rebuilds records_ (so dashboard cost summaries survive a
// restart), honoring the retention window and the cap (most-recent-N) bound.
#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

#include "observe/cost_tracker.h"
#include "storage/memory_persistent_store.h"

using namespace aegisgate;

namespace {

std::string isoOffsetDays(int days_ago) {
    auto tp = std::chrono::system_clock::now() - std::chrono::hours(24 * days_ago);
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

CostRecord mkCost(const std::string& id, const std::string& tenant,
                  const std::string& model, double cost,
                  const std::string& ts) {
    CostRecord r;
    r.request_id = id;
    r.tenant_id = tenant;
    r.model = model;
    r.input_tokens = 100;
    r.output_tokens = 50;
    r.total_cost = cost;
    r.timestamp = ts;
    return r;
}

}  // namespace

TEST(CostTrackerReload, ReplaysRecordsWithinWindow) {
    MemoryPersistentStore store;
    ASSERT_TRUE(store.initialize());
    ASSERT_TRUE(store.insertCostRecord(mkCost("r1", "t1", "gpt-4", 1.0, isoOffsetDays(2))));
    ASSERT_TRUE(store.insertCostRecord(mkCost("r2", "t1", "gpt-4", 2.0, isoOffsetDays(5))));
    // Outside a 30-day window → must be excluded.
    ASSERT_TRUE(store.insertCostRecord(mkCost("r3", "t2", "gpt-3.5", 4.0, isoOffsetDays(100))));

    CostTracker ct;
    ct.setPersistentStore(&store);
    ct.loadFromStore(30, 0);

    auto total = ct.totalSummary();
    EXPECT_EQ(total.request_count, 2);
    EXPECT_NEAR(total.total_cost, 3.0, 1e-9);
    EXPECT_EQ(ct.records().size(), 2u);
}

TEST(CostTrackerReload, HonorsCapKeepingMostRecent) {
    MemoryPersistentStore store;
    ASSERT_TRUE(store.initialize());
    ASSERT_TRUE(store.insertCostRecord(mkCost("old", "t1", "gpt-4", 1.0, isoOffsetDays(9))));
    ASSERT_TRUE(store.insertCostRecord(mkCost("mid", "t1", "gpt-4", 2.0, isoOffsetDays(5))));
    ASSERT_TRUE(store.insertCostRecord(mkCost("new", "t1", "gpt-4", 4.0, isoOffsetDays(1))));

    CostTracker ct;
    ct.setPersistentStore(&store);
    ct.loadFromStore(3650, /*cap=*/2);

    ASSERT_EQ(ct.records().size(), 2u);
    // ASC ordering → cap keeps the two most recent (mid, new).
    EXPECT_EQ(ct.records()[0].request_id, "mid");
    EXPECT_EQ(ct.records()[1].request_id, "new");
}

TEST(CostTrackerReload, NoStoreIsNoop) {
    CostTracker ct;
    ct.loadFromStore(30, 0);  // no store set → must not crash, no records.
    EXPECT_EQ(ct.records().size(), 0u);
}
