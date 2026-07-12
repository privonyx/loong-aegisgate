#include "smart_max_tokens.h"
#include "observe/token_estimator.h"
#include "gateway/connector/registry.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace aegisgate {

SmartMaxTokens::SmartMaxTokens() = default;

SmartMaxTokens::SmartMaxTokens(Config cfg) : cfg_(std::move(cfg)) {}

StageResult SmartMaxTokens::process(RequestContext& ctx) {
    if (!cfg_.enabled) return StageResult::Continue;

    if (ctx.chat_request.max_tokens.has_value()) {
        return StageResult::Continue;
    }

    int estimated_input = ctx.tokens_estimated > 0
                              ? ctx.tokens_estimated
                              : TokenEstimator::estimateMessages(ctx.chat_request.messages);

    int model_max_context = 4096;
    if (registry_) {
        const auto* info = registry_->findModelInfo(ctx.chat_request.model);
        if (!info && !ctx.target_model.empty()) {
            info = registry_->findModelInfo(ctx.target_model);
        }
        if (info) {
            model_max_context = info->max_context_tokens;
        }
    }

    int remaining = model_max_context - estimated_input;
    if (remaining < cfg_.min_output_tokens) {
        remaining = cfg_.min_output_tokens;
    }

    int ratio_based = static_cast<int>(static_cast<double>(estimated_input) * cfg_.max_output_ratio);

    int smart_max = std::min({cfg_.default_max_output, ratio_based, remaining});
    smart_max = std::max(smart_max, cfg_.min_output_tokens);

    ctx.chat_request.max_tokens = smart_max;

    spdlog::debug("SmartMaxTokens: input≈{}, model_ctx={}, set max_tokens={}",
                  estimated_input, model_max_context, smart_max);

    return StageResult::Continue;
}

} // namespace aegisgate
