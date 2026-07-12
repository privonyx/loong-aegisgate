#include "server/budget_guard_stage.h"

#include "core/context.h"
#include "observe/cost_tracker.h"
#include "observe/metrics.h"

#include <spdlog/spdlog.h>
#include <exception>

namespace aegisgate {

BudgetGuardStage::BudgetGuardStage(std::shared_ptr<CostTracker> tracker,
                                    std::shared_ptr<MLRouter> router,
                                    BudgetGuardConfig cfg)
    : tracker_(std::move(tracker)),
      router_(std::move(router)),
      cfg_(std::move(cfg)) {}

void BudgetGuardStage::setConfig(const BudgetGuardConfig& cfg) {
    std::lock_guard<std::mutex> lock(cfg_mutex_);
    cfg_ = cfg;
}

BudgetGuardConfig BudgetGuardStage::config() const {
    std::lock_guard<std::mutex> lock(cfg_mutex_);
    return cfg_;
}

double BudgetGuardStage::estimateRequestCost(const RequestContext& ctx) const {
    if (!tracker_) return 0.0;
    const auto& model = ctx.target_model.empty()
                          ? ctx.chat_request.model
                          : ctx.target_model;
    if (model.empty()) return 0.0;

    // tokens_estimated is populated by upstream preprocessing when
    // available; output tokens are unknown pre-flight so we charge
    // them at the same rate as input (conservative upper bound).
    int est_input  = ctx.tokens_estimated > 0 ? ctx.tokens_estimated : 64;
    int est_output = est_input;
    auto rec = tracker_->calculate(model, est_input, est_output);
    return rec.total_cost;
}

StageResult BudgetGuardStage::process(RequestContext& ctx) {
    BudgetGuardConfig cfg;
    {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        cfg = cfg_;
    }
    if (!cfg.enabled) return StageResult::Continue;
    if (ctx.tenant_id.empty()) {
        // No tenant context → can't enforce per-tenant budgets. Pass through.
        return StageResult::Continue;
    }

    try {
        const double request_cost = estimateRequestCost(ctx);
        const double tenant_24h   =
            tracker_ ? tracker_->getTenantCostInWindow(
                            ctx.tenant_id, std::chrono::hours{24})
                     : 0.0;

        const bool exceeds_24h     =
            tenant_24h + request_cost > cfg.per_tenant_24h_usd;
        const bool exceeds_request =
            request_cost > cfg.per_request_max_usd;

        if (!exceeds_24h && !exceeds_request) {
            return StageResult::Continue;
        }

        // Fail-CLOSED downgrade (graceful degradation, not rejection).
        ctx.chat_request.extra["quality_tier"] = cfg.downgrade_tier;
        if (!cfg.downgrade_header_name.empty()) {
            ctx.response_headers[cfg.downgrade_header_name] =
                cfg.downgrade_header_value;
        }
        const std::string reason =
            exceeds_24h ? "tenant_24h" : "request_estimate";
        LabelSet labels;
        labels.labels.emplace_back("tenant_id", ctx.tenant_id);
        labels.labels.emplace_back("reason", reason);
        MetricsRegistry::instance().budgetGuardTriggered().inc(labels);
        spdlog::warn(
            "BudgetGuard: tenant={} downgraded to {} (reason={}, "
            "tenant_24h_usd={:.4f}, request_estimate_usd={:.4f}, "
            "limit_24h_usd={:.2f}, limit_request_usd={:.2f})",
            ctx.tenant_id, cfg.downgrade_tier, reason,
            tenant_24h, request_cost,
            cfg.per_tenant_24h_usd, cfg.per_request_max_usd);
        return StageResult::Continue;
    } catch (const std::exception& e) {
        // T08 / SR4: BudgetGuard itself must never take down the data plane.
        if (cfg.fail_open_on_error) {
            spdlog::error("BudgetGuard: fail-open ({}): {}",
                          ctx.tenant_id, e.what());
            return StageResult::Continue;
        }
        spdlog::error("BudgetGuard: fail-closed ({}): {} — rejecting",
                      ctx.tenant_id, e.what());
        return StageResult::Reject;
    }
}

} // namespace aegisgate
