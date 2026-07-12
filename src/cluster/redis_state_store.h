#pragma once
#ifdef AEGISGATE_ENABLE_REDIS

#include "gateway/circuit_breaker.h"
#include <cstdint>
#include <string>

namespace aegisgate {

class RedisCacheStore;

// TASK-20260703-02 Epic 2 / C6：熔断状态判定纯函数（不依赖 redis 连接，沙箱可测）。
// 从持久化的 (state, failure_count, last_failure_ms) 与当前时间/阈值/超时推导状态。
// C6 根因：Closed 态累积失败到阈值（state=0, fc>=threshold）此前无条件返回 Open，
// 不受 reset_timeout 支配 → 永停 Open，只能靠 key TTL(3600s) 恢复。修复：该分支
// 同样基于 last_failure_ms 判断 reset_timeout → 超时后转 HalfOpen 允许试探恢复。
CircuitState computeCircuitState(int state, int failure_count,
                                 int64_t last_failure_ms, int64_t now_ms,
                                 int failure_threshold, int reset_timeout_s);

class RedisStateStore {
public:
    explicit RedisStateStore(RedisCacheStore* redis);

    bool initialize();

    bool rateLimitAllow(const std::string& key, double cost,
                         double max_tokens, double refill_rate);
    double rateLimitRemaining(const std::string& key, double max_tokens,
                               double refill_rate);

    void cbRecordSuccess(const std::string& model);
    // TASK-20260711-02 / TASK-20260701-01 P0-C-BAK D3 Option C: atomic Lua
    // EVAL now needs the failure_threshold to persist state=1(Open) when
    // the counter reaches threshold. Callers pass their CircuitConfig
    // failure_threshold.
    void cbRecordFailure(const std::string& model, int failure_threshold);
    CircuitState cbGetState(const std::string& model, int failure_threshold,
                             int reset_timeout_s);
    bool cbAllowRequest(const std::string& model, int failure_threshold,
                         int reset_timeout_s, int half_open_max);

    void abuseRecordRejection(const std::string& key, int window_seconds);
    int abuseGetCount(const std::string& key, int window_seconds);
    bool abuseIsBlocked(const std::string& key);
    void abuseSetBlocked(const std::string& key, int duration_seconds);

    void mlReportOutcome(const std::string& model, double latency_ms, bool success);
    struct MLStats {
        double avg_latency_ms = 100.0;
        double success_rate = 1.0;
        int sample_count = 0;
    };
    MLStats mlGetStats(const std::string& model);

private:
    int64_t nowMs() const;
    std::string uniqueId() const;

    static constexpr const char* kRateLimitPrefix = "aegisgate:rl:";
    static constexpr const char* kCircuitPrefix = "aegisgate:cb:";
    static constexpr const char* kAbusePrefix = "aegisgate:abuse:";
    static constexpr const char* kAbuseBlockPrefix = "aegisgate:abuse:blocked:";
    static constexpr const char* kMLStatsPrefix = "aegisgate:ml:";
    static constexpr double kEmaAlpha = 0.1;

    RedisCacheStore* redis_;
    std::string rl_script_sha_;
    std::string ml_script_sha_;
};

}  // namespace aegisgate

#endif  // AEGISGATE_ENABLE_REDIS
