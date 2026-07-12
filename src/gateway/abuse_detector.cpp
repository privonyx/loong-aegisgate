#include "gateway/abuse_detector.h"
#include "gateway/simhash.h"
#ifdef AEGISGATE_ENABLE_REDIS
#include "cluster/redis_state_store.h"
#endif
#include <spdlog/spdlog.h>

namespace aegisgate {

AbuseDetector::AbuseDetector(const Config& config) : config_(config) {}

size_t AbuseDetector::shardFor(const std::string& key) const {
    return std::hash<std::string>{}(key) % kShardCount;
}

void AbuseDetector::pruneExpired(SlidingWindow& window) const {
    auto cutoff = std::chrono::steady_clock::now() -
                  std::chrono::seconds(config_.window_seconds);
    while (!window.timestamps.empty() && window.timestamps.front() < cutoff) {
        window.timestamps.pop_front();
    }
}

int AbuseDetector::countInWindow(const SlidingWindow& window) const {
    auto cutoff = std::chrono::steady_clock::now() -
                  std::chrono::seconds(config_.window_seconds);
    int count = 0;
    for (const auto& ts : window.timestamps) {
        if (ts >= cutoff) ++count;
    }
    return count;
}

void AbuseDetector::ensureKeyCapacity(Shard& shard, const std::string& api_key) {
    if (shard.windows.find(api_key) != shard.windows.end()) return;
    if (shard.windows.size() < config_.max_keys_per_shard) return;

    std::string oldest_key;
    auto oldest_time = std::chrono::steady_clock::time_point::max();
    for (const auto& [k, w] : shard.windows) {
        auto last = w.timestamps.empty()
            ? std::chrono::steady_clock::time_point{}
            : w.timestamps.back();
        if (last < oldest_time) {
            oldest_time = last;
            oldest_key = k;
        }
    }
    if (!oldest_key.empty()) {
        shard.windows.erase(oldest_key);
    }
}

void AbuseDetector::pushFingerprint(SlidingWindow& window, uint64_t hash) {
    window.fingerprints.push_back(hash);
    while (window.fingerprints.size() > config_.similarity_max_fingerprints) {
        window.fingerprints.pop_front();
    }
}

AbuseDetector::Action AbuseDetector::actionFromCount(
    int count, const SlidingWindow* window) const {
    if (window && window->blocked_until > std::chrono::steady_clock::now()) {
        return Action::Block;
    }
    if (count >= config_.block_threshold) return Action::Block;
    if (count >= config_.throttle_threshold) return Action::Throttle;
    if (count >= config_.warn_threshold) return Action::Warn;
    return Action::Allow;
}

AbuseDetector::Action AbuseDetector::observe(const std::string& api_key,
                                             std::string_view content) {
    std::string_view trimmed = content;
    if (trimmed.size() > config_.similarity_max_content_bytes) {
        trimmed = trimmed.substr(0, config_.similarity_max_content_bytes);
    }

    bool similarity_hit = false;
    int hit_distance = 0;

    if (config_.similarity_enabled && !trimmed.empty()) {
        const uint64_t hash = simhash64(trimmed);

        auto& shard = shards_[shardFor(api_key)];
        std::lock_guard<std::mutex> lock(shard.mutex);
        ensureKeyCapacity(shard, api_key);
        auto& window = shard.windows[api_key];

        for (uint64_t fp : window.fingerprints) {
            int d = hamming64(hash, fp);
            if (d <= config_.similarity_hamming_threshold) {
                similarity_hit = true;
                hit_distance = d;
                break;
            }
        }

        if (similarity_hit) {
            window.timestamps.push_back(std::chrono::steady_clock::now());
            pruneExpired(window);
            if (static_cast<int>(window.timestamps.size()) >= config_.block_threshold &&
                window.blocked_until <= std::chrono::steady_clock::now()) {
                window.blocked_until = std::chrono::steady_clock::now() +
                    std::chrono::seconds(config_.block_duration_seconds);
            }
            spdlog::warn("AbuseDetector: similarity hit key='{}...' hamming={}",
                         api_key.substr(0, std::min<size_t>(8, api_key.size())),
                         hit_distance);
        }
        pushFingerprint(window, hash);
    }

#ifdef AEGISGATE_ENABLE_REDIS
    if (redis_store_) {
        if (similarity_hit) {
            redis_store_->abuseRecordRejection(api_key, config_.window_seconds);
            int count = redis_store_->abuseGetCount(api_key, config_.window_seconds);
            if (count >= config_.block_threshold &&
                !redis_store_->abuseIsBlocked(api_key)) {
                redis_store_->abuseSetBlocked(api_key,
                                             config_.block_duration_seconds);
            }
        }
        if (redis_store_->abuseIsBlocked(api_key)) return Action::Block;
        int count = redis_store_->abuseGetCount(api_key, config_.window_seconds);
        return actionFromCount(count, nullptr);
    }
#endif

    return getAction(api_key);
}

void AbuseDetector::recordRejection(const std::string& api_key) {
#ifdef AEGISGATE_ENABLE_REDIS
    if (redis_store_) {
        redis_store_->abuseRecordRejection(api_key, config_.window_seconds);
        int count = redis_store_->abuseGetCount(api_key, config_.window_seconds);
        if (count >= config_.block_threshold && !redis_store_->abuseIsBlocked(api_key)) {
            redis_store_->abuseSetBlocked(api_key, config_.block_duration_seconds);
            spdlog::warn("AbuseDetector(Redis): key '{}' blocked for {}s (rejections={})",
                         api_key.substr(0, 8) + "...", config_.block_duration_seconds, count);
        }
        return;
    }
#endif
    auto& shard = shards_[shardFor(api_key)];
    std::lock_guard<std::mutex> lock(shard.mutex);

    ensureKeyCapacity(shard, api_key);

    auto& window = shard.windows[api_key];
    window.timestamps.push_back(std::chrono::steady_clock::now());
    pruneExpired(window);

    int count = static_cast<int>(window.timestamps.size());
    if (count >= config_.block_threshold &&
        window.blocked_until <= std::chrono::steady_clock::now()) {
        window.blocked_until = std::chrono::steady_clock::now() +
                               std::chrono::seconds(config_.block_duration_seconds);
        spdlog::warn("AbuseDetector: key '{}' blocked for {}s (rejections={})",
                     api_key.substr(0, 8) + "...", config_.block_duration_seconds, count);
    }
}

AbuseDetector::Action AbuseDetector::getAction(const std::string& api_key) const {
#ifdef AEGISGATE_ENABLE_REDIS
    if (redis_store_) {
        if (redis_store_->abuseIsBlocked(api_key)) return Action::Block;
        int count = redis_store_->abuseGetCount(api_key, config_.window_seconds);
        return actionFromCount(count, nullptr);
    }
#endif
    auto& shard = shards_[shardFor(api_key)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.windows.find(api_key);
    if (it == shard.windows.end()) return Action::Allow;

    auto& window = it->second;
    int count = countInWindow(window);
    return actionFromCount(count, &window);
}

int AbuseDetector::rejectionCount(const std::string& api_key) const {
#ifdef AEGISGATE_ENABLE_REDIS
    if (redis_store_) {
        return redis_store_->abuseGetCount(api_key, config_.window_seconds);
    }
#endif
    auto& shard = shards_[shardFor(api_key)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.windows.find(api_key);
    if (it == shard.windows.end()) return 0;
    return countInWindow(it->second);
}

} // namespace aegisgate
