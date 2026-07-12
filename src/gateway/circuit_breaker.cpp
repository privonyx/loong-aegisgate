#include "circuit_breaker.h"
#ifdef AEGISGATE_ENABLE_REDIS
#include "cluster/redis_state_store.h"
#endif
#include "observe/metrics.h"
#include <algorithm>

namespace aegisgate {

CircuitBreaker::CircuitBreaker(CircuitConfig config)
    : config_(std::move(config)),
      per_shard_cap_((std::max<size_t>(1, config_.max_circuits) + kNumShards - 1) / kNumShards) {}

CircuitBreaker::Shard& CircuitBreaker::shardFor(const std::string& model) {
    return shards_[std::hash<std::string>{}(model) % kNumShards];
}

const CircuitBreaker::Shard& CircuitBreaker::shardFor(const std::string& model) const {
    return shards_[std::hash<std::string>{}(model) % kNumShards];
}

CircuitBreaker::ModelCircuit& CircuitBreaker::circuitFor(Shard& shard,
                                                          const std::string& model) {
    const auto now = std::chrono::steady_clock::now();
    auto it = shard.circuits.find(model);
    if (it != shard.circuits.end()) {
        if (config_.circuit_idle_ttl.count() > 0 &&
            now - it->second.last_access > config_.circuit_idle_ttl) {
            shard.lru.erase(it->second.lru_it);
            shard.circuits.erase(it);
            it = shard.circuits.end();
        } else {
            it->second.last_access = now;
            shard.lru.splice(shard.lru.end(), shard.lru, it->second.lru_it);
            return it->second.circuit;
        }
    }

    while (shard.circuits.size() >= per_shard_cap_ && !shard.lru.empty()) {
        const std::string victim = shard.lru.front();
        shard.lru.pop_front();
        shard.circuits.erase(victim);
    }

    shard.lru.push_back(model);
    auto lit = std::prev(shard.lru.end());
    CircuitEntry entry;
    entry.circuit = ModelCircuit{};
    entry.last_access = now;
    entry.lru_it = lit;
    auto [cit, _] = shard.circuits.emplace(model, std::move(entry));
    return cit->second.circuit;
}

CircuitBreaker::ModelCircuit& CircuitBreaker::evaluateEffectiveState(
    Shard& shard, const std::string& model) {
    // caller must hold shard.mutex
    auto& circuit = circuitFor(shard, model);
    if (circuit.state == CircuitState::Open) {
        auto now = std::chrono::steady_clock::now();
        if (now - circuit.last_failure >= config_.reset_timeout) {
            circuit.state = CircuitState::HalfOpen;
            circuit.half_open_calls = 0;
        }
    }
    return circuit;
}

bool CircuitBreaker::allowRequest(const std::string& model) {
#ifdef AEGISGATE_ENABLE_REDIS
    if (redis_store_) {
        return redis_store_->cbAllowRequest(model, config_.failure_threshold,
            static_cast<int>(config_.reset_timeout.count()), config_.half_open_max_calls);
    }
#endif
    auto& shard = shardFor(model);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto& circuit = evaluateEffectiveState(shard, model);

    if (circuit.state == CircuitState::Open) {
        return false;
    }

    if (circuit.state == CircuitState::HalfOpen) {
        if (circuit.half_open_calls < config_.half_open_max_calls) {
            ++circuit.half_open_calls;
            return true;
        }
        return false;
    }

    return true;
}

void CircuitBreaker::recordSuccess(const std::string& model) {
#ifdef AEGISGATE_ENABLE_REDIS
    if (redis_store_) { redis_store_->cbRecordSuccess(model); return; }
#endif
    auto& shard = shardFor(model);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto& circuit = circuitFor(shard, model);
    circuit.failure_count = 0;
    circuit.state = CircuitState::Closed;
}

void CircuitBreaker::recordFailure(const std::string& model) {
#ifdef AEGISGATE_ENABLE_REDIS
    if (redis_store_) {
        redis_store_->cbRecordFailure(model, config_.failure_threshold);
        return;
    }
#endif
    auto& shard = shardFor(model);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto& circuit = circuitFor(shard, model);
    circuit.failure_count++;
    circuit.last_failure = std::chrono::steady_clock::now();
    if (circuit.state == CircuitState::HalfOpen) {
        circuit.state = CircuitState::Open;
    } else if (circuit.failure_count >= config_.failure_threshold) {
        circuit.state = CircuitState::Open;
    }
}

CircuitState CircuitBreaker::state(const std::string& model) {
#ifdef AEGISGATE_ENABLE_REDIS
    if (redis_store_) {
        return redis_store_->cbGetState(model, config_.failure_threshold,
            static_cast<int>(config_.reset_timeout.count()));
    }
#endif
    auto& shard = shardFor(model);
    std::lock_guard<std::mutex> lock(shard.mutex);
    // First, honor idle-TTL absence semantics without materializing an entry:
    // if the model was never touched or has expired, keep returning Closed.
    auto it = shard.circuits.find(model);
    if (it == shard.circuits.end()) {
        return CircuitState::Closed;
    }
    if (config_.circuit_idle_ttl.count() > 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - it->second.last_access > config_.circuit_idle_ttl) {
            return CircuitState::Closed;
        }
    }
    // REV20260707-S4 D1 Option B: use the shared helper so we apply the
    // Open->HalfOpen timeout transition consistently with allowRequest().
    auto& circuit = evaluateEffectiveState(shard, model);
    return circuit.state;
}

void CircuitBreaker::exportMetrics(Gauge& gauge) {
    for (auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        // Snapshot model keys first because evaluateEffectiveState may
        // mutate the map (via circuitFor's LRU touch) and we don't want to
        // invalidate iterators during the walk.
        std::vector<std::string> keys;
        keys.reserve(shard.circuits.size());
        for (const auto& [model, _entry] : shard.circuits) {
            keys.push_back(model);
        }
        for (const auto& model : keys) {
            auto& circuit = evaluateEffectiveState(shard, model);
            gauge.set(static_cast<double>(circuit.state),
                      {{{std::string("model"), model}}, {}, false});
        }
    }
}

} // namespace aegisgate
