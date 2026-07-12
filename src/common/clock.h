#pragma once

// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic B.4 support.
//
// CR3 creative design — Clock abstraction:
//   memory-bank/creative/creative-rollout-ticker.md §Implementation指导.
//
// Provides an injectable clock interface so the RolloutTicker (and any
// future time-driven service) can be tested 100% deterministically via
// FakeClock::advance() + tickOnce(), while production paths use the
// SystemClock with a condition_variable-backed waitFor() that supports
// stop_pred early-wake for graceful shutdown.
//
// Split of nowMillis vs wallClockMillis:
//   * nowMillis()       — steady_clock; used for deltas, timeouts,
//                         stage_started_at bookkeeping. Immune to wall
//                         clock jumps.
//   * wallClockMillis() — system_clock; used for persisted records
//                         (paused_at, completed_at) that must cross
//                         process restarts and align with audit events.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>

namespace aegisgate::common {

class Clock {
public:
    virtual ~Clock() = default;

    // Monotonic, unit = ms. Callers must not rely on absolute value.
    virtual std::int64_t nowMillis() = 0;

    // Wall clock ms since epoch; may jump.
    virtual std::int64_t wallClockMillis() = 0;

    // Block up to `d`, returning earlier if `stop_pred()` is already
    // true or notify() is called. Must not throw.
    virtual void waitFor(std::chrono::milliseconds d,
                          std::function<bool()> stop_pred) = 0;

    // Wake any thread currently blocked in waitFor.
    virtual void notify() = 0;
};

class SystemClock : public Clock {
public:
    std::int64_t nowMillis() override;
    std::int64_t wallClockMillis() override;
    void waitFor(std::chrono::milliseconds d,
                  std::function<bool()> stop_pred) override;
    void notify() override;

private:
    std::mutex              mu_;
    std::condition_variable cv_;
};

// Test clock. advance() drives nowMillis/wallClockMillis; waitFor is a
// no-op (tests drive time explicitly via advance()).
class FakeClock : public Clock {
public:
    explicit FakeClock(std::int64_t init_ms = 0) noexcept : now_ms_(init_ms) {}

    void advance(std::chrono::milliseconds d) noexcept {
        now_ms_.fetch_add(d.count(), std::memory_order_acq_rel);
    }
    void setNow(std::int64_t ms) noexcept {
        now_ms_.store(ms, std::memory_order_release);
    }

    std::int64_t nowMillis() override { return now_ms_.load(std::memory_order_acquire); }
    std::int64_t wallClockMillis() override { return nowMillis(); }
    void waitFor(std::chrono::milliseconds, std::function<bool()>) override {}
    void notify() override {}

private:
    std::atomic<std::int64_t> now_ms_;
};

} // namespace aegisgate::common
