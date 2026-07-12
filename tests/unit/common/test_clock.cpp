// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic B.4 Clock tests.

#include "common/clock.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

namespace aegisgate::common {
namespace {

using namespace std::chrono_literals;

// --- FakeClock -----------------------------------------------------------

TEST(FakeClock, DefaultStartsAtZero) {
    FakeClock fc;
    EXPECT_EQ(fc.nowMillis(), 0);
    EXPECT_EQ(fc.wallClockMillis(), 0);
}

TEST(FakeClock, AdvanceAccumulatesMillis) {
    FakeClock fc(/*init_ms=*/1000);
    fc.advance(500ms);
    EXPECT_EQ(fc.nowMillis(), 1500);
    fc.advance(1s);
    EXPECT_EQ(fc.nowMillis(), 2500);
    EXPECT_EQ(fc.wallClockMillis(), 2500);  // FakeClock wall == steady
}

TEST(FakeClock, SetNowJumpsAbsolute) {
    FakeClock fc;
    fc.setNow(42);
    EXPECT_EQ(fc.nowMillis(), 42);
}

TEST(FakeClock, WaitForIsNoOp) {
    FakeClock fc;
    auto start = std::chrono::steady_clock::now();
    fc.waitFor(1s, []() { return false; });
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, 50ms);  // no-op should return immediately
}

// --- SystemClock ---------------------------------------------------------

TEST(SystemClock, NowMillisMonotonicOverBackToBackCalls) {
    SystemClock sc;
    std::int64_t a = sc.nowMillis();
    std::int64_t b = sc.nowMillis();
    EXPECT_GE(b, a);
}

TEST(SystemClock, WallClockMillisIsCloseToSystemClock) {
    SystemClock sc;
    auto ref = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    auto got = sc.wallClockMillis();
    EXPECT_LT(std::abs(got - ref), 1000);  // sanity — within 1s
}

TEST(SystemClock, WaitForReturnsOnStopPredTrue) {
    SystemClock sc;
    auto start = std::chrono::steady_clock::now();
    sc.waitFor(10s, []() { return true; });
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, 100ms);
}

TEST(SystemClock, NotifyUnblocksWaitFor) {
    SystemClock sc;
    std::atomic<bool> stop{false};
    std::atomic<std::int64_t> woke_at{0};
    auto waiter = std::thread([&]() {
        sc.waitFor(10s, [&]() { return stop.load(); });
        woke_at.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count());
    });
    std::this_thread::sleep_for(20ms);
    auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
    stop.store(true);
    sc.notify();
    waiter.join();
    EXPECT_LT(woke_at.load() - before, 500);  // woke within 500ms of notify
}

TEST(SystemClock, WaitForRespectsTimeout) {
    SystemClock sc;
    auto start = std::chrono::steady_clock::now();
    sc.waitFor(50ms, []() { return false; });
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 40ms);
    EXPECT_LT(elapsed, 500ms);
}

} // namespace
} // namespace aegisgate::common
