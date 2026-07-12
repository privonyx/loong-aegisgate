#include "gateway/ab_test_router.h"
#include <spdlog/spdlog.h>
#include <functional>

namespace aegisgate {

ABTestRouter::ABTestRouter(std::unique_ptr<Router> base_router,
                           std::vector<ABExperiment> experiments)
    : base_(std::move(base_router)), experiments_(std::move(experiments)) {}

std::string ABTestRouter::assignVariant(const ABExperiment& exp,
                                         const std::string& request_id) const {
    int total_weight = 0;
    for (const auto& v : exp.variants) {
        total_weight += v.weight;
    }
    if (total_weight <= 0 || exp.variants.empty()) {
        return "";
    }

    auto hash_val = std::hash<std::string>{}(exp.name + request_id);
    auto bucket = static_cast<int>(hash_val % static_cast<size_t>(total_weight));

    int cumulative = 0;
    for (const auto& v : exp.variants) {
        cumulative += v.weight;
        if (bucket < cumulative) {
            return v.model;
        }
    }
    return exp.variants.back().model;
}

std::string ABTestRouter::selectModel(RequestContext& ctx,
                                       const ConnectorRegistry& registry) {
    if (!ctx.chat_request.model.empty()) {
        if (registry.findByModel(ctx.chat_request.model)) {
            return ctx.chat_request.model;
        }
    }

    for (const auto& exp : experiments_) {
        if (!exp.enabled) continue;

        if (!exp.tenant_id.empty() && exp.tenant_id != ctx.tenant_id) continue;

        auto assigned = assignVariant(exp, ctx.request_id);
        if (assigned.empty()) continue;

        if (!registry.findByModel(assigned)) {
            spdlog::warn("ABTestRouter: variant model {} not available, "
                         "falling back to base router", assigned);
            continue;
        }

        ctx.ab_experiment = exp.name;
        ctx.ab_variant = assigned;
        spdlog::debug("ABTestRouter: experiment={}, variant={}, request={}",
                       exp.name, assigned, ctx.request_id);
        return assigned;
    }

    return base_->selectModel(ctx, registry);
}

}  // namespace aegisgate
