#include "common/clock.h"

namespace aegisgate::common {

std::int64_t SystemClock::nowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

std::int64_t SystemClock::wallClockMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch())
        .count();
}

void SystemClock::waitFor(std::chrono::milliseconds d,
                          std::function<bool()> stop_pred) {
    std::unique_lock<std::mutex> lk(mu_);
    if (stop_pred && stop_pred()) return;
    cv_.wait_for(lk, d, [&]() { return stop_pred && stop_pred(); });
}

void SystemClock::notify() {
    std::lock_guard<std::mutex> lk(mu_);
    cv_.notify_all();
}

} // namespace aegisgate::common
