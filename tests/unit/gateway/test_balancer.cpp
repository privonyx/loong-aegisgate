#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include "gateway/balancer.h"

using namespace aegisgate;

TEST(BalancerTest, RoundRobinRotation) {
    Balancer bal({{"key-a", 1}, {"key-b", 1}, {"key-c", 1}});

    auto k1 = bal.nextKey();
    auto k2 = bal.nextKey();
    auto k3 = bal.nextKey();

    // All keys should be returned (round-robin)
    std::set<std::string> seen{k1, k2, k3};
    EXPECT_EQ(seen.size(), 3u);
}

TEST(BalancerTest, SkipUnhealthyKey) {
    Balancer bal({{"good-key", 1}, {"bad-key", 1}});

    // Fail bad-key enough times
    for (int i = 0; i < Balancer::kMaxConsecutiveFailures; ++i) {
        bal.reportFailure("bad-key");
    }

    // Should only return good-key
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(bal.nextKey(), "good-key");
    }
    EXPECT_EQ(bal.healthyCount(), 1u);
}

TEST(BalancerTest, SuccessResetsFailCount) {
    Balancer bal({{"key-a", 1}});
    bal.reportFailure("key-a");
    bal.reportFailure("key-a");
    bal.reportSuccess("key-a");

    EXPECT_EQ(bal.healthyCount(), 1u);
    EXPECT_EQ(bal.nextKey(), "key-a");
}

TEST(BalancerTest, AllKeysUnhealthy) {
    Balancer bal({{"key-a", 1}});
    for (int i = 0; i < Balancer::kMaxConsecutiveFailures; ++i) {
        bal.reportFailure("key-a");
    }

    EXPECT_EQ(bal.healthyCount(), 0u);
    EXPECT_EQ(bal.nextKey(), "");
}

TEST(BalancerTest, SingleKey) {
    Balancer bal({{"only-key", 1}});
    EXPECT_EQ(bal.nextKey(), "only-key");
    EXPECT_EQ(bal.nextKey(), "only-key");
}

TEST(BalancerTest, ConcurrentNextKeyCorrectness) {
    Balancer bal({{"k0", 1}, {"k1", 2}, {"k2", 3}});
    std::atomic<int> total{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 1000; ++i) {
                auto key = bal.nextKey();
                EXPECT_FALSE(key.empty());
                total.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(total.load(), 8000);
}

TEST(BalancerTest, ConcurrentReportCorrectness) {
    Balancer bal({{"k0", 1}, {"k1", 1}});
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 500; ++i) {
                if (t % 2 == 0) {
                    bal.reportSuccess("k0");
                } else {
                    bal.reportFailure("k1");
                    bal.reportSuccess("k1");
                }
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_GE(bal.healthyCount(), 1u);
}

TEST(BalancerTest, WeightedDistribution) {
    Balancer bal({{"heavy", 3}, {"light", 1}});
    int heavy_count = 0;
    for (int i = 0; i < 400; ++i) {
        if (bal.nextKey() == "heavy") ++heavy_count;
    }
    EXPECT_GT(heavy_count, 250);
    EXPECT_LT(heavy_count, 350);
}
