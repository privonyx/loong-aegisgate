#pragma once
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <cmath>

namespace aegisgate {

enum class AnomalyType { RateSpike, LatencySpike, ErrorSpike, CostSpike };

struct AnomalyEvent {
    AnomalyType type;
    double z_score = 0.0;
    double current_value = 0.0;
    double mean = 0.0;
    double stddev = 0.0;
    std::chrono::steady_clock::time_point timestamp;
    std::string description;

    AnomalyEvent() : type(AnomalyType::RateSpike), timestamp(std::chrono::steady_clock::now()) {}
    AnomalyEvent(AnomalyType t, double z, double cur, double m, double s, std::string desc)
        : type(t), z_score(z), current_value(cur), mean(m), stddev(s),
          timestamp(std::chrono::steady_clock::now()), description(std::move(desc)) {}
};

struct AnomalyDetectorConfig {
    double z_score_threshold = 3.0;
    size_t window_size = 100;
    bool enabled = false;
};

class AnomalyDetector {
public:
    AnomalyDetector();
    explicit AnomalyDetector(AnomalyDetectorConfig config);

    void recordMetric(AnomalyType type, double value);
    std::vector<AnomalyEvent> checkAnomalies() const;
    std::vector<AnomalyEvent> recentEvents(size_t limit = 20) const;
    void setConfig(const AnomalyDetectorConfig& config);
    const AnomalyDetectorConfig& anomalyConfig() const { return config_; }
    void clear();

private:
    struct MetricWindow {
        std::deque<double> values;
        double sum = 0.0;
        double sum_sq = 0.0;
    };

    double computeZScore(const MetricWindow& window, double value) const;

    AnomalyDetectorConfig config_;
    std::unordered_map<int, MetricWindow> windows_;
    std::vector<AnomalyEvent> events_;
    mutable std::mutex mutex_;
    static constexpr size_t kMaxEvents = 1000;
};

} // namespace aegisgate
