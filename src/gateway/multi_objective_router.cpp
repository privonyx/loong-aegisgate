#include "gateway/multi_objective_router.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace aegisgate {

MultiObjectiveRouter::MultiObjectiveRouter() : default_weights_{} {}

MultiObjectiveRouter::MultiObjectiveRouter(Weights default_weights)
    : default_weights_(default_weights) {}

void MultiObjectiveRouter::reportOutcome(const std::string& model,
                                          double latency_ms, bool success) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& s = stats_[model];
    double success_val = success ? 1.0 : 0.0;
    if (s.sample_count == 0) {
        s.avg_latency_ms = latency_ms;
        s.success_rate = success_val;
    } else {
        s.avg_latency_ms = (1.0 - kEmaAlpha) * s.avg_latency_ms +
                           kEmaAlpha * latency_ms;
        s.success_rate = (1.0 - kEmaAlpha) * s.success_rate +
                         kEmaAlpha * success_val;
    }
    s.sample_count++;
}

void MultiObjectiveRouter::setActiveStrategy(const std::string& strategy_name,
                                              const Weights& override) {
    if (strategy_name.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    active_strategy_ = std::make_pair(strategy_name, override);
    spdlog::info("MultiObjectiveRouter: active strategy = {}", strategy_name);
}

void MultiObjectiveRouter::clearActiveStrategy() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_strategy_) {
        spdlog::info("MultiObjectiveRouter: cleared active strategy = {}",
                     active_strategy_->first);
    }
    active_strategy_.reset();
}

std::optional<std::string> MultiObjectiveRouter::getActiveStrategy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_strategy_) return std::nullopt;
    return active_strategy_->first;
}

MultiObjectiveRouter::Weights MultiObjectiveRouter::resolveWeights() const {
    // Caller holds mutex_.
    if (active_strategy_) return active_strategy_->second;
    return default_weights_;
}

double MultiObjectiveRouter::scoreModel(const ModelInfo& info,
                                         const ModelStats& stats,
                                         double min_cost, double max_cost,
                                         double min_latency, double max_latency,
                                         const Weights& w) const {
    double cost_score = 1.0;
    double cost = info.cost_per_1k_input + info.cost_per_1k_output;
    if (max_cost > min_cost) {
        cost_score = 1.0 - (cost - min_cost) / (max_cost - min_cost);
    }

    double quality_score = stats.success_rate;

    double latency_score = 1.0;
    if (max_latency > min_latency) {
        latency_score = 1.0 - (stats.avg_latency_ms - min_latency) /
                                   (max_latency - min_latency);
    }

    return w.cost * cost_score + w.quality * quality_score +
           w.latency * latency_score;
}

std::string MultiObjectiveRouter::selectModel(RequestContext& ctx,
                                               const ConnectorRegistry& registry) {
    if (!ctx.chat_request.model.empty()) {
        if (registry.findByModel(ctx.chat_request.model)) {
            return ctx.chat_request.model;
        }
        spdlog::warn(
            "MultiObjectiveRouter: requested model {} not found, falling back",
            ctx.chat_request.model);
    }

    auto all_models = registry.allModelInfos();
    if (all_models.empty()) {
        return registry.defaultModel();
    }

    double min_cost = std::numeric_limits<double>::max();
    double max_cost = 0.0;
    double min_latency = std::numeric_limits<double>::max();
    double max_latency = 0.0;

    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& m : all_models) {
        double cost = m.cost_per_1k_input + m.cost_per_1k_output;
        min_cost = std::min(min_cost, cost);
        max_cost = std::max(max_cost, cost);

        double lat = 100.0;
        auto it = stats_.find(m.id);
        if (it != stats_.end()) lat = it->second.avg_latency_ms;
        min_latency = std::min(min_latency, lat);
        max_latency = std::max(max_latency, lat);
    }

    const Weights w = resolveWeights();

    std::string best_model;
    double best_score = -1.0;

    for (const auto& m : all_models) {
        ModelStats st;
        auto it = stats_.find(m.id);
        if (it != stats_.end()) st = it->second;

        double score = scoreModel(m, st, min_cost, max_cost,
                                   min_latency, max_latency, w);
        if (score > best_score) {
            best_score = score;
            best_model = m.id;
        }
    }

    spdlog::debug("MultiObjectiveRouter: selected {} (score={:.3f}, strategy={})",
                  best_model, best_score,
                  active_strategy_ ? active_strategy_->first : "default");
    return best_model;
}

}  // namespace aegisgate
