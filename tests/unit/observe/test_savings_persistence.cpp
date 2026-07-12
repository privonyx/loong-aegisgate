// TASK-20260617-02 Epic 2 — SavingsAggregator write-behind persistence + reload.
// Verifies: (1) record→flush→new instance loadFromStore reconstructs snapshot;
// (2) a throwing store never crashes the flush thread / hot path (SR3);
// (3) the bounded write-behind queue drops oldest on overflow (SR4).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "observe/cost_tracker.h"
#include "observe/savings_aggregator.h"
#include "storage/memory_persistent_store.h"

using namespace aegisgate;
using namespace std::chrono_literals;

namespace {

// CostTracker holds a mutex (non-copyable) → configure in place per test.
void setupPricing(CostTracker& t) {
    t.setPricing("gpt-4", 0.03, 0.06);
    t.setPricing("gpt-3.5", 0.001, 0.002);
}

// Store whose insert blocks until released — lets the test fill the bounded
// write-behind queue deterministically to exercise drop-oldest (SR4).
class BlockingStore : public MemoryPersistentStore {
public:
    std::atomic<bool> release{false};
    std::atomic<int> insert_calls{0};
    bool insertSavingsEvent(const SavingsEventRecord& ev) override {
        insert_calls.fetch_add(1);
        while (!release.load()) std::this_thread::sleep_for(1ms);
        return MemoryPersistentStore::insertSavingsEvent(ev);
    }
};

// Store that always throws on insert — exercises flush-thread try/catch (SR3).
class ThrowingStore : public MemoryPersistentStore {
public:
    bool insertSavingsEvent(const SavingsEventRecord&) override {
        throw std::runtime_error("disk on fire");
    }
};

}  // namespace

TEST(SavingsPersistence, FlushAndReloadRoundtrip) {
    MemoryPersistentStore store;
    ASSERT_TRUE(store.initialize());
    CostTracker tracker;
    setupPricing(tracker);

    {
        SavingsAggregator agg(&tracker);
        agg.setPersistentStore(&store);
        agg.recordCacheHit("gpt-4", 100, 200, "t1");
        agg.recordCompression("gpt-4", 50, "t2");
        // Destructor performs a final drain — events land durably without a sleep.
    }
    EXPECT_EQ(store.savingsEventCount(), 2);

    // A fresh instance replays from the store and rebuilds the snapshot.
    SavingsAggregator agg2(&tracker);
    agg2.setPersistentStore(&store);
    agg2.loadFromStore(3650);
    EXPECT_EQ(agg2.eventCount(), 2u);

    auto snap = agg2.snapshot("", std::chrono::system_clock::time_point::min(),
                              std::chrono::system_clock::time_point::max());
    EXPECT_EQ(snap.total.event_count, 2);
    EXPECT_EQ(snap.total.tokens_saved, 350);  // 300 (cache) + 50 (compression)
    EXPECT_EQ(snap.by_type[static_cast<int>(SavingType::CacheHit)].event_count, 1);
    EXPECT_EQ(snap.by_type[static_cast<int>(SavingType::Compression)].event_count, 1);
    EXPECT_EQ(snap.by_tenant["t1"].event_count, 1);
    EXPECT_EQ(snap.by_tenant["t2"].event_count, 1);
}

// SR1: a tenant-scoped snapshot after reload must not leak other tenants' events.
TEST(SavingsPersistence, ReloadPreservesTenantIsolation) {
    MemoryPersistentStore store;
    ASSERT_TRUE(store.initialize());
    CostTracker tracker;
    setupPricing(tracker);
    {
        SavingsAggregator agg(&tracker);
        agg.setPersistentStore(&store);
        agg.recordCacheHit("gpt-4", 100, 0, "tenant-A");
        agg.recordCacheHit("gpt-4", 200, 0, "tenant-B");
    }
    SavingsAggregator agg2(&tracker);
    agg2.setPersistentStore(&store);
    agg2.loadFromStore(3650);

    auto a = agg2.snapshot("tenant-A", std::chrono::system_clock::time_point::min(),
                           std::chrono::system_clock::time_point::max());
    EXPECT_EQ(a.total.event_count, 1);
    EXPECT_EQ(a.total.tokens_saved, 100);
    auto b = agg2.snapshot("tenant-B", std::chrono::system_clock::time_point::min(),
                           std::chrono::system_clock::time_point::max());
    EXPECT_EQ(b.total.event_count, 1);
    EXPECT_EQ(b.total.tokens_saved, 200);
}

TEST(SavingsPersistence, ThrowingStoreNeverCrashes) {
    ThrowingStore store;
    ASSERT_TRUE(store.initialize());
    CostTracker tracker;
    setupPricing(tracker);

    SavingsAggregator agg(&tracker);
    agg.setPersistentStore(&store);
    // Hot path must stay noexcept and in-memory aggregation must still work even
    // though every async insert throws. Destructor (final drain) must not abort.
    for (int i = 0; i < 100; ++i) {
        agg.recordCacheHit("gpt-4", 10, 10, "t1");
    }
    auto snap = agg.snapshot("", std::chrono::system_clock::time_point::min(),
                             std::chrono::system_clock::time_point::max());
    EXPECT_EQ(snap.total.event_count, 100);
}

TEST(SavingsPersistence, BoundedQueueDropsOldest) {
    BlockingStore store;
    ASSERT_TRUE(store.initialize());
    CostTracker tracker;
    setupPricing(tracker);

    {
        SavingsAggregator agg(&tracker);
        agg.setPersistentStore(&store);
        // Flush thread blocks inside the first insert; queue fills past kMaxQueue
        // (10000) so drop-oldest must kick in.
        for (int i = 0; i < 12000; ++i) {
            agg.recordCacheHit("gpt-4", 1, 1, "t1");
        }
        EXPECT_GT(agg.droppedCount(), 0);
        store.release.store(true);  // unblock so destructor can drain + join
    }
    SUCCEED();
}
