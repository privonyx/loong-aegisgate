#include "gateway/bandit_router.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>

namespace aegisgate {

BanditRouter::BanditRouter(Router* base_router, Config cfg)
    : base_router_(base_router), cfg_(cfg), rng_(std::random_device{}()) {}

bool BanditRouter::isAutonomyEnabled() {
    // SR17 reuse — mirrors AutonomyApprovalWorkflow::isAutonomyEnabled().
    const char* v = std::getenv("AEGISGATE_DISABLE_AUTONOMY");
    if (v == nullptr) return true;
    return std::string(v) != "1";
}

BanditMode BanditRouter::getMode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cfg_.mode;
}

bool BanditRouter::transitionToLive(double canary_pct) {
    // SR5 + SR17: refuse to enter Live mode while autonomy is disabled.
    if (!isAutonomyEnabled()) {
        spdlog::warn(
            "BanditRouter: transitionToLive refused — autonomy disabled (SR17)");
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    cfg_.mode = BanditMode::Live;
    cfg_.canary_pct = canary_pct;
    spdlog::info("BanditRouter: transitioned to Live (canary={:.2f}%)",
                  canary_pct * 100.0);
    return true;
}

void BanditRouter::revertToShadow() {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg_.mode = BanditMode::Shadow;
    spdlog::info("BanditRouter: reverted to Shadow");
}

void BanditRouter::recordOutcome(const std::string& model, bool success,
                                  double reward) {
    if (model.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& s = arms_[model];
    if (success) s.successes++;
    else s.failures++;
    s.total_reward += reward;
}

void BanditRouter::reportOutcome(const std::string& model, double latency_ms,
                                  bool success) {
    // Update our own Beta/ε-greedy arm posteriors (reward = 1.0 on success).
    recordOutcome(model, success, success ? 1.0 : 0.0);
    // Forward to the wrapped base learner so it keeps tracking latency/success
    // even while we serve from Bandit posteriors in Live mode.
    if (base_router_) {
        base_router_->reportOutcome(model, latency_ms, success);
    }
}

std::vector<std::string> BanditRouter::candidateModels(
    const ConnectorRegistry& registry) {
    std::vector<std::string> out;
    auto infos = registry.allModelInfos();
    out.reserve(infos.size());
    for (const auto& m : infos) out.push_back(m.id);
    return out;
}

std::string BanditRouter::selectEpsilonGreedy(
    const std::vector<std::string>& candidates) {
    // Caller holds mutex_.
    if (candidates.empty()) return "";

    std::uniform_real_distribution<double> uni(0.0, 1.0);
    if (uni(rng_) < cfg_.epsilon) {
        std::uniform_int_distribution<size_t> pick(0, candidates.size() - 1);
        return candidates[pick(rng_)];
    }

    // Exploit: highest empirical mean reward (ties → first).
    std::string best = candidates.front();
    double best_mean = -std::numeric_limits<double>::infinity();
    for (const auto& c : candidates) {
        auto it = arms_.find(c);
        double mean = 0.0;
        if (it != arms_.end()) {
            int n = it->second.successes + it->second.failures;
            if (n > 0) mean = it->second.total_reward / n;
        }
        if (mean > best_mean) {
            best_mean = mean;
            best = c;
        }
    }
    return best;
}

std::string BanditRouter::selectThompson(
    const std::vector<std::string>& candidates) {
    // Caller holds mutex_.
    if (candidates.empty()) return "";

    // Thompson Sampling with Beta(1+successes, 1+failures) prior.
    std::string best = candidates.front();
    double best_sample = -std::numeric_limits<double>::infinity();
    for (const auto& c : candidates) {
        int alpha = 1;
        int beta = 1;
        auto it = arms_.find(c);
        if (it != arms_.end()) {
            alpha += it->second.successes;
            beta += it->second.failures;
        }
        // Beta sample via two gamma samples: x/(x+y).
        std::gamma_distribution<double> ga(static_cast<double>(alpha), 1.0);
        std::gamma_distribution<double> gb(static_cast<double>(beta), 1.0);
        double x = ga(rng_);
        double y = gb(rng_);
        double sample = (x + y > 0.0) ? x / (x + y) : 0.5;
        if (sample > best_sample) {
            best_sample = sample;
            best = c;
        }
    }
    return best;
}

std::string BanditRouter::selectModel(RequestContext& ctx,
                                       const ConnectorRegistry& registry) {
    // SR5 / D7: Shadow mode forwards to base router (decision is recorded
    // but the base wins). Live mode skips the base entirely.
    BanditMode current_mode;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_mode = cfg_.mode;
    }

    if (current_mode == BanditMode::Shadow) {
        if (base_router_ == nullptr) {
            spdlog::error("BanditRouter: shadow mode but base_router is null");
            return registry.defaultModel();
        }
        return base_router_->selectModel(ctx, registry);
    }

    // Live mode — Bandit selection.
    if (!ctx.chat_request.model.empty()) {
        if (registry.findByModel(ctx.chat_request.model)) {
            return ctx.chat_request.model;
        }
    }

    auto candidates = candidateModels(registry);
    if (candidates.empty()) return registry.defaultModel();

    std::lock_guard<std::mutex> lock(mutex_);
    if (cfg_.algorithm == BanditAlgorithm::EpsilonGreedy) {
        return selectEpsilonGreedy(candidates);
    }
    return selectThompson(candidates);
}

}  // namespace aegisgate
