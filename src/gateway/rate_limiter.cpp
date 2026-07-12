#include "rate_limiter.h"
#ifdef AEGISGATE_ENABLE_REDIS
#include "cluster/redis_state_store.h"
#endif
#include <algorithm>
#include <functional>

namespace aegisgate {

RateLimiter::RateLimiter(const Config& global_config)
    : global_config_(global_config) {}

size_t RateLimiter::shardFor(const std::string& key) const {
    return std::hash<std::string>{}(key) % kShardCount;
}

bool RateLimiter::allow(const std::string& key, double cost) {
    if (cost <= 0.0) return true;
#ifdef AEGISGATE_ENABLE_REDIS
    if (redis_store_) {
        return redis_store_->rateLimitAllow(key, cost,
            global_config_.max_tokens, global_config_.refill_rate);
    }
#endif
    auto& shard = shards_[shardFor(key)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto& bucket = getBucket(shard, key);
    refill(bucket);

    if (bucket.tokens >= cost) {
        bucket.tokens -= cost;
        return true;
    }
    return false;
}

void RateLimiter::setKeyConfig(const std::string& key, const Config& config) {
    auto& shard = shards_[shardFor(key)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto now = std::chrono::steady_clock::now();
    shard.buckets[key] = {config.max_tokens, config.max_tokens,
                          config.refill_rate, now};
}

double RateLimiter::remaining(const std::string& key) const {
#ifdef AEGISGATE_ENABLE_REDIS
    if (redis_store_) {
        return redis_store_->rateLimitRemaining(key,
            global_config_.max_tokens, global_config_.refill_rate);
    }
#endif
    auto& shard = shards_[shardFor(key)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.buckets.find(key);
    if (it == shard.buckets.end()) return global_config_.max_tokens;
    return it->second.tokens;
}

void RateLimiter::refill(TokenBucket& bucket) const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(now - bucket.last_refill).count();
    bucket.tokens = std::min(bucket.max_tokens,
                              bucket.tokens + elapsed * bucket.refill_rate);
    bucket.last_refill = now;
}

TokenBucket& RateLimiter::getBucket(Shard& shard, const std::string& key) {
    auto it = shard.buckets.find(key);
    if (it == shard.buckets.end()) {
        maybePrune(shard);
        auto now = std::chrono::steady_clock::now();
        shard.buckets[key] = {global_config_.max_tokens, global_config_.max_tokens,
                              global_config_.refill_rate, now};
        return shard.buckets[key];
    }
    return it->second;
}

void RateLimiter::maybePrune(Shard& shard) {
    if (++shard.op_counter < kPruneIntervalOps &&
        shard.buckets.size() < kMaxBucketsPerShard) return;
    shard.op_counter = 0;

    auto now = std::chrono::steady_clock::now();
    for (auto it = shard.buckets.begin(); it != shard.buckets.end(); ) {
        auto age = std::chrono::duration<double>(now - it->second.last_refill).count();
        bool full = it->second.tokens >= it->second.max_tokens;
        if (full && age > 300.0) {
            it = shard.buckets.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace aegisgate
