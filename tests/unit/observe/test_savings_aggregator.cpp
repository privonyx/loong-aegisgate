#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

#include "observe/cost_tracker.h"
#include "observe/savings_aggregator.h"

using namespace aegisgate;

class SavingsAggregatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        tracker_.setPricing("gpt-4", 0.03, 0.06);
        tracker_.setPricing("gpt-3.5", 0.001, 0.002);
        aggregator_ = std::make_unique<SavingsAggregator>(&tracker_);
    }
    CostTracker tracker_;
    std::unique_ptr<SavingsAggregator> aggregator_;
};

TEST_F(SavingsAggregatorTest, RecordCacheHit_KnownPricing) {
    aggregator_->recordCacheHit("gpt-4", 100, 200, "tenant-A");
    EXPECT_EQ(1u, aggregator_->eventCount());

    auto snap = aggregator_->snapshot(
        "",
        std::chrono::system_clock::time_point::min(),
        std::chrono::system_clock::time_point::max());
    EXPECT_EQ(1, snap.total.event_count);
    EXPECT_EQ(300, snap.total.tokens_saved);
    // cost = 100/1000 * 0.03 + 200/1000 * 0.06 = 0.003 + 0.012 = 0.015
    EXPECT_NEAR(0.015, snap.total.cost_saved, 1e-6);
    EXPECT_EQ(0, snap.total.fallback_count);
    EXPECT_EQ(1, snap.by_type[static_cast<int>(SavingType::CacheHit)].event_count);
}

TEST_F(SavingsAggregatorTest, RecordCacheHit_UnknownPricing_Fallback) {
    aggregator_->recordCacheHit("unknown-model", 100, 200, "tenant-A");
    auto snap = aggregator_->snapshot(
        "",
        std::chrono::system_clock::time_point::min(),
        std::chrono::system_clock::time_point::max());
    EXPECT_EQ(1, snap.total.event_count);
    // unknown model 单价为 0，cost_saved = 0，但 fallback 标记为 true（透明度）
    EXPECT_NEAR(0.0, snap.total.cost_saved, 1e-9);
    EXPECT_EQ(1, snap.total.fallback_count);
}

TEST_F(SavingsAggregatorTest, RecordCompression_OnlyInput) {
    aggregator_->recordCompression("gpt-4", 50, "tenant-A");
    auto snap = aggregator_->snapshot(
        "",
        std::chrono::system_clock::time_point::min(),
        std::chrono::system_clock::time_point::max());
    EXPECT_EQ(1, snap.total.event_count);
    EXPECT_EQ(50, snap.total.tokens_saved);
    // cost = 50/1000 * 0.03 = 0.0015
    EXPECT_NEAR(0.0015, snap.total.cost_saved, 1e-6);
}

TEST_F(SavingsAggregatorTest, RecordRouting_PotentialOnly) {
    aggregator_->recordRouting("gpt-4", "gpt-3.5", 5.50, "tenant-A");
    auto snap = aggregator_->snapshot(
        "",
        std::chrono::system_clock::time_point::min(),
        std::chrono::system_clock::time_point::max());
    EXPECT_EQ(1, snap.total.event_count);
    EXPECT_NEAR(5.50, snap.total.cost_saved, 1e-6);
    EXPECT_EQ(0, snap.total.tokens_saved);  // routing 是潜在节省，不报告 token
    EXPECT_EQ(1, snap.by_type[static_cast<int>(SavingType::Routing)].event_count);
}

TEST_F(SavingsAggregatorTest, Snapshot_TimeWindowFilter) {
    auto t0 = std::chrono::system_clock::now();
    aggregator_->recordCacheHit("gpt-4", 100, 100, "A");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto cut = std::chrono::system_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    aggregator_->recordCacheHit("gpt-4", 200, 200, "A");

    auto snap_before = aggregator_->snapshot("", t0, cut);
    auto snap_after = aggregator_->snapshot(
        "", cut, std::chrono::system_clock::time_point::max());
    EXPECT_EQ(1, snap_before.total.event_count);
    EXPECT_EQ(1, snap_after.total.event_count);
    EXPECT_EQ(200, snap_before.total.tokens_saved);
    EXPECT_EQ(400, snap_after.total.tokens_saved);
}

TEST_F(SavingsAggregatorTest, Snapshot_TenantFilter) {
    aggregator_->recordCacheHit("gpt-4", 100, 100, "A");
    aggregator_->recordCacheHit("gpt-4", 200, 200, "B");
    aggregator_->recordCacheHit("gpt-4", 300, 300, "A");

    auto tmin = std::chrono::system_clock::time_point::min();
    auto tmax = std::chrono::system_clock::time_point::max();
    auto snap_a = aggregator_->snapshot("A", tmin, tmax);
    auto snap_b = aggregator_->snapshot("B", tmin, tmax);
    auto snap_all = aggregator_->snapshot("", tmin, tmax);

    EXPECT_EQ(2, snap_a.total.event_count);
    EXPECT_EQ(800, snap_a.total.tokens_saved);
    EXPECT_EQ(1, snap_b.total.event_count);
    EXPECT_EQ(400, snap_b.total.tokens_saved);
    EXPECT_EQ(3, snap_all.total.event_count);
    EXPECT_EQ(2u, snap_all.by_tenant.size());
}

