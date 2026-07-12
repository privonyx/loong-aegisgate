#pragma once

// Phase 11.2 TASK-20260521-03 — BanditRouter (decorator D7=C).
//
// BanditRouter wraps a base Router (typically MultiObjectiveRouter or
// MLRouter). In Shadow mode (default per SR5) it forwards selectModel
// to the base; in Live mode (entered only via transitionToLive() after
// isAutonomyEnabled() check) it serves traffic according to:
//   - ε-greedy:     with prob (1-ε) pick best mean reward, else explore
//   - Thompson:     sample from Beta(α+success, β+failure) per arm,
//                   pick arm with highest sample
//
// transitionToLive() / revertToShadow() are the SR5 shadow-first hooks
// invoked by BanditAutonomyApplier::apply()/rollback() (Epic 5) after
// AutonomyApprovalWorkflow approval.
//
// Threading: mutex_ guards arms_ + mode_ + rng_ (Lock Layer 1).

#include "gateway/router.h"

#include <mutex>
#include <random>
#include <string>
#include <unordered_map>

namespace aegisgate {

enum class BanditAlgorithm { EpsilonGreedy, ThompsonSampling };
enum class BanditMode { Shadow, Live };

class BanditRouter : public Router {
public:
    struct Config {
        BanditAlgorithm algorithm = BanditAlgorithm::ThompsonSampling;
        double epsilon = 0.1;            // only used for ε-greedy
        BanditMode mode = BanditMode::Shadow;  // M1: MUST default to Shadow
        double canary_pct = 0.05;         // SR5: start small
    };

    // base_router: non-owning, MUST outlive this router. Shadow mode
    // forwards selectModel to it. Live mode does not consult it.
    BanditRouter(Router* base_router, Config cfg);

    std::string selectModel(RequestContext& ctx,
                             const ConnectorRegistry& registry) override;

    // Update Bandit stats for a model based on observed outcome. reward
    // is in [0,1] (use 1.0 for success, 0.0 for failure unless caller
    // computes a custom reward function).
    void recordOutcome(const std::string& model, bool success, double reward);

    // P1-E: unified outcome hook. Updates this router's own arm posteriors and
    // forwards the outcome to the wrapped base router so the shadow/base learner
    // (MLRouter / MultiObjectiveRouter) keeps learning regardless of mode.
    void reportOutcome(const std::string& model, double latency_ms,
                       bool success) override;

    // SR5 shadow-to-live transition. Returns false (and does NOT change
    // mode) when isAutonomyEnabled() == false (SR17 reuse).
    // canary_pct is recorded but enforcement of per-tenant gating is
    // performed by BanditAutonomyApplier (Epic 5) using
    // RolloutController hooks — Live mode at the router level means
    // "serve from Bandit posteriors".
    bool transitionToLive(double canary_pct);

    // Reverse path (manual rollback or Applier::rollback).
    void revertToShadow();

    BanditMode getMode() const;

    // SR17 reuse — same env var as AutonomyApprovalWorkflow.
    static bool isAutonomyEnabled();

private:
    std::string selectEpsilonGreedy(const std::vector<std::string>& candidates);
    std::string selectThompson(const std::vector<std::string>& candidates);
    static std::vector<std::string> candidateModels(
        const ConnectorRegistry& registry);

    Router* base_router_;  // non-owning
    Config cfg_;
    mutable std::mutex mutex_;  // Lock Layer 1
    struct ArmStats {
        int successes = 0;
        int failures = 0;
        double total_reward = 0.0;
    };
    std::unordered_map<std::string, ArmStats> arms_;
    std::mt19937 rng_;
};

}  // namespace aegisgate
