#include "observe/quality_monitor.h"

namespace aegisgate {

QualityMonitor::QualityMonitor() = default;

QualityMonitor::QualityMonitor(QualityMonitorConfig config)
    : config_(config) {}

void QualityMonitor::recordQuality(const std::string& model,
                                   double quality_score,
                                   const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& mq = models_[model];

    if (!mq.initialized) {
        mq.ema = quality_score;
        mq.prev_ema = quality_score;
        mq.initialized = true;
    } else {
        mq.prev_ema = mq.ema;
        mq.ema = config_.ema_alpha * quality_score + (1.0 - config_.ema_alpha) * mq.ema;
    }
    mq.count++;

    // TASK-20260527-02 (SR3): white-list-only reason taxonomy.
    // Unknown values are silently dropped — neither the unknown bucket nor a
    // generic "other" counter is incremented. This prevents adversaries (or
    // buggy upstream callers) from inflating Case Study Numbers via
    // free-form reason strings.
    if (reason == "factuality") {
        mq.reason_factuality++;
    } else if (reason == "refusal") {
        mq.reason_refusal++;
    } else if (reason == "latency_degraded") {
        mq.reason_latency_degraded++;
    }
}

std::vector<QualityTrend> QualityMonitor::getTrends() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<QualityTrend> trends;
    trends.reserve(models_.size());

    for (const auto& [name, mq] : models_) {
        QualityTrend t(name, mq.ema, mq.ema - mq.prev_ema, mq.count);
        if (mq.count >= config_.min_samples && mq.ema < config_.alert_threshold) {
            t.alert_triggered = true;
        }
        t.reason_factuality = mq.reason_factuality;
        t.reason_refusal = mq.reason_refusal;
        t.reason_latency_degraded = mq.reason_latency_degraded;
        trends.push_back(std::move(t));
    }
    return trends;
}

std::optional<QualityTrend> QualityMonitor::getTrend(const std::string& model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = models_.find(model);
    if (it == models_.end()) return std::nullopt;

    const auto& mq = it->second;
    QualityTrend t(model, mq.ema, mq.ema - mq.prev_ema, mq.count);
    if (mq.count >= config_.min_samples && mq.ema < config_.alert_threshold) {
        t.alert_triggered = true;
    }
    t.reason_factuality = mq.reason_factuality;
    t.reason_refusal = mq.reason_refusal;
    t.reason_latency_degraded = mq.reason_latency_degraded;
    return t;
}

void QualityMonitor::setConfig(const QualityMonitorConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

void QualityMonitor::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    models_.clear();
}

} // namespace aegisgate
