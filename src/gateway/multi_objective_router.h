#pragma once

// Phase 11.2 TASK-20260521-03 — MultiObjectiveRouter.
//
// Pareto-front cost/quality/latency router with per-strategy weight
// overrides installed by RoutingStrategyCatalog + BanditAutonomyApplier.
// Scoring formula mirrors MLRouter::scoreModel (normalized weighted sum
// across the 3 objectives) but with a strategy-aware weight table:
//
//   default     = {cost: 0.4,  quality: 0.35, latency: 0.25}
//   cost-first  = {cost: 0.7,  quality: 0.2,  latency: 0.1}
//   quality-first = {cost: 0.2, quality: 0.6, latency: 0.2}
//   hybrid      = default
//   canary/shadow = default (carries a Bandit config but reuses weights)
//
// This router does NOT do online learning — that is BanditRouter's job.
// It exposes reportOutcome() for callers (e.g. RolloutController feedback
// path) but uses safe defaults (success_rate=1.0, avg_latency=100ms)
// when stats are absent.
//
// Threading: mutex_ guards stats_ + active_strategy_ (Lock Layer 1).

#include "gateway/router.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace aegisgate {

class MultiObjectiveRouter : public Router {
public:
    struct Weights {
        double cost = 0.4;
        double quality = 0.35;
        double latency = 0.25;
    };

    MultiObjectiveRouter();
    explicit MultiObjectiveRouter(Weights default_weights);

    std::string selectModel(RequestContext& ctx,
                             const ConnectorRegistry& registry) override;

    // Outcome feedback (latency_ms + success). Updates EMA of avg_latency
    // and success_rate.  Safe to call from any thread.
    void reportOutcome(const std::string& model, double latency_ms,
                       bool success) override;

    // RoutingStrategyCatalog + BanditAutonomyApplier wire these to install
    // a named strategy override. When an override is active, getActiveStrategy
    // returns the name and selectModel uses the override weights.
    void setActiveStrategy(const std::string& strategy_name,
                            const Weights& override);
    void clearActiveStrategy();
    std::optional<std::string> getActiveStrategy() const;

private:
    struct ModelStats {
        double avg_latency_ms = 100.0;
        double success_rate = 1.0;
        int sample_count = 0;
    };

    Weights resolveWeights() const;
    double scoreModel(const ModelInfo& info, const ModelStats& stats,
                       double min_cost, double max_cost,
                       double min_latency, double max_latency,
                       const Weights& w) const;

    Weights default_weights_;
    mutable std::mutex mutex_;  // Lock Layer 1
    std::unordered_map<std::string, ModelStats> stats_;
    std::optional<std::pair<std::string, Weights>> active_strategy_;
    static constexpr double kEmaAlpha = 0.1;
};

}  // namespace aegisgate
