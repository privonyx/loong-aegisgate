#include <gtest/gtest.h>
#include "gateway/rate_limiter.h"
#include <atomic>
#include <thread>
#include <vector>

using namespace aegisgate;

TEST(RateLimiterTest, AllowWithinLimit) {
    RateLimiter limiter({10.0, 10.0});
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(limiter.allow("user-1"));
    }
}

TEST(RateLimiterTest, RejectOverLimit) {
    RateLimiter limiter({5.0, 5.0});
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(limiter.allow("user-1"));
    }
    EXPECT_FALSE(limiter.allow("user-1"));
}

TEST(RateLimiterTest, DifferentKeysIndependent) {
    RateLimiter limiter({2.0, 2.0});
    EXPECT_TRUE(limiter.allow("user-1"));
    EXPECT_TRUE(limiter.allow("user-1"));
    EXPECT_FALSE(limiter.allow("user-1"));
    EXPECT_TRUE(limiter.allow("user-2"));
    EXPECT_TRUE(limiter.allow("user-2"));
}

TEST(RateLimiterTest, RefillOverTime) {
    RateLimiter limiter({2.0, 100.0});
    EXPECT_TRUE(limiter.allow("user-1"));
    EXPECT_TRUE(limiter.allow("user-1"));
    EXPECT_FALSE(limiter.allow("user-1"));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(limiter.allow("user-1"));
}

TEST(RateLimiterTest, CustomCost) {
    RateLimiter limiter({10.0, 10.0});
    EXPECT_TRUE(limiter.allow("user-1", 5.0));
    EXPECT_TRUE(limiter.allow("user-1", 5.0));
    EXPECT_FALSE(limiter.allow("user-1", 1.0));
}

TEST(RateLimiterTest, NegativeCost_AllowedWithoutDrain) {
    RateLimiter limiter({5.0, 5.0});
    EXPECT_TRUE(limiter.allow("user-1", -10.0));
    // Negative cost should not add tokens
    EXPECT_EQ(limiter.remaining("user-1"), 5.0);
}

TEST(RateLimiterTest, ZeroCost_AllowedWithoutDrain) {
    RateLimiter limiter({5.0, 5.0});
    EXPECT_TRUE(limiter.allow("user-1", 0.0));
}

TEST(RateLimiterTest, PerKeyConfig) {
    RateLimiter limiter({100.0, 100.0});
    limiter.setKeyConfig("vip", {1000.0, 1000.0});

    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(limiter.allow("vip"));
    }
}

TEST(RateLimiterTest, ConcurrentDifferentKeys) {
    RateLimiter limiter({1000.0, 1000.0});
    std::atomic<int> allowed{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&limiter, &allowed, t]() {
            std::string key = "user-" + std::to_string(t);
            for (int i = 0; i < 100; ++i) {
                if (limiter.allow(key)) ++allowed;
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(allowed.load(), 800);
}

TEST(RateLimiterTest, ConcurrentSameKey) {
    // Disable refill so the assertion validates only concurrent token consumption.
    RateLimiter limiter({100.0, 0.0});
    std::atomic<int> allowed{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&limiter, &allowed]() {
            for (int i = 0; i < 50; ++i) {
                if (limiter.allow("shared-key")) ++allowed;
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(allowed.load(), 100);
}
