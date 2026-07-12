#pragma once
#include "gateway/circuit_breaker.h"
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace aegisgate {

class MockRedisStateStore {
public:
    bool rateLimitAllow(const std::string& key, double cost,
                         double max_tokens, double refill_rate) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& b = buckets_[key];
        auto now = nowMs();
        if (b.last_refill == 0) {
            b.tokens = max_tokens;
            b.last_refill = now;
        }
        double elapsed = static_cast<double>(now - b.last_refill) / 1000.0;
        b.tokens = std::min(max_tokens, b.tokens + elapsed * refill_rate);
        b.last_refill = now;
        if (b.tokens >= cost) {
            b.tokens -= cost;
            return true;
        }
        return false;
    }

    double rateLimitRemaining(const std::string& key, double max_tokens,
                               double refill_rate) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = buckets_.find(key);
        if (it == buckets_.end()) return max_tokens;
        auto now = nowMs();
        double elapsed = static_cast<double>(now - it->second.last_refill) / 1000.0;
        return std::min(max_tokens, it->second.tokens + elapsed * refill_rate);
    }

    void cbRecordSuccess(const std::string& model) {
        std::lock_guard<std::mutex> lock(mutex_);
        circuits_[model] = {CircuitState::Closed, 0, 0, 0};
    }

    // TASK-20260711-02 / TASK-20260701-01 P0-C-BAK D3: signature now takes
    // failure_threshold so the mock can persist the state field explicitly
    // (matching the real RedisStateStore's atomic Lua behavior).
    void cbRecordFailure(const std::string& model, int failure_threshold) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& c = circuits_[model];
        c.failure_count++;
        c.last_failure_ms = nowMs();
        if (c.state == CircuitState::HalfOpen) {
            c.state = CircuitState::Open;
        } else if (c.state == CircuitState::Closed
                   && c.failure_count >= failure_threshold) {
            c.state = CircuitState::Open;
        }
    }

    CircuitState cbGetState(const std::string& model, int failure_threshold,
                             int reset_timeout_s) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = circuits_.find(model);
        if (it == circuits_.end()) return CircuitState::Closed;
        auto& c = it->second;
        if (c.state == CircuitState::Open) {
            if ((nowMs() - c.last_failure_ms) / 1000 >= reset_timeout_s)
                return CircuitState::HalfOpen;
            return CircuitState::Open;
        }
        if (c.failure_count >= failure_threshold) return CircuitState::Open;
        return c.state;
    }

    bool cbAllowRequest(const std::string& model, int failure_threshold,
                         int reset_timeout_s, int half_open_max) {
        auto state = cbGetState(model, failure_threshold, reset_timeout_s);
        if (state == CircuitState::Closed) return true;
        if (state == CircuitState::Open) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        auto& c = circuits_[model];
        if (c.half_open_calls < half_open_max) {
            c.half_open_calls++;
            c.state = CircuitState::HalfOpen;
            return true;
        }
        return false;
    }

    void abuseRecordRejection(const std::string& key, int /*window_seconds*/) {
        std::lock_guard<std::mutex> lock(mutex_);
        abuse_timestamps_[key].push_back(nowMs());
    }

    int abuseGetCount(const std::string& key, int window_seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = abuse_timestamps_.find(key);
        if (it == abuse_timestamps_.end()) return 0;
        auto cutoff = nowMs() - static_cast<int64_t>(window_seconds) * 1000;
        int count = 0;
        for (auto ts : it->second) {
            if (ts >= cutoff) count++;
        }
        return count;
    }

    bool abuseIsBlocked(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = blocked_until_.find(key);
        return it != blocked_until_.end() && it->second > nowMs();
    }

    void abuseSetBlocked(const std::string& key, int duration_seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        blocked_until_[key] = nowMs() + static_cast<int64_t>(duration_seconds) * 1000;
    }

    void mlReportOutcome(const std::string& model, double latency_ms, bool success) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& s = ml_stats_[model];
        double sv = success ? 1.0 : 0.0;
        if (s.sample_count == 0) {
            s.avg_latency_ms = latency_ms;
            s.success_rate = sv;
        } else {
            s.avg_latency_ms = 0.9 * s.avg_latency_ms + 0.1 * latency_ms;
            s.success_rate = 0.9 * s.success_rate + 0.1 * sv;
        }
        s.sample_count++;
    }

    struct MLStats {
        double avg_latency_ms = 100.0;
        double success_rate = 1.0;
        int sample_count = 0;
    };

    MLStats mlGetStats(const std::string& model) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = ml_stats_.find(model);
        if (it == ml_stats_.end()) return {};
        return it->second;
    }

private:
    int64_t nowMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    struct Bucket { double tokens = 0; int64_t last_refill = 0; };
    struct Circuit {
        CircuitState state = CircuitState::Closed;
        int failure_count = 0;
        int half_open_calls = 0;
        int64_t last_failure_ms = 0;
    };

    std::mutex mutex_;
    std::unordered_map<std::string, Bucket> buckets_;
    std::unordered_map<std::string, Circuit> circuits_;
    std::unordered_map<std::string, std::deque<int64_t>> abuse_timestamps_;
    std::unordered_map<std::string, int64_t> blocked_until_;
    std::unordered_map<std::string, MLStats> ml_stats_;
};

}  // namespace aegisgate
