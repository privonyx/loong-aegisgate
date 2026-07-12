#include <gtest/gtest.h>
#include "observe/usage_predictor.h"
#include "storage/memory_persistent_store.h"
#include <chrono>
#include <ctime>

using namespace aegisgate;

namespace {
std::string utcToday() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm buf{};
    gmtime_r(&tt, &buf);
    char date_buf[16];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &buf);
    return date_buf;
}

std::string utcDateOffset(int days) {
    auto now = std::chrono::system_clock::now() + std::chrono::hours(24 * days);
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm buf{};
    gmtime_r(&tt, &buf);
    char date_buf[16];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &buf);
    return date_buf;
}
}  // namespace

class UsagePredictorTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryPersistentStore>();
        store_->initialize();
    }
    std::unique_ptr<MemoryPersistentStore> store_;
};

TEST_F(UsagePredictorTest, LinearTrendDetected) {
    for (int d = 0; d < 10; ++d) {
        CostRecord rec;
        rec.request_id = "r" + std::to_string(d);
        rec.tenant_id = "t1";
        rec.total_cost = 10.0 + d * 2.0;
        rec.timestamp = utcDateOffset(d - 9) + "T12:00:00Z";
        store_->insertCostRecord(rec);
    }

    UsagePredictor predictor(store_.get());
    auto result = predictor.predict("t1", 30, 3);

    EXPECT_GT(result.daily_trend, 0.0);
    EXPECT_EQ(result.predicted.size(), 3u);
    EXPECT_GT(result.r_squared, 0.8);
}

TEST_F(UsagePredictorTest, InsufficientDataReturnsEmpty) {
    CostRecord rec;
    rec.request_id = "r1";
    rec.tenant_id = "t1";
    rec.total_cost = 5.0;
    rec.timestamp = utcDateOffset(-1) + "T12:00:00Z";
    store_->insertCostRecord(rec);

    UsagePredictor predictor(store_.get());
    auto result = predictor.predict("t1", 30, 7);
    EXPECT_TRUE(result.predicted.empty());
    EXPECT_DOUBLE_EQ(result.r_squared, 0.0);
}

TEST_F(UsagePredictorTest, BudgetExhaustionEstimated) {
    for (int d = 0; d < 10; ++d) {
        CostRecord rec;
        rec.request_id = "r" + std::to_string(d);
        rec.tenant_id = "t1";
        rec.total_cost = 100.0;
        rec.timestamp = utcDateOffset(d - 9) + "T12:00:00Z";
        store_->insertCostRecord(rec);
    }

    UsagePredictor predictor(store_.get());
    auto result = predictor.predictBudgetExhaustion("t1", 2000.0, 30);

    EXPECT_FALSE(result.budget_exhaustion_date.empty());
}

TEST_F(UsagePredictorTest, NoBudgetExhaustionWhenSufficient) {
    for (int d = 0; d < 5; ++d) {
        CostRecord rec;
        rec.request_id = "r" + std::to_string(d);
        rec.tenant_id = "t1";
        rec.total_cost = 1.0;
        rec.timestamp = utcDateOffset(d - 4) + "T12:00:00Z";
        store_->insertCostRecord(rec);
    }

    UsagePredictor predictor(store_.get());
    auto result = predictor.predictBudgetExhaustion("t1", 999999.0, 30);
    EXPECT_TRUE(result.budget_exhaustion_date.empty());
}

TEST_F(UsagePredictorTest, EmptyTenantReturnsEmpty) {
    UsagePredictor predictor(store_.get());
    auto result = predictor.predict("nonexistent", 30, 7);
    EXPECT_TRUE(result.predicted.empty());
}

TEST_F(UsagePredictorTest, AggregatesDailyCorrectly) {
    auto today = utcToday();
    for (int i = 0; i < 5; ++i) {
        CostRecord rec;
        rec.request_id = "r" + std::to_string(i);
        rec.tenant_id = "t1";
        rec.total_cost = 10.0;
        rec.timestamp = today + "T" + std::to_string(10 + i) + ":00:00Z";
        store_->insertCostRecord(rec);
    }

    UsagePredictor predictor(store_.get());
    auto result = predictor.predict("t1", 1, 0);
    ASSERT_EQ(result.historical.size(), 1u);
    EXPECT_DOUBLE_EQ(result.historical[0].total_cost, 50.0);
    EXPECT_EQ(result.historical[0].request_count, 5);
}
