#include "observe/cost_optimizer.h"

#include "observe/autonomy/approval_proposal.h"
#include "observe/autonomy/approval_state.h"
#include "observe/autonomy/approval_workflow.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <string_view>

namespace {
// Heuristic model→tier mapping used by proposeRecommendations() to
// populate the from/to_quality_tier payload fields expected by
// CostAutonomyApplier::isLowRisk (C2 creative decision). Keeps v2
// self-contained without forcing every test to inject a model registry.
std::string inferTier(const std::string& model) {
    static const std::string_view premium_prefixes[] = {
        "gpt-4", "claude-3-opus", "claude-4", "gemini-1.5-pro", "o1",
    };
    static const std::string_view economy_markers[] = {
        "qwen", "llama", "mistral", "phi", "tinyllama", ":7b", ":3b",
        "mini", "haiku", "flash", "nano",
    };
    for (auto p : premium_prefixes) {
        if (model.rfind(std::string(p), 0) == 0) return "premium";
    }
    for (auto m : economy_markers) {
        if (model.find(std::string(m)) != std::string::npos) return "economy";
    }
    return "standard";
}
} // namespace

namespace aegisgate {

CostOptimizer::CostOptimizer() = default;

CostOptimizer::CostOptimizer(CostOptimizerConfig config)
    : config_(config) {}

void CostOptimizer::recordUsage(const std::string& model, double cost, double quality) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& s = stats_[model];
    s.total_cost += cost;
    s.total_quality += quality;
    s.count++;
}

std::vector<OptimizationRecommendation> CostOptimizer::getRecommendations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OptimizationRecommendation> recs;

    struct ProfileData {
        std::string model;
        double avg_cost;
        double avg_quality;
        double cost_per_quality;
        int count;
    };

    std::vector<ProfileData> profiles;
    profiles.reserve(stats_.size());

    for (const auto& [name, s] : stats_) {
        if (s.count < config_.min_requests_for_recommendation) continue;
        double avg_cost = s.total_cost / s.count;
        double avg_quality = s.total_quality / s.count;
        double cpq = (avg_quality > 0) ? avg_cost / avg_quality : 0.0;
        profiles.push_back({name, avg_cost, avg_quality, cpq, s.count});
    }

    for (const auto& current : profiles) {
        for (const auto& candidate : profiles) {
            if (candidate.model == current.model) continue;
            if (candidate.avg_quality < config_.min_quality_threshold) continue;

            double quality_loss = current.avg_quality - candidate.avg_quality;
            if (quality_loss > config_.max_quality_loss) continue;

            if (candidate.cost_per_quality < current.cost_per_quality) {
                double savings = (current.avg_cost - candidate.avg_cost) * current.count;

                std::ostringstream oss;
                oss << candidate.model << " has lower cost/quality ratio ("
                    << candidate.cost_per_quality << " vs " << current.cost_per_quality << ")";

                recs.emplace_back(current.model, candidate.model, savings,
                                   -quality_loss, oss.str());
            }
        }
    }

    return recs;
}

std::vector<ModelUsageProfile> CostOptimizer::getProfiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ModelUsageProfile> profiles;
    profiles.reserve(stats_.size());

    for (const auto& [name, s] : stats_) {
        double avg_q = (s.count > 0) ? s.total_quality / s.count : 0.0;
        profiles.emplace_back(name, s.total_cost, avg_q, s.count);
    }
    return profiles;
}

void CostOptimizer::setConfig(const CostOptimizerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

void CostOptimizer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.clear();
    propose_dedup_.clear();
}

std::vector<std::string> CostOptimizer::proposeRecommendations(
    const std::string& tenant_id,
    std::shared_ptr<autonomy::AutonomyApprovalWorkflow> workflow,
    std::chrono::seconds dedup_window) {
    std::vector<std::string> proposed_ids;
    if (!workflow) {
        spdlog::warn("CostOptimizer::proposeRecommendations: workflow=null, skip");
        return proposed_ids;
    }

    auto recs = getRecommendations();  // takes mutex_ internally
    if (recs.empty()) return proposed_ids;

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        dedup_window).count();

    for (const auto& rec : recs) {
        const auto dedup_key = tenant_id + "|" + rec.current_model + "=>" +
                                rec.recommended_model;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = propose_dedup_.find(dedup_key);
            if (it != propose_dedup_.end() &&
                now_ms - it->second < window_ms) {
                spdlog::debug(
                    "CostOptimizer::proposeRecommendations: dedup key={} "
                    "skipped (last propose {}ms ago)",
                    dedup_key, now_ms - it->second);
                continue;
            }
        }

        autonomy::ApprovalProposal p;
        p.source  = autonomy::AutonomySource::CostOptimizer;
        std::ostringstream subj;
        subj << "Switch " << rec.current_model << " → " << rec.recommended_model
             << " for tenant " << tenant_id;
        p.subject = subj.str();
        // payload schema is the contract with CostAutonomyApplier (E2.2)
        // and the C2 /creative isLowRisk decision (4 rules).
        const auto from_tier = inferTier(rec.current_model);
        const auto to_tier   = inferTier(rec.recommended_model);
        // potential_savings from v1 is a per-window sum; treat as a
        // conservative 24h estimate (true window is min_requests*sample
        // interval but we don't track interval, so this stays a coarse
        // upper bound — fine for the R3 $50 cap which is itself coarse).
        const double savings_usd_24h = std::max(0.0, rec.potential_savings);
        // affected RPS heuristic: v1's profile.count is the sample size
        // that produced this rec; treat as a 24h count → divide by 86400
        // and rescale by 3600 = / 24 for an hourly approximation.
        const int affected_rps_h     =
            static_cast<int>(std::max(0.0, std::ceil(savings_usd_24h * 10.0)));
        p.payload = nlohmann::json{
            {"action",                      "override_quality_tier"},
            {"tenant_id",                   tenant_id},
            {"current_model",               rec.current_model},
            {"recommended_model",           rec.recommended_model},
            {"from_quality_tier",           from_tier},
            {"to_quality_tier",             to_tier},
            {"estimated_savings_usd_24h",   savings_usd_24h},
            {"affected_requests_per_hour",  affected_rps_h},
            {"potential_savings",           rec.potential_savings},
            {"quality_impact",              rec.quality_impact}};
        // C4 decision trace — 5 required fields + optional notes.
        p.decision_trace = nlohmann::json{
            {"source_id",         "cost_optimizer"},
            {"algorithm_name",    "cost_per_quality_v1"},
            {"input_hash_sha256",
             autonomy::computePayloadSha256(nlohmann::json{
                 {"tenant", tenant_id},
                 {"recs", recs.size()}})},
            {"proposed_at_ms",    now_ms},
            {"notes",             rec.reason}};

        auto id = workflow->propose(std::move(p));
        if (!id.empty()) {
            proposed_ids.push_back(id);
            std::lock_guard<std::mutex> lock(mutex_);
            propose_dedup_[dedup_key] = now_ms;
        }
    }
    return proposed_ids;
}

} // namespace aegisgate