TEST_F(SavingsAggregatorTest, Snapshot_ByModel_ByType) {
    aggregator_->recordCacheHit("gpt-4", 100, 100, "A");
    aggregator_->recordCacheHit("gpt-3.5", 200, 200, "A");
    aggregator_->recordCompression("gpt-4", 50, "A");

    auto snap = aggregator_->snapshot(
        "",
        std::chrono::system_clock::time_point::min(),
        std::chrono::system_clock::time_point::max());

    // by_model: gpt-4 命中 200 token + 压缩 50 token = 250；gpt-3.5 命中 400 token
    EXPECT_EQ(250, snap.by_model["gpt-4"].tokens_saved);
    EXPECT_EQ(400, snap.by_model["gpt-3.5"].tokens_saved);

    // by_type: CacheHit = 600 token；Compression = 50 token
    EXPECT_EQ(600, snap.by_type[static_cast<int>(SavingType::CacheHit)].tokens_saved);
    EXPECT_EQ(50, snap.by_type[static_cast<int>(SavingType::Compression)].tokens_saved);
}

TEST_F(SavingsAggregatorTest, FIFO_100k_Cap) {
    // 写入 100K + 50 个事件，验证最早 50 个被环形淘汰
    for (int i = 0; i < 100050; ++i) {
        aggregator_->recordCompression("gpt-4", 1, "A");
    }
    EXPECT_EQ(100000u, aggregator_->eventCount());
    auto snap = aggregator_->snapshot(
        "",
        std::chrono::system_clock::time_point::min(),
        std::chrono::system_clock::time_point::max());
    EXPECT_EQ(100000, snap.total.event_count);
    EXPECT_EQ(100000, snap.total.tokens_saved);
}

TEST_F(SavingsAggregatorTest, ConcurrentRecord_Snapshot_Safe) {
    const int n_threads = 8;
    const int per_thread = 1000;
    std::vector<std::thread> threads;
    threads.reserve(n_threads);
    for (int t = 0; t < n_threads; ++t) {
        threads.emplace_back([this, t]() {
            for (int i = 0; i < per_thread; ++i) {
                aggregator_->recordCacheHit(
                    "gpt-4", 1, 1, "tenant-" + std::to_string(t));
            }
        });
    }
    std::thread snap_thread([this]() {
        for (int i = 0; i < 100; ++i) {
            auto snap = aggregator_->snapshot(
                "",
                std::chrono::system_clock::time_point::min(),
                std::chrono::system_clock::time_point::max());
            (void)snap;  // ASAN/TSAN 验证 race-free
        }
    });
    for (auto& th : threads) {
        th.join();
    }
    snap_thread.join();
    EXPECT_EQ(static_cast<size_t>(n_threads * per_thread), aggregator_->eventCount());
}

TEST_F(SavingsAggregatorTest, TimeSeriesByDay_FormatAndCount) {
    aggregator_->recordCacheHit("gpt-4", 100, 100, "A");
    aggregator_->recordCacheHit("gpt-4", 200, 200, "A");
    auto snap = aggregator_->snapshot(
        "",
        std::chrono::system_clock::time_point::min(),
        std::chrono::system_clock::time_point::max());
    ASSERT_GE(snap.time_series_by_day.size(), 1u);
    EXPECT_EQ(10u, snap.time_series_by_day[0].first.size());  // YYYY-MM-DD
    EXPECT_EQ('-', snap.time_series_by_day[0].first[4]);
    EXPECT_EQ('-', snap.time_series_by_day[0].first[7]);
}

TEST_F(SavingsAggregatorTest, ZeroOrNegativeCompressionTokens_Ignored) {
    aggregator_->recordCompression("gpt-4", 0, "A");
    aggregator_->recordCompression("gpt-4", -5, "A");
    EXPECT_EQ(0u, aggregator_->eventCount());
}

TEST_F(SavingsAggregatorTest, StartedAt_StableAcrossCalls) {
    auto a = aggregator_->startedAt();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto b = aggregator_->startedAt();
    EXPECT_EQ(a, b);
}

// SR-NEW4：hot path noexcept 安全。
// 验证 tracker = nullptr 时 record* 不崩、不抛、并正确标记 fallback。
TEST(SavingsAggregatorSafety, NullCostTracker_RecordsWithFallback) {
    SavingsAggregator agg(nullptr);
    EXPECT_NO_THROW(agg.recordCacheHit("any-model", 100, 100, "tenant-A"));
    EXPECT_NO_THROW(agg.recordCompression("any-model", 50, "tenant-A"));
    EXPECT_EQ(2u, agg.eventCount());

    auto snap = agg.snapshot(
        "",
        std::chrono::system_clock::time_point::min(),
        std::chrono::system_clock::time_point::max());
    EXPECT_EQ(2, snap.total.event_count);
    EXPECT_EQ(2, snap.total.fallback_count);  // nullptr tracker → 全部 fallback
    EXPECT_NEAR(0.0, snap.total.cost_saved, 1e-9);
}

// SR-NEW4：noexcept 编译期保证。
// 如果未来某次重构去掉 noexcept 标注，本测试将编译失败。
static_assert(noexcept(std::declval<SavingsAggregator&>().recordCacheHit(
                  std::declval<const std::string&>(), 0, 0,
                  std::declval<const std::string&>())),
              "recordCacheHit must be noexcept (SR-NEW4)");
static_assert(noexcept(std::declval<SavingsAggregator&>().recordCompression(
                  std::declval<const std::string&>(), 0,
                  std::declval<const std::string&>())),
              "recordCompression must be noexcept (SR-NEW4)");

TEST(SavingsAggregatorSafety, NoExceptHotPath) {
    // 编译期 static_assert 通过即说明 noexcept 标注存在。
    // 此处运行时再做一遍函数指针校验作为冗余保险。
    EXPECT_TRUE(noexcept(std::declval<SavingsAggregator&>().recordCacheHit(
        std::declval<const std::string&>(), 0, 0,
        std::declval<const std::string&>())));
    EXPECT_TRUE(noexcept(std::declval<SavingsAggregator&>().recordCompression(
        std::declval<const std::string&>(), 0,
        std::declval<const std::string&>())));
}
