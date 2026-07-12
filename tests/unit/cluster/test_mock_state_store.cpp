#include <gtest/gtest.h>
#include "cluster/mock_redis_state_store.h"

using namespace aegisgate;

TEST(MockRedisStateStoreTest, RateLimitAllowAndDeny) {
    MockRedisStateStore store;
    EXPECT_TRUE(store.rateLimitAllow("k1", 8.0, 10.0, 1.0));
    EXPECT_TRUE(store.rateLimitAllow("k1", 2.0, 10.0, 1.0));
    EXPECT_FALSE(store.rateLimitAllow("k1", 1.0, 10.0, 0.01));
}

TEST(MockRedisStateStoreTest, RateLimitRemaining) {
    MockRedisStateStore store;
    EXPECT_DOUBLE_EQ(store.rateLimitRemaining("new", 100.0, 1.0), 100.0);
    store.rateLimitAllow("new", 30.0, 100.0, 1.0);
    EXPECT_LT(store.rateLimitRemaining("new", 100.0, 1.0), 100.0);
}

TEST(MockRedisStateStoreTest, CircuitBreakerDefaultClosed) {
    MockRedisStateStore store;
    EXPECT_EQ(store.cbGetState("m1", 3, 30), CircuitState::Closed);
    EXPECT_TRUE(store.cbAllowRequest("m1", 3, 30, 1));
}

TEST(MockRedisStateStoreTest, CircuitBreakerOpensOnFailures) {
    MockRedisStateStore store;
    for (int i = 0; i < 5; ++i) store.cbRecordFailure("m2", 3);
    EXPECT_EQ(store.cbGetState("m2", 3, 30), CircuitState::Open);
}

TEST(MockRedisStateStoreTest, CircuitBreakerResetsOnSuccess) {
    MockRedisStateStore store;
    store.cbRecordFailure("m3", 3);
    store.cbRecordSuccess("m3");
    EXPECT_EQ(store.cbGetState("m3", 3, 30), CircuitState::Closed);
}

TEST(MockRedisStateStoreTest, AbuseCountAndBlock) {
    MockRedisStateStore store;
    store.abuseRecordRejection("a1", 300);
    store.abuseRecordRejection("a1", 300);
    EXPECT_EQ(store.abuseGetCount("a1", 300), 2);
    EXPECT_FALSE(store.abuseIsBlocked("a1"));
    store.abuseSetBlocked("a1", 60);
    EXPECT_TRUE(store.abuseIsBlocked("a1"));
}

TEST(MockRedisStateStoreTest, MLStatsEMA) {
    MockRedisStateStore store;
    auto s0 = store.mlGetStats("x");
    EXPECT_EQ(s0.sample_count, 0);

    store.mlReportOutcome("x", 50.0, true);
    auto s1 = store.mlGetStats("x");
    EXPECT_EQ(s1.sample_count, 1);
    EXPECT_DOUBLE_EQ(s1.avg_latency_ms, 50.0);

    store.mlReportOutcome("x", 150.0, false);
    auto s2 = store.mlGetStats("x");
    EXPECT_EQ(s2.sample_count, 2);
    EXPECT_GT(s2.avg_latency_ms, 50.0);
    EXPECT_LT(s2.success_rate, 1.0);
}
