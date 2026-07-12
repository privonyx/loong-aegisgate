#pragma once
#include "gateway/router.h"
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#ifdef AEGISGATE_ENABLE_REDIS
namespace aegisgate { class RedisStateStore; }
#endif

namespace aegisgate {

class MLRouter : public Router {
public:
    struct Weights {
        double cost = 0.4;
        double quality = 0.35;
        double latency = 0.25;
    };

    MLRouter();
    explicit MLRouter(Weights weights);
    std::string selectModel(RequestContext& ctx,
                             const ConnectorRegistry& registry) override;
    void reportOutcome(const std::string& model, double latency_ms,
                       bool success) override;

    // Phase 11.5 — per-tenant quality_tier override (E2.0). CostAutonomyApplier
    // installs/clears these overrides as approval proposals get applied or
    // rolled back; BudgetGuardStage also writes them under sustained spend.
    // The override is consulted BEFORE the in-request quality_tier extra so
    // an autonomy decision can downgrade a tenant even if the client asked
    // for "premium". Empty tenant_id is a no-op (defensive guard).
    void overrideQualityTier(const std::string& tenant_id,
                              const std::string& tier);
    void clearQualityTierOverride(const std::string& tenant_id);
    std::optional<std::string> getQualityTierOverride(
        const std::string& tenant_id) const;

#ifdef AEGISGATE_ENABLE_REDIS
    void setRedisStateStore(RedisStateStore* store) { redis_store_ = store; }
#endif

private:
    struct ModelStats {
        double avg_latency_ms = 100.0;
        double success_rate = 1.0;
        int sample_count = 0;
    };
    double scoreModel(const ModelInfo& info, const ModelStats& stats,
                      double min_cost, double max_cost,
                      double min_latency, double max_latency) const;

    Weights weights_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ModelStats> stats_;
    // Phase 11.5 — per-tenant quality_tier overrides installed by autonomy
    // appliers. Guarded by mutex_; empty value would be removed via the
    // explicit clear API rather than stored as "".
    std::unordered_map<std::string, std::string> quality_tier_overrides_;
    static constexpr double kEmaAlpha = 0.1;
#ifdef AEGISGATE_ENABLE_REDIS
    RedisStateStore* redis_store_ = nullptr;
#endif
};

}  // namespace aegisgate
