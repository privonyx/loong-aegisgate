#include "cache/composite_summarizer.h"
#include <spdlog/spdlog.h>

namespace aegisgate {

CompositeSummarizer::CompositeSummarizer(
    std::unique_ptr<ConversationSummarizer> primary,
    std::unique_ptr<ConversationSummarizer> fallback)
    : primary_(std::move(primary)), fallback_(std::move(fallback)) {}

std::string CompositeSummarizer::summarize(const std::vector<Message>& msgs) {
    if (primary_) {
        auto out = primary_->summarize(msgs);
        if (!out.empty()) return out;
    }
    if (!fallback_) return "";
    fallback_count_.fetch_add(1, std::memory_order_relaxed);
    spdlog::warn(
        "ConversationSummarizer fallback: primary={} returned empty, using fallback={}",
        primary_ ? primary_->name() : "<null>",
        fallback_->name());
    return fallback_->summarize(msgs);
}

std::string CompositeSummarizer::name() const {
    const std::string p = primary_ ? primary_->name() : "<null>";
    const std::string f = fallback_ ? fallback_->name() : "<null>";
    return p + "+" + f;
}

} // namespace aegisgate
