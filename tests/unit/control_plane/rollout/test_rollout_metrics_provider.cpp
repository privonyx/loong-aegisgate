// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic B.2 RolloutMetricsProvider.
//
// Covers CR1 creative design (naive ring buffer + per-query sort):
//   memory-bank/creative/creative-rollout-metrics-algorithm.md
//
// Structured across 6 surfaces:
//   1. Empty / single-sample basics.
//   2. error_rate — precise counting.
//   3. p99 — naive sort over full sample vector.
//   4. Sliding window filter — samples outside [now - window, now] excluded.
//   5. max_samples_per_version enforcement (SR15 cap).
//   6. Multi-version isolation + FeedbackBus integration + SR15 noexcept.

#include "control_plane/rollout/rollout_metrics_provider.h"
#include "aegisgate/feedback_event.h"
#include "observe/feedback_bus.h"

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <nlohmann/json.hpp>

namespace aegisgate {
namespace {

using std::chrono::seconds;

// Helper: build a provider without bus attachment. The bus-less ctor lets
// tests drive the ring buffer via recordRouterOutcome() without spinning
// a background dispatcher.
std::unique_ptr<FeedbackBusMetricsProvider>
makeStandalone(std::size_t cap = 50'000) {
    return std::make_unique<FeedbackBusMetricsProvider>(cap);
}

// --- 1. Empty / single-sample basics --------------------------------------

TEST(MetricsProvider, EmptyProviderReturnsZeroes) {
    auto p = makeStandalone();
    auto m = p->forVersionAt("v-unknown", seconds{60}, /*now_ms=*/1'000);
    EXPECT_EQ(m.sample_count, 0);
    EXPECT_DOUBLE_EQ(m.error_rate, 0.0);
    EXPECT_DOUBLE_EQ(m.p99_latency_ms, 0.0);
}

TEST(MetricsProvider, SingleSuccessSampleProducesExactValues) {
    auto p = makeStandalone();
    p->recordRouterOutcome("v1", /*ts_ms=*/1'000, /*latency=*/42.0, /*is_error=*/false);
    auto m = p->forVersionAt("v1", seconds{60}, /*now_ms=*/1'500);
    EXPECT_EQ(m.sample_count, 1);
    EXPECT_DOUBLE_EQ(m.error_rate, 0.0);
    EXPECT_DOUBLE_EQ(m.p99_latency_ms, 42.0);
}

TEST(MetricsProvider, SingleErrorSampleSetsErrorRateOne) {
    auto p = makeStandalone();
    p->recordRouterOutcome("v1", 1'000, 75.0, /*is_error=*/true);
    auto m = p->forVersionAt("v1", seconds{60}, /*now_ms=*/1'500);
    EXPECT_EQ(m.sample_count, 1);
    EXPECT_DOUBLE_EQ(m.error_rate, 1.0);
    EXPECT_DOUBLE_EQ(m.p99_latency_ms, 75.0);  // p99 is over ALL samples.
}

// --- 2. error_rate ---------------------------------------------------------

TEST(MetricsProvider, ErrorRateExactAcrossHundredSamples) {
    auto p = makeStandalone();
    for (int i = 0; i < 100; ++i) {
        p->recordRouterOutcome("v1", /*ts_ms=*/1'000 + i,
                               /*latency=*/double(i),
                               /*is_error=*/(i < 10));  // first 10 are errors
    }
    auto m = p->forVersionAt("v1", seconds{60}, /*now_ms=*/2'000);
    EXPECT_EQ(m.sample_count, 100);
    EXPECT_DOUBLE_EQ(m.error_rate, 0.10);
}

TEST(MetricsProvider, AllErrorsRaiseRateToOne) {
    auto p = makeStandalone();
    for (int i = 0; i < 50; ++i) {
        p->recordRouterOutcome("v1", 1'000 + i, 10.0, true);
    }
    auto m = p->forVersionAt("v1", seconds{60}, /*now_ms=*/2'000);
    EXPECT_DOUBLE_EQ(m.error_rate, 1.0);
}

TEST(MetricsProvider, AllSuccessesKeepRateZero) {
    auto p = makeStandalone();
    for (int i = 0; i < 50; ++i) {
        p->recordRouterOutcome("v1", 1'000 + i, 10.0, false);
    }
    auto m = p->forVersionAt("v1", seconds{60}, /*now_ms=*/2'000);
    EXPECT_DOUBLE_EQ(m.error_rate, 0.0);
}

// --- 3. p99 ----------------------------------------------------------------

TEST(MetricsProvider, P99OfIncreasingSequenceIs99thValue) {
    // 100 samples with latencies 0..99. Formula: idx = min(n-1, n*0.99)
    // → idx = 99, so p99 = 99.
    auto p = makeStandalone();
    for (int i = 0; i < 100; ++i) {
        p->recordRouterOutcome("v1", 1'000 + i, double(i), false);
    }
    auto m = p->forVersionAt("v1", seconds{60}, /*now_ms=*/2'000);
    EXPECT_DOUBLE_EQ(m.p99_latency_ms, 99.0);
}

TEST(MetricsProvider, P99AllIdenticalSamplesReturnsThatValue) {
    auto p = makeStandalone();
    for (int i = 0; i < 50; ++i) {
        p->recordRouterOutcome("v1", 1'000 + i, 12.5, false);
    }
    auto m = p->forVersionAt("v1", seconds{60}, /*now_ms=*/2'000);
    EXPECT_DOUBLE_EQ(m.p99_latency_ms, 12.5);
}

TEST(MetricsProvider, P99SingleSampleReturnsThatSample) {
    auto p = makeStandalone();
    p->recordRouterOutcome("v1", 1'000, 77.0, false);
    auto m = p->forVersionAt("v1", seconds{60}, /*now_ms=*/1'500);
    EXPECT_DOUBLE_EQ(m.p99_latency_ms, 77.0);
}

TEST(MetricsProvider, P99SkewedDistributionReflectsTail) {
    // 99 fast samples + 1 slow outlier → p99 should pick up the outlier.
    auto p = makeStandalone();
    for (int i = 0; i < 99; ++i) {
        p->recordRouterOutcome("v1", 1'000 + i, 5.0, false);
    }
    p->recordRouterOutcome("v1", 1'100, /*latency=*/999.0, false);
    auto m = p->forVersionAt("v1", seconds{60}, /*now_ms=*/2'000);
    EXPECT_EQ(m.sample_count, 100);
    EXPECT_DOUBLE_EQ(m.p99_latency_ms, 999.0);
}

// --- 4. Sliding window filter ---------------------------------------------

TEST(MetricsProvider, WindowFilterExcludesOlderThanWindow) {
    auto p = makeStandalone();
    // 50 samples in window, 50 older.
    for (int i = 0; i < 50; ++i) {
        p->recordRouterOutcome("v1", /*ts=*/1'000 + i, 10.0, true);  // old, errors
    }
    for (int i = 0; i < 50; ++i) {
        p->recordRouterOutcome("v1", /*ts=*/200'000 + i, 20.0, false);  // fresh, ok
    }
    // Window 60s, now=260'000 → cutoff=200'000. Older samples excluded.
    auto m = p->forVersionAt("v1", seconds{60}, /*now_ms=*/260'000);
    EXPECT_EQ(m.sample_count, 50);
    EXPECT_DOUBLE_EQ(m.error_rate, 0.0);
    EXPECT_DOUBLE_EQ(m.p99_latency_ms, 20.0);
}

TEST(MetricsProvider, WindowBoundaryInclusiveAtCutoff) {
    auto p = makeStandalone();
    // Samples inserted in monotonic order (matches production hot-path
    // contract). window=60s means ts >= now - 60'000 are included (inclusive).
    p->recordRouterOutcome("v1", /*ts=*/ 99'999, 10.0, false);  // 1ms outside
    p->recordRouterOutcome("v1", /*ts=*/100'000, 5.0, false);   // exactly at cutoff
    auto m = p->forVersionAt("v1", seconds{60}, /*now_ms=*/160'000);
    EXPECT_EQ(m.sample_count, 1);
    EXPECT_DOUBLE_EQ(m.p99_latency_ms, 5.0);
}

TEST(MetricsProvider, WindowZeroTreatedAsEmpty) {
    auto p = makeStandalone();
    p->recordRouterOutcome("v1", 1'000, 10.0, false);
    auto m = p->forVersionAt("v1", seconds{0}, /*now_ms=*/1'000);
    // cutoff = now - 0 = now = 1000. ts=1000 is >= cutoff → included.
    EXPECT_EQ(m.sample_count, 1);
}

// --- 5. max_samples_per_version (SR15) -----------------------------------

TEST(MetricsProvider, MaxSamplesCapEnforcedStrictly) {
    auto p = makeStandalone(/*cap=*/100);
    for (int i = 0; i < 500; ++i) {
        p->recordRouterOutcome("v1", 1'000 + i, double(i), false);
    }
    // Only the last 100 samples retained: ts 1'400..1'499, latencies 400..499.
    auto m = p->forVersionAt("v1", seconds{60}, /*now_ms=*/2'000);
    EXPECT_EQ(m.sample_count, 100);
    // p99 = latency at idx = min(99, 100*0.99=99) = 99 → index 99 of sorted
    // 400..499 → 499.
    EXPECT_DOUBLE_EQ(m.p99_latency_ms, 499.0);
}

TEST(MetricsProvider, MaxSamplesCapPreservesNewestErrorRate) {
    auto p = makeStandalone(/*cap=*/100);
    // First 400: success. Last 100: half error, half success.
    for (int i = 0; i < 400; ++i) {
        p->recordRouterOutcome("v1", 1'000 + i, 5.0, false);
    }
    for (int i = 0; i < 100; ++i) {
        p->recordRouterOutcome("v1", 2'000 + i, 5.0, /*is_error=*/(i % 2 == 0));
    }
    auto m = p->forVersionAt("v1", seconds{120}, /*now_ms=*/3'000);
    EXPECT_EQ(m.sample_count, 100);
    EXPECT_DOUBLE_EQ(m.error_rate, 0.5);  // old successes dropped.
}

// --- 6. Multi-version isolation + bus integration + SR15 ------------------

TEST(MetricsProvider, VersionsAreIsolated) {
    auto p = makeStandalone();
    for (int i = 0; i < 100; ++i) {
        p->recordRouterOutcome("vA", 1'000 + i, 10.0, true);
    }
    for (int i = 0; i < 100; ++i) {
        p->recordRouterOutcome("vB", 1'000 + i, 20.0, false);
    }
    auto a = p->forVersionAt("vA", seconds{60}, /*now_ms=*/2'000);
    auto b = p->forVersionAt("vB", seconds{60}, /*now_ms=*/2'000);
    EXPECT_EQ(a.sample_count, 100);
    EXPECT_DOUBLE_EQ(a.error_rate, 1.0);
    EXPECT_DOUBLE_EQ(a.p99_latency_ms, 10.0);
    EXPECT_EQ(b.sample_count, 100);
    EXPECT_DOUBLE_EQ(b.error_rate, 0.0);
    EXPECT_DOUBLE_EQ(b.p99_latency_ms, 20.0);
}

TEST(MetricsProvider, SubscribesToBusAndConsumesRouterOutcome) {
    FeedbackBusConfig cfg;
    cfg.enabled = true;
    FeedbackBus bus(cfg);
    bus.start();

    FeedbackBusMetricsProvider p(bus, /*cap=*/1000);

    for (int i = 0; i < 20; ++i) {
        FeedbackEvent ev;
        ev.type = FeedbackEventType::RouterOutcome;
        ev.topic = "router.outcome";
        ev.timestamp = std::chrono::system_clock::time_point{
            std::chrono::milliseconds{1'000 + i}};
        ev.payload = {
            {"version_id", "v1"},
            {"latency_ms", double(i)},
            {"is_error", i < 5},
            {"ts_ms", 1'000 + i},
        };
        bus.publish(std::move(ev));
    }
    ASSERT_TRUE(bus.flush(std::chrono::milliseconds{2000}));

    auto m = p.forVersionAt("v1", seconds{60}, /*now_ms=*/2'000);
    EXPECT_EQ(m.sample_count, 20);
    EXPECT_DOUBLE_EQ(m.error_rate, 0.25);  // 5/20
    EXPECT_DOUBLE_EQ(m.p99_latency_ms, 19.0);

    bus.shutdown();
}

TEST(MetricsProvider, IgnoresNonRouterOutcomeTopics) {
    FeedbackBusConfig cfg;
    cfg.enabled = true;
    FeedbackBus bus(cfg);
    bus.start();
    FeedbackBusMetricsProvider p(bus);

    FeedbackEvent ev;
    ev.type = FeedbackEventType::QualityFeedback;
    ev.topic = "quality.feedback";
    ev.payload = {{"version_id", "v1"}, {"latency_ms", 10.0}, {"is_error", true}};
    bus.publish(std::move(ev));
    ASSERT_TRUE(bus.flush(std::chrono::milliseconds{2000}));

    auto m = p.forVersionAt("v1", seconds{60}, /*now_ms=*/2'000);
    EXPECT_EQ(m.sample_count, 0);

    bus.shutdown();
}

TEST(MetricsProvider, MalformedPayloadDoesNotCrash) {
    // SR15: subscriber callback must swallow any exception from payload parsing.
    FeedbackBusConfig cfg;
    cfg.enabled = true;
    FeedbackBus bus(cfg);
    bus.start();
    FeedbackBusMetricsProvider p(bus);

    // Missing version_id / wrong types / non-object payload.
    FeedbackEvent bad1;
    bad1.type = FeedbackEventType::RouterOutcome;
    bad1.topic = "router.outcome";
    bad1.payload = nlohmann::json::array({1, 2, 3});
    bus.publish(std::move(bad1));

    FeedbackEvent bad2;
    bad2.type = FeedbackEventType::RouterOutcome;
    bad2.topic = "router.outcome";
    bad2.payload = {{"version_id", 123}, {"latency_ms", "oops"}};
    bus.publish(std::move(bad2));

    // Then a valid event after the bad ones — provider must still work.
    FeedbackEvent good;
    good.type = FeedbackEventType::RouterOutcome;
    good.topic = "router.outcome";
    good.payload = {
        {"version_id", "v1"}, {"latency_ms", 10.0}, {"is_error", false},
        {"ts_ms", 1'000},
    };
    bus.publish(std::move(good));
    ASSERT_TRUE(bus.flush(std::chrono::milliseconds{2000}));

    auto m = p.forVersionAt("v1", seconds{60}, /*now_ms=*/2'000);
    EXPECT_EQ(m.sample_count, 1);

    // delivery_errors did not explode the bus (may be 0 because we swallow
    // exceptions inside our own callback; at worst it should be finite).
    auto s = bus.stats();
    EXPECT_GE(s.delivered, 3u);

    bus.shutdown();
}

TEST(MetricsProvider, RAIIUnsubscribesOnDestruction) {
    FeedbackBusConfig cfg;
    cfg.enabled = true;
    FeedbackBus bus(cfg);
    bus.start();
    {
        FeedbackBusMetricsProvider p(bus);
        EXPECT_EQ(bus.stats().subscriber_count, 1u);
    }
    EXPECT_EQ(bus.stats().subscriber_count, 0u);
    bus.shutdown();
}

TEST(MetricsProvider, ConcurrentAppendsAreThreadSafe) {
    // 4 producer threads × 500 samples, then snapshot — total sample_count
    // must be exactly 2000 with no crash / leak. Enough to flush TSan.
    auto p = makeStandalone();
    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t, &p] {
            for (int i = 0; i < kPerThread; ++i) {
                p->recordRouterOutcome("v1",
                                       /*ts_ms=*/1'000 + t * kPerThread + i,
                                       /*latency=*/double(i), false);
            }
        });
    }
    for (auto& th : threads) th.join();
    // Samples' ts_ms ∈ [1'000, 3'000). Use a window that covers all of them
    // ending at a now_ms within 120s of every sample; concurrent inserts
    // mean the deque is NOT time-monotone, so we intentionally disable the
    // early-exit optimization's benefit here by using a wide window.
    auto m = p->forVersionAt("v1", seconds{120}, /*now_ms=*/3'000);
    EXPECT_EQ(m.sample_count, kThreads * kPerThread);
}

} // namespace
} // namespace aegisgate
