#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <chrono>

namespace aegisgate {

struct KeyState {
    std::string key;
    int weight = 1;
    int cw = 0; // smooth WRR current weight
    bool healthy = true;
    int consecutive_failures = 0;
    std::chrono::steady_clock::time_point cooldown_until;
};

class Balancer {
public:
    explicit Balancer(const std::vector<std::pair<std::string, int>>& keys);

    std::string nextKey();
    void reportSuccess(const std::string& key);
    void reportFailure(const std::string& key);
    size_t healthyCount() const;

    static constexpr int kMaxConsecutiveFailures = 3;
    static constexpr int kCooldownSeconds = 30;

private:
    std::vector<KeyState> keys_;
    std::unordered_map<std::string, size_t> key_index_;
    mutable std::mutex mutex_; // Lock Layer 1 — see docs/LOCK_ORDERING.md
};

} // namespace aegisgate
