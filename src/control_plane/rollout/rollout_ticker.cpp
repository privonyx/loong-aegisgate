#include "control_plane/rollout/rollout_ticker.h"

#include <spdlog/spdlog.h>
#include <chrono>
#include <exception>
#include <future>

namespace aegisgate {

RolloutTicker::RolloutTicker(common::Clock& clock,
                             RolloutTickHandler& handler,
                             std::chrono::milliseconds interval)
    : clock_(clock), handler_(handler), interval_(interval) {
    if (interval_.count() <= 0) {
        interval_ = std::chrono::seconds(1);
    }
}

RolloutTicker::~RolloutTicker() { stop(std::chrono::seconds(2)); }

void RolloutTicker::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
        return;  // already running — idempotent
    }
    stopping_.store(false, std::memory_order_release);
    th_ = std::thread([this]() { loop(); });
}

void RolloutTicker::stop(std::chrono::seconds max_wait) {
    if (!started_.exchange(false, std::memory_order_acq_rel)) return;
    stopping_.store(true, std::memory_order_release);
    clock_.notify();
    if (!th_.joinable()) return;

    // std::thread has no native timed_join — wrap join() in an async
    // helper so we can enforce max_wait and detach on timeout.
    auto join_future = std::async(std::launch::async,
                                   [this]() { th_.join(); });
    if (join_future.wait_for(max_wait) != std::future_status::ready) {
        spdlog::warn("rollout-ticker: stop timeout after {}s, detaching thread",
                      max_wait.count());
        if (th_.joinable()) th_.detach();
        // join_future's thread will still try to join; that's fine
        // because detach transfers ownership cleanly.
    }
}

void RolloutTicker::tickOnce() noexcept {
    try {
        handler_.onTick(clock_.nowMillis(), clock_.wallClockMillis());
    } catch (const std::exception& e) {
        spdlog::error("rollout-ticker: onTick exception: {}", e.what());
    } catch (...) {
        spdlog::error("rollout-ticker: onTick unknown exception");
    }
}

void RolloutTicker::loop() noexcept {
    while (!stopping_.load(std::memory_order_acquire)) {
        tickOnce();
        if (stopping_.load(std::memory_order_acquire)) break;
        clock_.waitFor(interval_, [this]() {
            return stopping_.load(std::memory_order_acquire);
        });
    }
}

} // namespace aegisgate
