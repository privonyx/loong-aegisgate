#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include "observe/cost_attribution.h"

using namespace aegisgate;

class CostAttributionTest : public ::testing::Test {
protected:
    void SetUp() override { ca_.clear(); }
    CostAttribution ca_;
};

TEST_F(CostAttributionTest, RecordAndQuery) {
    ca_.record(CostAttributionEntry("r1", "t1", "app1", "gpt-4", 0.06, 1500));
    ca_.record(CostAttributionEntry("r2", "t1", "app1", "gpt-4", 0.03, 750));
    ca_.record(CostAttributionEntry("r3", "t2", "app2", "claude", 0.05, 1000));

    EXPECT_NEAR(ca_.getCostByApp("app1"), 0.09, 1e-9);
    EXPECT_NEAR(ca_.getCostByTenant("t1"), 0.09, 1e-9);
    EXPECT_NEAR(ca_.getCostByModel("gpt-4"), 0.09, 1e-9);
    EXPECT_NEAR(ca_.getCostByModel("claude"), 0.05, 1e-9);
    EXPECT_NEAR(ca_.getCostByTenant("t2"), 0.05, 1e-9);
}

TEST_F(CostAttributionTest, TopCostApps) {
    ca_.record(CostAttributionEntry("r1", "t1", "app-a", "m", 10.0, 100));
    ca_.record(CostAttributionEntry("r2", "t1", "app-b", "m", 30.0, 200));
    ca_.record(CostAttributionEntry("r3", "t1", "app-c", "m", 20.0, 150));

    auto top = ca_.getTopCostApps(2);
    ASSERT_EQ(top.size(), 2u);
    EXPECT_EQ(top[0].app_id, "app-b");
    EXPECT_NEAR(top[0].total_cost, 30.0, 1e-9);
    EXPECT_EQ(top[1].app_id, "app-c");
    EXPECT_NEAR(top[1].total_cost, 20.0, 1e-9);
}

TEST_F(CostAttributionTest, TimeWindowFilter) {
    CostAttributionEntry old_entry("r-old", "t1", "app1", "gpt-4", 100.0, 5000);
    old_entry.timestamp = std::chrono::steady_clock::now() - std::chrono::hours(48);
    ca_.record(old_entry);

    ca_.record(CostAttributionEntry("r-new", "t1", "app1", "gpt-4", 1.0, 50));

    EXPECT_NEAR(ca_.getCostByApp("app1", std::chrono::seconds(3600)), 1.0, 1e-9);
    EXPECT_NEAR(ca_.getCostByApp("app1", std::chrono::seconds(200000)), 101.0, 1e-9);
}

TEST_F(CostAttributionTest, PruneOldEntries) {
    for (int i = 0; i < 5; i++) {
        CostAttributionEntry e("r" + std::to_string(i), "t1", "app1", "m", 1.0, 10);
        e.timestamp = std::chrono::steady_clock::now() - std::chrono::hours(200);
        ca_.record(e);
    }

    EXPECT_EQ(ca_.size(), 5u);

    ca_.record(CostAttributionEntry("r-new", "t1", "app1", "m", 1.0, 10));
    EXPECT_GE(ca_.size(), 1u);
}

TEST_F(CostAttributionTest, EmptyResult) {
    EXPECT_DOUBLE_EQ(ca_.getCostByApp("nonexistent"), 0.0);
    EXPECT_DOUBLE_EQ(ca_.getCostByTenant("nonexistent"), 0.0);
    EXPECT_DOUBLE_EQ(ca_.getCostByModel("nonexistent"), 0.0);
    EXPECT_EQ(ca_.size(), 0u);

    auto top = ca_.getTopCostApps();
    EXPECT_TRUE(top.empty());
}

TEST_F(CostAttributionTest, SizeAndClear) {
    ca_.record(CostAttributionEntry("r1", "t1", "a1", "m1", 1.0, 10));
    ca_.record(CostAttributionEntry("r2", "t2", "a2", "m2", 2.0, 20));
    EXPECT_EQ(ca_.size(), 2u);

    ca_.clear();
    EXPECT_EQ(ca_.size(), 0u);
}
