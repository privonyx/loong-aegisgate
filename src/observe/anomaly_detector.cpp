#include "observe/anomaly_detector.h"
#include "observe/metrics.h"
#include <sstream>

namespace aegisgate {

AnomalyDetector::AnomalyDetector() = default;

AnomalyDetector::AnomalyDetector(AnomalyDetectorConfig config)
    : config_(config) {}

void AnomalyDetector::recordMetric(AnomalyType type, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& w = windows_[static_cast<int>(type)];

    w.values.push_back(value);
    w.sum += value;
    w.sum_sq += value * value;

    if (w.values.size() > config_.window_size) {
        double old = w.values.front();
        w.values.pop_front();
        w.sum -= old;
        w.sum_sq -= old * old;
    }

    if (config_.enabled) {
        double z = computeZScore(w, value);
        if (z > config_.z_score_threshold) {
            double n = static_cast<double>(w.values.size());
            double mean = w.sum / n;
            double variance = (w.sum_sq / n) - (mean * mean);
            double sd = (variance > 0.0) ? std::sqrt(variance) : 0.0;

            std::ostringstream oss;
            oss << "Anomaly detected: z_score=" << z << " value=" << value
                << " mean=" << mean << " stddev=" << sd;

            events_.emplace_back(type, z, value, mean, sd, oss.str());
            // P2-#5: count each detected anomaly so anomaly_events_total
            // reflects reality instead of staying permanently zero.
            MetricsRegistry::instance().anomalyEventsTotal().inc();
            if (events_.size() > kMaxEvents) {
                events_.erase(events_.begin());
            }
        }
    }
}

std::vector<AnomalyEvent> AnomalyDetector::checkAnomalies() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AnomalyEvent> result;

    for (const auto& [key, w] : windows_) {
        if (w.values.empty()) continue;
        double latest = w.values.back();
        double z = computeZScore(w, latest);
        if (z > config_.z_score_threshold) {
            double n = static_cast<double>(w.values.size());
            double mean = w.sum / n;
            double variance = (w.sum_sq / n) - (mean * mean);
            double sd = (variance > 0.0) ? std::sqrt(variance) : 0.0;

            std::ostringstream oss;
            oss << "Current anomaly: z_score=" << z << " value=" << latest;

            result.emplace_back(static_cast<AnomalyType>(key), z, latest, mean, sd, oss.str());
        }
    }

    return result;
}

std::vector<AnomalyEvent> AnomalyDetector::recentEvents(size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.size() <= limit) {
        return events_;
    }
    return {events_.end() - static_cast<long>(limit), events_.end()};
}

void AnomalyDetector::setConfig(const AnomalyDetectorConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

void AnomalyDetector::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    windows_.clear();
    events_.clear();
}

double AnomalyDetector::computeZScore(const MetricWindow& window, double value) const {
    size_t n = window.values.size();
    if (n < 3) return 0.0;

    double dn = static_cast<double>(n);
    double mean = window.sum / dn;
    double variance = (window.sum_sq / dn) - (mean * mean);
    if (variance < 1e-12) return 0.0;

    double sd = std::sqrt(variance);
    return std::abs(value - mean) / sd;
}

} // namespace aegisgate
