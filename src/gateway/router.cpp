#include "router.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace aegisgate {

std::string BasicRouter::selectModel(RequestContext& ctx,
                                      const ConnectorRegistry& registry) {
    if (!ctx.chat_request.model.empty()) {
        if (registry.findByModel(ctx.chat_request.model)) {
            ctx.routing_decision_reason = "user_pinned";  // P1-7
            return ctx.chat_request.model;
        }
        spdlog::warn("Requested model {} not found, falling back to default",
                      ctx.chat_request.model);
    }

    if (ctx.chat_request.extra.contains("preferred_tag") &&
        ctx.chat_request.extra["preferred_tag"].is_string()) {
        auto tag = ctx.chat_request.extra["preferred_tag"].get<std::string>();
        auto models = registry.findModelsByTag(tag);
        if (!models.empty()) {
            ctx.routing_decision_reason = "router_tag";  // P1-7
            return models[0]->id;
        }
    }

    auto default_model = registry.defaultModel();
    if (!default_model.empty()) {
        ctx.routing_decision_reason = "router_default";  // P1-7
        return default_model;
    }

    spdlog::error("No model available for routing");
    return "";
}

// --- CostAwareRouter ---

size_t CostAwareRouter::estimateComplexity(const RequestContext& ctx) const {
    size_t total_chars = 0;
    for (const auto& msg : ctx.chat_request.messages) {
        total_chars += msg.content.size();
    }
    return total_chars;
}

std::string CostAwareRouter::selectByCharCount(const RequestContext& ctx,
                                                const ConnectorRegistry& registry) const {
    auto all_infos = registry.allModelInfos();
    if (all_infos.empty()) {
        spdlog::error("No model available for routing");
        return "";
    }

    std::sort(all_infos.begin(), all_infos.end(), [](const ModelInfo& a, const ModelInfo& b) {
        return (a.cost_per_1k_input + a.cost_per_1k_output) <
               (b.cost_per_1k_input + b.cost_per_1k_output);
    });

    std::vector<ModelInfo> available;
    for (const auto& info : all_infos) {
        if (registry.findByModel(info.id)) {
            available.push_back(info);
        }
    }
    if (available.empty()) {
        auto default_model = registry.defaultModel();
        return default_model.empty() ? "" : default_model;
    }

    size_t complexity = estimateComplexity(ctx);

    if (complexity < 500 && available.size() >= 1) {
        return available.front().id;
    }
    if (complexity >= 2000 && available.size() >= 3) {
        return available.back().id;
    }

    size_t mid = available.size() / 2;
    return available[mid].id;
}

std::string CostAwareRouter::selectModel(RequestContext& ctx,
                                          const ConnectorRegistry& registry) {
    if (!ctx.chat_request.model.empty()) {
        if (registry.findByModel(ctx.chat_request.model)) {
            ctx.routing_decision_reason = "user_pinned";  // P1-7
            return ctx.chat_request.model;
        }
        spdlog::warn("Requested model {} not found, falling back to cost-aware routing",
                      ctx.chat_request.model);
    }

    if (ctx.chat_request.extra.contains("preferred_tag") &&
        ctx.chat_request.extra["preferred_tag"].is_string()) {
        auto tag = ctx.chat_request.extra["preferred_tag"].get<std::string>();
        auto models = registry.findModelsByTag(tag);
        if (!models.empty()) {
            ctx.routing_decision_reason = "router_tag";  // P1-7
            return models[0]->id;
        }
    }

    std::string tier;
    if (ctx.chat_request.extra.contains("quality_tier") &&
        ctx.chat_request.extra["quality_tier"].is_string()) {
        tier = ctx.chat_request.extra["quality_tier"].get<std::string>();
    }

    auto all_models = registry.allModelInfos();
    if (all_models.empty()) {
        ctx.routing_decision_reason = "router_default";  // P1-7
        return selectByCharCount(ctx, registry);
    }

    if (tier == "economy" || tier == "premium") {
        std::sort(all_models.begin(), all_models.end(),
                  [](const ModelInfo& a, const ModelInfo& b) {
                      return (a.cost_per_1k_input + a.cost_per_1k_output) <
                             (b.cost_per_1k_input + b.cost_per_1k_output);
                  });
        if (tier == "economy") {
            for (const auto& info : all_models) {
                if (registry.findByModel(info.id)) {
                    ctx.routing_decision_reason = "router_economy";  // P1-7
                    return info.id;
                }
            }
            ctx.routing_decision_reason = "router_default";  // P1-7
            return selectByCharCount(ctx, registry);
        }
        if (tier == "premium") {
            for (auto it = all_models.rbegin(); it != all_models.rend(); ++it) {
                if (registry.findByModel(it->id)) {
                    ctx.routing_decision_reason = "router_quality";  // P1-7
                    return it->id;
                }
            }
            ctx.routing_decision_reason = "router_default";  // P1-7
            return selectByCharCount(ctx, registry);
        }
    }

    ctx.routing_decision_reason = "router_default";  // P1-7
    return selectByCharCount(ctx, registry);
}

} // namespace aegisgate
