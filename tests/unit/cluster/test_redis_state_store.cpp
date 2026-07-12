#include <gtest/gtest.h>

#ifdef AEGISGATE_ENABLE_REDIS
#include "cluster/redis_state_store.h"
#include "storage/redis_cache_store.h"
#include "gateway/rate_limiter.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace aegisgate;

class RedisStateStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        RedisConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = 6379;
        redis_ = std::make_unique<RedisCacheStore>(cfg);
        if (!redis_->initialize()) {
            GTEST_SKIP() << "Redis not available, skipping cluster state tests";
        }
        store_ = std::make_unique<RedisStateStore>(redis_.get());
        store_->initialize();
        redis_->clear();
    }

    void TearDown() override {
        if (redis_ && redis_->isHealthy()) redis_->clear();
    }

    std::unique_ptr<RedisCacheStore> redis_;
    std::unique_ptr<RedisStateStore> store_;
};

TEST_F(RedisStateStoreTest, RateLimitAllowDecrement) {
    EXPECT_TRUE(store_->rateLimitAllow("test-key", 5.0, 10.0, 1.0));
    double rem = store_->rateLimitRemaining("test-key", 10.0, 1.0);
    EXPECT_LT(rem, 10.0);
}

TEST_F(RedisStateStoreTest, RateLimitRejectsOverBudget) {
    EXPECT_TRUE(store_->rateLimitAllow("over-key", 10.0, 10.0, 0.01));
    EXPECT_FALSE(store_->rateLimitAllow("over-key", 5.0, 10.0, 0.01));
}

