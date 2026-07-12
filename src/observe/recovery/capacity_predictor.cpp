// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 4.

#include "observe/recovery/capacity_predictor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace aegisgate {

CapacityPredictor::CapacityPredictor() : cfg_(Config{}) {}
CapacityPredictor::CapacityPredictor(Config cfg) : cfg_(std::move(cfg)) {}

double CapacityPredictor::predictQps(const std::vector<QpsSample>& history,
                                        int horizon_s) const {
    if (history.size() < 2) {
        return history.empty() ? 0.0 : history.back().qps;
    }
    // Least-squares fit on (ts_seconds, qps).
    double n = static_cast<double>(history.size());
    double sum_x = 0, sum_y = 0, sum_xx = 0, sum_xy = 0;
    const double base_ts = static_cast<double>(history.front().ts_ms) / 1000.0;
    for (const auto& s : history) {
        const double x = static_cast<double>(s.ts_ms) / 1000.0 - base_ts;
        const double y = s.qps;
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
    }
    const double denom = n * sum_xx - sum_x * sum_x;
    if (denom == 0.0) {
        return history.back().qps;
    }
    const double slope     = (n * sum_xy - sum_x * sum_y) / denom;
    const double intercept = (sum_y - slope * sum_x) / n;
    const double last_x    = static_cast<double>(history.back().ts_ms) / 1000.0
                              - base_ts;
    const double future_x  = last_x + static_cast<double>(horizon_s);
    const double predicted = intercept + slope * future_x;
    return std::max(0.0, predicted);
}

nlohmann::json
CapacityPredictor::proposeHpa(const std::vector<QpsSample>& history,
                                int current_replicas) const {
    const double current_qps =
        history.empty() ? 0.0 : history.back().qps;
    const double predicted_qps =
        predictQps(history, cfg_.forecast_horizon_seconds);

    // Effective per-replica capacity after applying safety margin.
    const double per_replica_effective =
        static_cast<double>(cfg_.target_qps_per_replica) /
        (1.0 + cfg_.safety_margin);

    int proposed_replicas = current_replicas;
    if (per_replica_effective > 0.0) {
        proposed_replicas = static_cast<int>(
            std::ceil(predicted_qps / per_replica_effective));
    }

    // SR-NEW3 clamps.
    proposed_replicas = std::max(proposed_replicas, cfg_.min_replicas);
    proposed_replicas = std::min(proposed_replicas, cfg_.max_replicas);

    const int delta = proposed_replicas - current_replicas;
    const double cost_increase = static_cast<double>(delta) *
                                  cfg_.max_cost_per_replica_usd_24h;

    std::ostringstream cmd;
    cmd << "kubectl scale deployment/aegisgate --replicas="
         << proposed_replicas;

    std::ostringstream rationale;
    rationale << "predicted_qps_30min=" << predicted_qps
                << "; target_qps_per_replica=" << cfg_.target_qps_per_replica
                << "; safety_margin=" << cfg_.safety_margin
                << "; effective_per_replica=" << per_replica_effective
                << "; clamped=[" << cfg_.min_replicas << ","
                << cfg_.max_replicas << "]";

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

    return nlohmann::json{
        {"generated_at_ms",                  now_ms},
        {"current_qps",                      current_qps},
        {"predicted_qps_30min",              predicted_qps},
        {"current_replicas",                 current_replicas},
        {"proposed_replicas",                proposed_replicas},
        {"safety_margin",                    cfg_.safety_margin},
        {"estimated_cost_increase_usd_24h",  cost_increase},
        {"suggested_kubectl_command",        cmd.str()},
        {"rationale",                        rationale.str()},
    };
}

} // namespace aegisgate
