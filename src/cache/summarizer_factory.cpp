#include "cache/summarizer_factory.h"
#include "cache/composite_summarizer.h"
#include "cache/rule_based_summarizer.h"
#include "cache/onnx_summarizer.h"
#include <spdlog/spdlog.h>

namespace aegisgate {

std::unique_ptr<ConversationSummarizer>
makeSummarizer(const SummarizerConfig& cfg, const PIIFilter* pii) {
    auto rule = std::make_unique<RuleBasedSummarizer>(
        cfg.max_chars, cfg.top_keywords, pii);

    if (cfg.type == "onnx") {
        if (cfg.onnx_model_path.empty()) {
            spdlog::warn("SummarizerFactory: type=onnx but onnx_model_path empty; "
                         "falling back to RuleBased");
            return rule;
        }
        auto onnx = std::make_unique<OnnxSummarizer>(
            cfg.onnx_model_path,
            std::chrono::milliseconds(cfg.onnx_timeout_ms),
            pii);
        if (onnx->isReady()) {
            return std::make_unique<CompositeSummarizer>(
                std::move(onnx), std::move(rule));
        }
        spdlog::warn(
            "SummarizerFactory: ONNX init failed for '{}', using RuleBased only",
            cfg.onnx_model_path);
        return rule;
    }

    return rule;
}

} // namespace aegisgate
