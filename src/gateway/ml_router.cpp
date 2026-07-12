#include "gateway/ml_router.h"
#ifdef AEGISGATE_ENABLE_REDIS
#include "cluster/redis_state_store.h"
#endif
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace aegisgate {

MLRouter::MLRouter() : weights_{} {}
MLRouter::MLRouter(Weights weights) : weights_(weights) {}

void MLRouter::overrideQualityTier(const std::string& tenant_id,
                                    const std::string& tier) {
    if (tenant_id.empty() || tier.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    quality_tier_overrides_[tenant_id] = tier;
    spdlog::info("MLRouter: tenant={} quality_tier override set to {}",
                 tenant_id, tier);
}

void MLRouter::clearQualityTierOverride(const std::string& tenant_id) {
    if (tenant_id.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto erased = quality_tier_overrides_.erase(tenant_id);
    if (erased) {
        spdlog::info("MLRouter: tenant={} quality_tier override cleared",
                     tenant_id);
    }
}

std::optional<std::string>
MLRouter::getQualityTierOverride(const std::string& tenant_id) const {
    if (tenant_id.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = quality_tier_overrides_.find(tenant_id);
    if (it == quality_tier_overrides_.end()) return std::nullopt;
    return it->second;
}

double MLRouter::scoreModel(const ModelInfo& info, const ModelStats& stats,
                            double min_cost, double max_cost,
                            double min_latency, double max_latency) const {
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

    return weights_.cost * cost_score +
           weights_.quality * quality_score +
           weights_.latency * latency_score;
}

void MLRouter::reportOutcome(const std::string& model, double latency_ms,
                              bool success) {
#ifdef AEGISGATE_ENABLE_REDIS
    if (redis_store_) {
        redis_store_->mlReportOutcome(model, latency_ms, success);
    }
#endif
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

std::string MLRouter::selectModel(RequestContext& ctx,
                                   const ConnectorRegistry& registry) {
    if (!ctx.chat_request.model.empty()) {
        if (registry.findByModel(ctx.chat_request.model)) {
            return ctx.chat_request.model;
        }
        spdlog::warn("MLRouter: requested model {} not found, using ML routing",
                      ctx.chat_request.model);
    }

    auto all_models = registry.allModelInfos();
    if (all_models.empty()) {
        auto def = registry.defaultModel();
        if (def.empty()) {
            spdlog::error("MLRouter: no model available");
        }
        return def;
    }

    // Phase 11.5 — per-tenant override (installed by autonomy applier or
    // BudgetGuardStage) takes precedence over the in-request quality_tier
    // so that an autonomy downgrade cannot be defeated by a client header.
    std::string tier;
    if (auto override_tier = getQualityTierOverride(ctx.tenant_id)) {
        tier = *override_tier;
        spdlog::debug("MLRouter: tenant={} using override quality_tier={}",
                      ctx.tenant_id, tier);
    } else if (ctx.chat_request.extra.contains("quality_tier") &&
        ctx.chat_request.extra["quality_tier"].is_string()) {
        tier = ctx.chat_request.extra["quality_tier"].get<std::string>();
    }

    if (!tier.empty()) {
        std::sort(all_models.begin(), all_models.end(),
                  [](const ModelInfo& a, const ModelInfo& b) {
                      return (a.cost_per_1k_input + a.cost_per_1k_output) <
                             (b.cost_per_1k_input + b.cost_per_1k_output);
                  });

        size_t mid = all_models.size() / 2;
        if (tier == "economy") {
            std::vector<ModelInfo> filtered(all_models.begin(),
                                            all_models.begin() +
                                                static_cast<ptrdiff_t>(mid));
            all_models = std::move(filtered);
        } else if (tier == "premium") {
            std::vector<ModelInfo> filtered(all_models.begin() +
                                                static_cast<ptrdiff_t>(mid),
                                            all_models.end());
            all_models = std::move(filtered);
        }
    }

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
        if (it != stats_.end()) {
            lat = it->second.avg_latency_ms;
        }
#ifdef AEGISGATE_ENABLE_REDIS
        else if (redis_store_) {
            auto rs = redis_store_->mlGetStats(m.id);
            if (rs.sample_count > 0) lat = rs.avg_latency_ms;
        }
#endif
        min_latency = std::min(min_latency, lat);
        max_latency = std::max(max_latency, lat);
    }

    std::string best_model;
    double best_score = -1.0;

    for (const auto& m : all_models) {
        ModelStats st;
        auto it = stats_.find(m.id);
        if (it != stats_.end()) {
            st = it->second;
        }
#ifdef AEGISGATE_ENABLE_REDIS
        else if (redis_store_) {
            auto rs = redis_store_->mlGetStats(m.id);
            st.avg_latency_ms = rs.avg_latency_ms;
            st.success_rate = rs.success_rate;
            st.sample_count = rs.sample_count;
        }
#endif

        double score = scoreModel(m, st, min_cost, max_cost,
                                   min_latency, max_latency);
        if (score > best_score) {
            best_score = score;
            best_model = m.id;
        }
    }

    spdlog::debug("MLRouter: selected {} (score={:.3f})", best_model, best_score);
    return best_model;
}

}  // namespace aegisgate
