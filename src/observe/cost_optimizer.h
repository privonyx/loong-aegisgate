#pragma once
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aegisgate::autonomy {
class AutonomyApprovalWorkflow;
}

namespace aegisgate {

struct ModelUsageProfile {
    std::string model;
    double total_cost = 0.0;
    double avg_quality = 0.0;
    int request_count = 0;
    double cost_per_quality = 0.0;

    ModelUsageProfile() = default;
    ModelUsageProfile(std::string m, double c, double q, int r)
        : model(std::move(m)), total_cost(c), avg_quality(q), request_count(r),
          cost_per_quality(q > 0 ? c / q : 0.0) {}
};

struct OptimizationRecommendation {
    std::string current_model;
    std::string recommended_model;
    double potential_savings = 0.0;
    double quality_impact = 0.0;
    std::string reason;

    OptimizationRecommendation() = default;
    OptimizationRecommendation(std::string cur, std::string rec, double sav,
                                double qi, std::string r)
        : current_model(std::move(cur)), recommended_model(std::move(rec)),
          potential_savings(sav), quality_impact(qi), reason(std::move(r)) {}
};

struct CostOptimizerConfig {
    bool enabled = false;
    double min_quality_threshold = 0.5;
    double max_quality_loss = 0.1;
    int min_requests_for_recommendation = 50;
};

class CostOptimizer {
public:
    CostOptimizer();
    explicit CostOptimizer(CostOptimizerConfig config);

    void recordUsage(const std::string& model, double cost, double quality);
    std::vector<OptimizationRecommendation> getRecommendations() const;
    std::vector<ModelUsageProfile> getProfiles() const;
    void setConfig(const CostOptimizerConfig& config);
    const CostOptimizerConfig& costOptimizerConfig() const { return config_; }
    void clear();

    // Phase 11.5 v2 (TASK-20260518-02 E2.1) — push every recommendation
    // through AutonomyApprovalWorkflow as an ApprovalProposal. v1 callers
    // of getRecommendations() are unaffected; this is an additive surface.
    //
    // Returns the ULIDs that the workflow assigned to the new proposals.
    // Empty proposals (failed dedup / propose-blocked-by-env / no recs)
    // are skipped silently; spdlog::warn explains each skip.
    //
    // Idempotency: within `dedup_window`, a recommendation for the same
    // (current_model, recommended_model) pair on the same tenant is NOT
    // re-proposed. Default 1h matches plan §D Task 2.1 dedup intent.
    std::vector<std::string> proposeRecommendations(
        const std::string& tenant_id,
        std::shared_ptr<autonomy::AutonomyApprovalWorkflow> workflow,
        std::chrono::seconds dedup_window = std::chrono::hours{1});

private:
    struct ModelStats {
        double total_cost = 0.0;
        double total_quality = 0.0;
        int count = 0;
    };

    CostOptimizerConfig config_;
    std::unordered_map<std::string, ModelStats> stats_;
    // Dedup cache: key = "<tenant>|<current>=><recommended>",
    // value = unix ms when last proposed. Pruned opportunistically.
    std::unordered_map<std::string, std::int64_t> propose_dedup_;
    mutable std::mutex mutex_;
};

} // namespace aegisgate
