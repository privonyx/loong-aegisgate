#include "balancer.h"
#include <spdlog/spdlog.h>

namespace aegisgate {

Balancer::Balancer(const std::vector<std::pair<std::string, int>>& keys) {
    keys_.reserve(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        keys_.push_back({keys[i].first, keys[i].second, 0, true, 0, {}});
        key_index_[keys[i].first] = i;
    }
}

std::string Balancer::nextKey() {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();

    // Try auto-recovery for cooled-down keys
    for (auto& k : keys_) {
        if (!k.healthy && now >= k.cooldown_until) {
            spdlog::info("Key recovered from cooldown, re-enabling");
            k.healthy = true;
            k.consecutive_failures = 0;
            k.cw = 0;
        }
    }

    // Smooth weighted round-robin among healthy keys
    int total = 0;
    KeyState* best = nullptr;
    for (auto& k : keys_) {
        if (!k.healthy) continue;
        const int w = k.weight > 0 ? k.weight : 1;
        total += w;
        k.cw += w;
        if (!best || k.cw > best->cw) {
            best = &k;
        }
    }

    if (!best || total == 0) {
        spdlog::error("No healthy API keys available");
        return "";
    }

    best->cw -= total;
    return best->key;
}

void Balancer::reportSuccess(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = key_index_.find(key);
    if (it == key_index_.end()) return;
    auto& k = keys_[it->second];
    k.consecutive_failures = 0;
    k.healthy = true;
}

void Balancer::reportFailure(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = key_index_.find(key);
    if (it == key_index_.end()) return;
    auto& k = keys_[it->second];
    k.consecutive_failures++;
    if (k.consecutive_failures >= kMaxConsecutiveFailures) {
        k.healthy = false;
        k.cooldown_until = std::chrono::steady_clock::now() +
            std::chrono::seconds(kCooldownSeconds);
        spdlog::warn("Key marked unhealthy after {} consecutive failures",
                      k.consecutive_failures);
    }
}

size_t Balancer::healthyCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& k : keys_) {
        if (k.healthy) ++count;
    }
    return count;
}

} // namespace aegisgate
