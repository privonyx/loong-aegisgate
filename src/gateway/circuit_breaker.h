#pragma once
#include <string>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <list>
#include <array>
#include <functional>

namespace aegisgate {

#ifdef AEGISGATE_ENABLE_REDIS
class RedisStateStore;
#endif

enum class CircuitState { Closed, Open, HalfOpen };

struct CircuitConfig {
    int failure_threshold = 3;
    std::chrono::seconds reset_timeout{30};
    int half_open_max_calls = 1;
    size_t max_circuits = 512;
    /// Per-entry idle TTL; 0 disables TTL eviction (LRU still applies when full).
    std::chrono::seconds circuit_idle_ttl{0};
};

class CircuitBreaker {
public:
    explicit CircuitBreaker(CircuitConfig config = {});

    bool allowRequest(const std::string& model);
    void recordSuccess(const std::string& model);
    void recordFailure(const std::string& model);
    // REV20260707-S4 D1 Option B: state()/exportMetrics() apply the same
    // Open->HalfOpen timeout transition as allowRequest() via the private
    // evaluateEffectiveState() helper. Both are now non-const because the
    // effective-state evaluation mutates the stored circuit in-place.
    CircuitState state(const std::string& model);
    void exportMetrics(class Gauge& gauge);

#ifdef AEGISGATE_ENABLE_REDIS
    void setRedisStateStore(RedisStateStore* store) { redis_store_ = store; }
#endif

private:
    struct ModelCircuit {
        CircuitState state = CircuitState::Closed;
        int failure_count = 0;
        int half_open_calls = 0;
        std::chrono::steady_clock::time_point last_failure;
    };

    struct CircuitEntry {
        ModelCircuit circuit;
        std::chrono::steady_clock::time_point last_access{};
        std::list<std::string>::iterator lru_it{};
    };

    static constexpr size_t kNumShards = 16;

    struct Shard {
        mutable std::mutex mutex; // Lock Layer 1 — see docs/LOCK_ORDERING.md
        std::list<std::string> lru;
        std::unordered_map<std::string, CircuitEntry> circuits;
    };

    Shard& shardFor(const std::string& model);
    const Shard& shardFor(const std::string& model) const;
    ModelCircuit& circuitFor(Shard& shard, const std::string& model); // caller must hold shard.mutex
    // REV20260707-S4 D1 Option B: applies the Open->HalfOpen timeout
    // transition in-place; caller must hold shard.mutex. Used by
    // allowRequest / state / exportMetrics for consistent effective-state
    // semantics.
    ModelCircuit& evaluateEffectiveState(Shard& shard, const std::string& model);

    CircuitConfig config_;
    size_t per_shard_cap_;
    std::array<Shard, kNumShards> shards_;
#ifdef AEGISGATE_ENABLE_REDIS
    RedisStateStore* redis_store_ = nullptr;
#endif
};

} // namespace aegisgate
