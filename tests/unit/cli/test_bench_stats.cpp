#include <gtest/gtest.h>
#include "cli/bench_stats.h"
#include <numeric>

using namespace aegisgate::cli;

TEST(BenchStatsTest, PercentilesKnownData) {
    std::vector<double> latencies(100);
    std::iota(latencies.begin(), latencies.end(), 1.0); // 1..100

    auto stats = computeBenchStats(latencies, 0, 10.0);
    EXPECT_DOUBLE_EQ(stats.p50, 51.0);
    EXPECT_DOUBLE_EQ(stats.p90, 91.0);
    EXPECT_DOUBLE_EQ(stats.p99, 100.0);
    EXPECT_DOUBLE_EQ(stats.rps, 10.0);
    EXPECT_EQ(stats.completed, 100);
    EXPECT_EQ(stats.errors, 0);
}

TEST(BenchStatsTest, PercentilesEmpty) {
    std::vector<double> latencies;
    auto stats = computeBenchStats(latencies, 0, 5.0);
    EXPECT_DOUBLE_EQ(stats.p50, 0.0);
    EXPECT_DOUBLE_EQ(stats.p90, 0.0);
    EXPECT_DOUBLE_EQ(stats.p99, 0.0);
    EXPECT_DOUBLE_EQ(stats.rps, 0.0);
    EXPECT_EQ(stats.completed, 0);
}

TEST(BenchStatsTest, PercentilesSingleElement) {
    std::vector<double> latencies = {42.5};
    auto stats = computeBenchStats(latencies, 1, 1.0);
    EXPECT_DOUBLE_EQ(stats.p50, 42.5);
    EXPECT_DOUBLE_EQ(stats.p90, 42.5);
    EXPECT_DOUBLE_EQ(stats.p99, 42.5);
    EXPECT_DOUBLE_EQ(stats.rps, 1.0);
    EXPECT_EQ(stats.errors, 1);
}

TEST(BenchStatsTest, PercentilesUnsortedInput) {
    std::vector<double> latencies = {50, 10, 30, 20, 40, 90, 70, 80, 60, 100};
    auto stats = computeBenchStats(latencies, 2, 2.0);
    EXPECT_DOUBLE_EQ(stats.p50, 60.0);
    EXPECT_DOUBLE_EQ(stats.p90, 100.0);
    EXPECT_DOUBLE_EQ(stats.p99, 100.0);
    EXPECT_DOUBLE_EQ(stats.rps, 5.0);
    EXPECT_EQ(stats.errors, 2);
}

TEST(BenchStatsTest, RpsZeroDuration) {
    std::vector<double> latencies = {10.0, 20.0};
    auto stats = computeBenchStats(latencies, 0, 0.0);
    EXPECT_DOUBLE_EQ(stats.rps, 0.0);
}

TEST(BenchStatsTest, WorkDistributionEven) {
    auto counts = distributeWork(100, 10);
    ASSERT_EQ(counts.size(), 10u);
    for (int c : counts) EXPECT_EQ(c, 10);
    int total = std::accumulate(counts.begin(), counts.end(), 0);
    EXPECT_EQ(total, 100);
}

TEST(BenchStatsTest, WorkDistributionRemainder) {
    auto counts = distributeWork(103, 10);
    ASSERT_EQ(counts.size(), 10u);
    int total = std::accumulate(counts.begin(), counts.end(), 0);
    EXPECT_EQ(total, 103);
    // First 3 threads get 11, rest get 10
    for (int i = 0; i < 3; ++i) EXPECT_EQ(counts[i], 11);
    for (int i = 3; i < 10; ++i) EXPECT_EQ(counts[i], 10);
}

TEST(BenchStatsTest, WorkDistributionZeroConcurrency) {
    auto counts = distributeWork(100, 0);
    EXPECT_TRUE(counts.empty());
}

TEST(BenchStatsTest, WorkDistributionSingleThread) {
    auto counts = distributeWork(50, 1);
    ASSERT_EQ(counts.size(), 1u);
    EXPECT_EQ(counts[0], 50);
}
