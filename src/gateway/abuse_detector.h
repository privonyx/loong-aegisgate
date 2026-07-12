#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <array>
#include <chrono>
#include <cstdint>

namespace aegisgate {

#ifdef AEGISGATE_ENABLE_REDIS
class RedisStateStore;
#endif

class AbuseDetector {
public:
    enum class Action { Allow, Warn, Throttle, Block };

    struct Config {
        int window_seconds = 300;
        int warn_threshold = 5;
        int throttle_threshold = 10;
        int block_threshold = 20;
        int block_duration_seconds = 1800;
        double throttle_factor = 0.5;
        size_t max_keys_per_shard = 1024;
        bool similarity_enabled = true;
        int similarity_hamming_threshold = 3;
        size_t similarity_max_fingerprints = 32;
        size_t similarity_max_content_bytes = 8192;
    };

    explicit AbuseDetector(const Config& config);

    // Production path: fingerprint + optional similarity hit, then Action.
    Action observe(const std::string& api_key, std::string_view content);

    void recordRejection(const std::string& api_key);
    Action getAction(const std::string& api_key) const;
    int rejectionCount(const std::string& api_key) const;
    // P1-C: exposed so the gateway can translate a Throttle decision into a
    // tighter rate-limit budget (cost multiplier = 1 / throttle_factor).
    double throttleFactor() const { return config_.throttle_factor; }
    size_t similarityMaxContentBytes() const {
        return config_.similarity_max_content_bytes;
    }

#ifdef AEGISGATE_ENABLE_REDIS
    void setRedisStateStore(RedisStateStore* store) { redis_store_ = store; }
#endif

    static constexpr size_t kShardCount = 16;

private:
    struct SlidingWindow {
        std::deque<std::chrono::steady_clock::time_point> timestamps;
        std::chrono::steady_clock::time_point blocked_until{};
        std::deque<uint64_t> fingerprints;
    };

    struct Shard {  // Lock Layer 1 — see docs/LOCK_ORDERING.md
        mutable std::mutex mutex;
        std::unordered_map<std::string, SlidingWindow> windows;
    };

    size_t shardFor(const std::string& key) const;
    void pruneExpired(SlidingWindow& window) const;  // caller must hold shard.mutex
    int countInWindow(const SlidingWindow& window) const;  // caller must hold shard.mutex
    void ensureKeyCapacity(Shard& shard, const std::string& api_key);  // caller holds lock
    void pushFingerprint(SlidingWindow& window, uint64_t hash);  // caller holds lock
    Action actionFromCount(int count, const SlidingWindow* window) const;

    Config config_;
    mutable std::array<Shard, kShardCount> shards_;
#ifdef AEGISGATE_ENABLE_REDIS
    RedisStateStore* redis_store_ = nullptr;
#endif
};

} // namespace aegisgate
