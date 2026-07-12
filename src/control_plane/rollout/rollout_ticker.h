#pragma once

// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic B.4.
//
// CR3 creative design — single-threaded tick loop + external Clock injection
// at tick=1s. See memory-bank/creative/creative-rollout-ticker.md.
//
// Why a dedicated handler interface instead of a direct RolloutController
// dependency: B.4 is implemented strictly before B.6 (controller), and
// breaking the dependency on the concrete controller lets us land + test
// the ticker plumbing in isolation. B.6 will simply implement
// RolloutTickHandler on the controller and wire it through.

#include "common/clock.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace aegisgate {

// Invoked by the ticker on each tick and by tests via tickOnce().
// Implementations should enumerate active rollouts and evaluate each;
// any exception thrown is caught and logged by the ticker, never
// propagated to the background thread — a single misbehaving rollout
// cannot wedge the whole evaluator.
class RolloutTickHandler {
public:
    virtual ~RolloutTickHandler() = default;
    virtual void onTick(std::int64_t steady_now_ms,
                         std::int64_t wall_now_ms) = 0;
};

class RolloutTicker {
public:
    RolloutTicker(common::Clock& clock,
                   RolloutTickHandler& handler,
                   std::chrono::milliseconds interval = std::chrono::seconds(1));

    ~RolloutTicker();

    RolloutTicker(const RolloutTicker&) = delete;
    RolloutTicker& operator=(const RolloutTicker&) = delete;

    // Idempotent. After start() the background thread runs the loop and
    // invokes handler_.onTick() every `interval_` until stop().
    void start();

    // Idempotent. Signals stopping_, wakes waitFor(), and joins within
    // max_wait. On timeout the thread is detached (last-resort safety
    // so the main server shutdown path is never blocked).
    void stop(std::chrono::seconds max_wait = std::chrono::seconds(5));

    // Synchronous tick. Used by tests and by the loop itself. Swallows
    // exceptions from the handler.
    void tickOnce() noexcept;

private:
    void loop() noexcept;

    common::Clock&              clock_;
    RolloutTickHandler&         handler_;
    std::chrono::milliseconds   interval_;
    std::atomic<bool>           started_{false};
    std::atomic<bool>           stopping_{false};
    std::thread                 th_;
};

} // namespace aegisgate
