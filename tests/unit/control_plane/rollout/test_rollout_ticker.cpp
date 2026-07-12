// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic B.4.
//
// Covers CR3 creative design (single-threaded tick loop + Clock injection):
//   memory-bank/creative/creative-rollout-ticker.md
//
// Tests split across:
//   1. tickOnce — sync path used by all higher-level tests.
//   2. Exception isolation — handler throws must not crash the ticker.
//   3. Clock argument pass-through (steady vs wall).
//   4. start/stop idempotence + production-style SystemClock integration.

#include "control_plane/rollout/rollout_ticker.h"
#include "common/clock.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace aegisgate {
namespace {

using namespace std::chrono_literals;
using aegisgate::common::FakeClock;
using aegisgate::common::SystemClock;

// Captures every call so tests can assert on count + arguments.
struct CaptureHandler : RolloutTickHandler {
    std::atomic<int> calls{0};
    std::atomic<std::int64_t> last_steady_ms{0};
    std::atomic<std::int64_t> last_wall_ms{0};
    std::atomic<bool> should_throw{false};

    void onTick(std::int64_t steady_ms, std::int64_t wall_ms) override {
        last_steady_ms.store(steady_ms);
        last_wall_ms.store(wall_ms);
        calls.fetch_add(1);
        if (should_throw.load()) throw std::runtime_error("boom");
    }
};

// --- 1. tickOnce ---------------------------------------------------------

TEST(RolloutTicker, TickOnceInvokesHandlerExactlyOnce) {
    FakeClock fc(/*init=*/1000);
    CaptureHandler h;
    RolloutTicker t(fc, h, 1s);
    t.tickOnce();
    EXPECT_EQ(h.calls.load(), 1);
    EXPECT_EQ(h.last_steady_ms.load(), 1000);
    EXPECT_EQ(h.last_wall_ms.load(), 1000);
}

TEST(RolloutTicker, TickOncePassesAdvancedTime) {
    FakeClock fc(/*init=*/1000);
    CaptureHandler h;
    RolloutTicker t(fc, h, 1s);

    t.tickOnce();
    EXPECT_EQ(h.last_steady_ms.load(), 1000);

    fc.advance(5s);
    t.tickOnce();
    EXPECT_EQ(h.last_steady_ms.load(), 6000);
    EXPECT_EQ(h.calls.load(), 2);
}

// --- 2. Exception isolation ---------------------------------------------

TEST(RolloutTicker, TickOnceSwallowsHandlerException) {
    FakeClock fc;
    CaptureHandler h;
    h.should_throw.store(true);
    RolloutTicker t(fc, h, 1s);
    EXPECT_NO_THROW(t.tickOnce());
    EXPECT_EQ(h.calls.load(), 1);  // still counted (increment before throw)
}

TEST(RolloutTicker, HandlerThrowDoesNotLeakAcrossTicks) {
    FakeClock fc;
    CaptureHandler h;
    RolloutTicker t(fc, h, 1s);
    h.should_throw.store(true);
    t.tickOnce();
    h.should_throw.store(false);
    t.tickOnce();
    EXPECT_EQ(h.calls.load(), 2);
}

// --- 3. Clock pass-through ----------------------------------------------

TEST(RolloutTicker, SteadyAndWallMayDiffer) {
    // FakeClock collapses them, but SystemClock keeps them distinct.
    SystemClock sc;
    CaptureHandler h;
    RolloutTicker t(sc, h, 1s);
    t.tickOnce();
    EXPECT_GT(h.last_steady_ms.load(), 0);
    EXPECT_GT(h.last_wall_ms.load(), 1'000'000'000'000);  // wall ms > 2001-09-09
}

// --- 4. start/stop -------------------------------------------------------

TEST(RolloutTicker, StartAndStopProduceAtLeastOneTick) {
    SystemClock sc;
    CaptureHandler h;
    RolloutTicker t(sc, h, 10ms);  // tight tick so we observe quickly
    t.start();
    // Wait until at least 3 ticks observed or 500ms elapsed.
    auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (h.calls.load() < 3 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    t.stop(1s);
    EXPECT_GE(h.calls.load(), 1);  // at least 1 tick fired
}

TEST(RolloutTicker, StartIsIdempotent) {
    SystemClock sc;
    CaptureHandler h;
    RolloutTicker t(sc, h, 10ms);
    t.start();
    t.start();  // second call must be a no-op, not spawn a second thread
    std::this_thread::sleep_for(30ms);
    t.stop(1s);
    // No crash / no hang ⇒ pass. The handler MAY have been invoked; we
    // don't assert on exact count (scheduling is nondeterministic).
    SUCCEED();
}

TEST(RolloutTicker, StopIsIdempotent) {
    SystemClock sc;
    CaptureHandler h;
    RolloutTicker t(sc, h, 10ms);
    t.start();
    std::this_thread::sleep_for(30ms);
    t.stop(1s);
    t.stop(1s);  // no-op
    SUCCEED();
}

TEST(RolloutTicker, StopWithoutStartIsNoop) {
    SystemClock sc;
    CaptureHandler h;
    RolloutTicker t(sc, h, 10ms);
    t.stop(1s);  // must not hang
    EXPECT_EQ(h.calls.load(), 0);
}

TEST(RolloutTicker, DestructorStopsBackgroundLoop) {
    SystemClock sc;
    CaptureHandler h;
    {
        RolloutTicker t(sc, h, 10ms);
        t.start();
        std::this_thread::sleep_for(30ms);
    }  // ~dtor — must join cleanly (or detach within 2s)
    auto calls_at_dtor = h.calls.load();
    std::this_thread::sleep_for(50ms);
    // No more ticks should occur after destruction.
    EXPECT_EQ(h.calls.load(), calls_at_dtor);
}

} // namespace
} // namespace aegisgate
