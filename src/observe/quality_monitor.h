#pragma once
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <optional>

namespace aegisgate {

struct QualityTrend {
    std::string model;
    double current_ema = 0.0;
    double slope = 0.0;
    size_t sample_count = 0;
    bool alert_triggered = false;

    // TASK-20260527-02 (MVP-5 case-study) — reason taxonomy (D5=A 3 buckets).
    // SR3: only "factuality" / "refusal" / "latency_degraded" are accepted by
    // recordQuality(); unknown reasons are silently dropped (0 counter increment).
    size_t reason_factuality = 0;
    size_t reason_refusal = 0;
    size_t reason_latency_degraded = 0;

    QualityTrend() = default;
    QualityTrend(std::string m, double ema, double s, size_t n)
        : model(std::move(m)), current_ema(ema), slope(s), sample_count(n) {}
};

struct QualityMonitorConfig {
    bool enabled = false;
    double ema_alpha = 0.1;
    double alert_threshold = 0.3;
    size_t min_samples = 10;
};

class QualityMonitor {
public:
    QualityMonitor();
    explicit QualityMonitor(QualityMonitorConfig config);

    // TASK-20260527-02: optional reason taxonomy (default empty = legacy
    // call sites unaffected). SR3: reason must be one of
    // "" | "factuality" | "refusal" | "latency_degraded"; any other value
    // is silently dropped (0 counter increment), but EMA & sample_count
    // are still updated normally.
    void recordQuality(const std::string& model, double quality_score,
                       const std::string& reason = "");
    std::vector<QualityTrend> getTrends() const;
    std::optional<QualityTrend> getTrend(const std::string& model) const;
    void setConfig(const QualityMonitorConfig& config);
    const QualityMonitorConfig& qualityMonitorConfig() const { return config_; }
    void clear();

private:
    struct ModelQuality {
        double ema = 0.0;
        double prev_ema = 0.0;
        size_t count = 0;
        bool initialized = false;
        // TASK-20260527-02: reason taxonomy counters (3 buckets / SR3 white-list).
        size_t reason_factuality = 0;
        size_t reason_refusal = 0;
        size_t reason_latency_degraded = 0;
    };

    QualityMonitorConfig config_;
    std::unordered_map<std::string, ModelQuality> models_;
    mutable std::mutex mutex_;
};

} // namespace aegisgate
