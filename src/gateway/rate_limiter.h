#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <array>
#include <chrono>
#include <functional>

namespace aegisgate {

#ifdef AEGISGATE_ENABLE_REDIS
class RedisStateStore;
#endif

struct TokenBucket {
    double tokens;
    double max_tokens;
    double refill_rate; // tokens per second
    std::chrono::steady_clock::time_point last_refill;
};

class RateLimiter {
public:
    struct Config {
        double max_tokens = 100.0;
        double refill_rate = 10.0; // per second
    };

    explicit RateLimiter(const Config& global_config);

    bool allow(const std::string& key, double cost = 1.0);
    void setKeyConfig(const std::string& key, const Config& config);
    double remaining(const std::string& key) const;

#ifdef AEGISGATE_ENABLE_REDIS
    void setRedisStateStore(RedisStateStore* store) { redis_store_ = store; }
#endif

    static constexpr size_t kMaxBucketsPerShard = 10000;
    static constexpr int kPruneIntervalOps = 1000;
    static constexpr size_t kShardCount = 16;

private:
    struct Shard { // Lock Layer 1 — see docs/LOCK_ORDERING.md
        mutable std::mutex mutex;
        std::unordered_map<std::string, TokenBucket> buckets;
        int op_counter = 0;
    };

    size_t shardFor(const std::string& key) const;
    void refill(TokenBucket& bucket) const; // caller must hold shard.mutex
    TokenBucket& getBucket(Shard& shard, const std::string& key); // caller must hold shard.mutex
    void maybePrune(Shard& shard); // caller must hold shard.mutex

    Config global_config_;
    mutable std::array<Shard, kShardCount> shards_;
#ifdef AEGISGATE_ENABLE_REDIS
    RedisStateStore* redis_store_ = nullptr;
#endif
};

} // namespace aegisgate