// P0-C (TASK-20260701-01): the distributed rate limiter must decrement the
// shared bucket atomically. The pre-fix implementation did a non-atomic
// GET -> modify -> SET, so concurrent callers (mirroring multiple cluster
// nodes hitting the same key) all read the same token count and over-admit
// beyond the bucket capacity. With an atomic single-round-trip EVAL, the total
// number of allowed requests can never exceed the initial capacity when refill
// is zero.
TEST_F(RedisStateStoreTest, RateLimitAtomicUnderConcurrency) {
    const std::string key = "concurrency-key";
    const double max_tokens = 50.0;
    const double refill = 0.0;  // no refill for the duration of the test
    const double cost = 1.0;
    const int threads = 16;
    const int per_thread = 20;  // 320 attempts against a capacity of 50

    std::atomic<int> allowed{0};
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            for (int i = 0; i < per_thread; ++i) {
                if (store_->rateLimitAllow(key, cost, max_tokens, refill)) {
                    allowed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : pool) th.join();

    EXPECT_LE(allowed.load(), static_cast<int>(max_tokens))
        << "over-admission indicates a non-atomic read-modify-write race";
}

// TASK-20260708-02 / REV20260707-C1 Epic 6 SR-4 — end-to-end cluster-quota
// consistency at the RateLimiter API layer. Two RateLimiter instances share a
// single RedisStateStore (mirroring two gateway processes sharing a Redis
// cluster). 320 concurrent allow() calls against a bucket capacity of 50 must
// total no more than 50 admissions — proving that Epic 3's
// rate_limiter_->setRedisStateStore wiring routes bucket state through Redis
// (not through the per-instance in-memory shards) and that the atomic EVAL
// bucket is honored end-to-end.
TEST_F(RedisStateStoreTest, ClusterQuotaConsistencyAcrossTwoRateLimiters_SR4) {
    RateLimiter::Config cfg;
    cfg.max_tokens = 50.0;
    cfg.refill_rate = 0.0;  // no refill for the duration of the test

    RateLimiter rl_a(cfg);
    RateLimiter rl_b(cfg);
    rl_a.setRedisStateStore(store_.get());
    rl_b.setRedisStateStore(store_.get());

    const std::string key = "test-sr4:tenant";
    const int threads = 16;
    const int per_thread = 20;   // 16 * 20 = 320 attempts

    std::atomic<int> allowed{0};
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (int t = 0; t < threads; ++t) {
        // Half the threads hit instance A, half hit instance B.
        RateLimiter& limiter = (t % 2 == 0) ? rl_a : rl_b;
        pool.emplace_back([&limiter, &allowed, &key, per_thread] {
            for (int i = 0; i < per_thread; ++i) {
                if (limiter.allow(key, 1.0)) {
                    allowed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : pool) th.join();

    EXPECT_LE(allowed.load(), static_cast<int>(cfg.max_tokens))
        << "Two RateLimiter instances sharing a RedisStateStore over-admitted "
        << allowed.load() << " requests against a capacity of "
        << cfg.max_tokens << " — the shared Redis bucket is not authoritative "
        << "(per-instance in-memory shards leaked through).";
}

// ---------------------------------------------------------------------------
// TASK-20260703-02 Epic 2 / C6 — computeCircuitState 纯逻辑（不依赖 redis 连接，
// 沙箱无 redis 也能跑，非 fixture）。C6 根因：Closed 累积失败到阈值（state=0,
// fc>=threshold）此前无条件 Open，不受 reset_timeout 支配 → 永停 Open 靠 TTL 恢复。
// ---------------------------------------------------------------------------

TEST(CircuitStateComputeTest, ClosedBelowThreshold) {
    EXPECT_EQ(computeCircuitState(0, 2, 0, 100000, 3, 30), CircuitState::Closed);
}

TEST(CircuitStateComputeTest, ClosedAtThresholdWithinTimeoutIsOpen) {
    int64_t now = 100000, last = now - 5 * 1000;  // 5s ago < 30s
    EXPECT_EQ(computeCircuitState(0, 3, last, now, 3, 30), CircuitState::Open);
}

TEST(CircuitStateComputeTest, ClosedAtThresholdAfterTimeoutIsHalfOpen) {
    // C6 核心修复：Closed 累积到阈值，经过 reset_timeout → HalfOpen（修复前永 Open）。
    int64_t now = 100000, last = now - 40 * 1000;  // 40s ago >= 30s
    EXPECT_EQ(computeCircuitState(0, 3, last, now, 3, 30), CircuitState::HalfOpen);
}

TEST(CircuitStateComputeTest, ExplicitOpenWithinTimeoutStaysOpen) {
    int64_t now = 100000, last = now - 5 * 1000;
    EXPECT_EQ(computeCircuitState(1, 5, last, now, 3, 30), CircuitState::Open);
}

TEST(CircuitStateComputeTest, ExplicitOpenAfterTimeoutIsHalfOpen) {
    int64_t now = 100000, last = now - 40 * 1000;
    EXPECT_EQ(computeCircuitState(1, 5, last, now, 3, 30), CircuitState::HalfOpen);
}

TEST(CircuitStateComputeTest, ExplicitHalfOpenState) {
    EXPECT_EQ(computeCircuitState(2, 5, 0, 100000, 3, 30), CircuitState::HalfOpen);
}

TEST_F(RedisStateStoreTest, CircuitBreakerDefaultClosed) {
    EXPECT_EQ(store_->cbGetState("model-x", 3, 30), CircuitState::Closed);
    EXPECT_TRUE(store_->cbAllowRequest("model-x", 3, 30, 1));
}

TEST_F(RedisStateStoreTest, CircuitBreakerOpensOnFailures) {
    for (int i = 0; i < 5; ++i) store_->cbRecordFailure("fail-model", 3);
    auto state = store_->cbGetState("fail-model", 3, 30);
    EXPECT_EQ(state, CircuitState::Open);
}

TEST_F(RedisStateStoreTest, CircuitBreakerResetsOnSuccess) {
    store_->cbRecordFailure("reset-model", 3);
    store_->cbRecordSuccess("reset-model");
    EXPECT_EQ(store_->cbGetState("reset-model", 3, 30), CircuitState::Closed);
}

TEST_F(RedisStateStoreTest, AbuseRecordAndCount) {
    store_->abuseRecordRejection("abuser-1", 300);
    store_->abuseRecordRejection("abuser-1", 300);
    EXPECT_EQ(store_->abuseGetCount("abuser-1", 300), 2);
}

TEST_F(RedisStateStoreTest, AbuseBlockedFlag) {
    EXPECT_FALSE(store_->abuseIsBlocked("clean-key"));
    store_->abuseSetBlocked("bad-key", 60);
    EXPECT_TRUE(store_->abuseIsBlocked("bad-key"));
}

TEST_F(RedisStateStoreTest, MLStatsDefaultValues) {
    auto stats = store_->mlGetStats("unknown-model");
    EXPECT_DOUBLE_EQ(stats.avg_latency_ms, 100.0);
    EXPECT_DOUBLE_EQ(stats.success_rate, 1.0);
    EXPECT_EQ(stats.sample_count, 0);
}

TEST_F(RedisStateStoreTest, MLStatsUpdateViaEMA) {
    store_->mlReportOutcome("ema-model", 50.0, true);
    auto s1 = store_->mlGetStats("ema-model");
    EXPECT_EQ(s1.sample_count, 1);
    EXPECT_DOUBLE_EQ(s1.avg_latency_ms, 50.0);

    store_->mlReportOutcome("ema-model", 150.0, false);
    auto s2 = store_->mlGetStats("ema-model");
    EXPECT_EQ(s2.sample_count, 2);
    EXPECT_GT(s2.avg_latency_ms, 50.0);
    EXPECT_LT(s2.success_rate, 1.0);
}

// TASK-20260711-02 / TASK-20260701-01 P0-C-BAK · D3 Option C — Redis
// cbRecordFailure must (a) atomically increment the failure counter and
// (b) explicitly persist state=1(Open) when the counter reaches threshold,
// eliminating the previous lost-update race and the observability drift
// where the state field remained stale (state=0) with Open only implicit
// via computeCircuitState()'s C6 FIX fallback.

// SR-7: After threshold failures, the persisted state field must be
// Open — not just the computed effective state. Verified via a direct
// HGET so the test cannot be fooled by the compute() fallback.
TEST_F(RedisStateStoreTest, CbRecordFailurePostThresholdPersistStateOpen_SR7) {
    const std::string model = "sr7-model";
    const int threshold = 3;
    for (int i = 0; i < threshold; ++i) {
        store_->cbRecordFailure(model, threshold);
    }
    // Read the raw hash field bypassing cbGetState so we verify the
    // persisted state (not the computed fallback).
    auto raw = redis_->executeRaw(
        "HGET aegisgate:aegisgate:cb:" + model + " state");
    ASSERT_TRUE(raw.has_value()) << "state field must be persisted";
    EXPECT_EQ(*raw, "1") << "state field must be 1 (Open) after "
                          << threshold << " failures";
}

// SR-8: Post-timeout, cbGetState reports HalfOpen driven by the state
// field being Open (not by C6 FIX fallback). Regression: even if
// computeCircuitState() were disabled, cbGetState must still transition
// via reset_timeout.
TEST_F(RedisStateStoreTest, CbGetStatePostTimeoutReportsHalfOpenViaStateField_SR8) {
    const std::string model = "sr8-model";
    const int threshold = 3;
    const int reset_timeout_s = 1;  // 1s to keep the test fast
    for (int i = 0; i < threshold; ++i) {
        store_->cbRecordFailure(model, threshold);
    }
    // Wait past the timeout window.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    EXPECT_EQ(store_->cbGetState(model, threshold, reset_timeout_s),
              CircuitState::HalfOpen);
}

// SR-9: Two RedisStateStore instances sharing a Redis backend (mirroring
// two gateway nodes in a cluster) must not lose updates when concurrently
// recording failures. Atomic EVAL guarantees the final counter equals the
// total number of calls.
TEST_F(RedisStateStoreTest, CbRecordFailureTwoNodeInterleaveAtomicCount_SR9) {
    // Use two separate RedisStateStore instances against the same Redis.
    RedisStateStore store_b(redis_.get());
    store_b.initialize();
    const std::string model = "sr9-model";
    const int threshold = 100;  // deliberately high so we don't Open before
                                  // finishing the interleave loop
    for (int i = 0; i < 6; ++i) {
        store_->cbRecordFailure(model, threshold);
        store_b.cbRecordFailure(model, threshold);
    }
    auto raw = redis_->executeRaw(
        "HGET aegisgate:aegisgate:cb:" + model + " failure_count");
    ASSERT_TRUE(raw.has_value());
    EXPECT_EQ(*raw, "12")
        << "Two-node interleaved cbRecordFailure lost updates — expected 12, "
           "got " << *raw;
}

#else

TEST(RedisStateStoreTest, SkippedWithoutRedis) {
    GTEST_SKIP() << "AEGISGATE_ENABLE_REDIS is OFF, cluster state tests skipped";
}

#endif
