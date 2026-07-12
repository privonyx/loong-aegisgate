#pragma once
#include "cache/conversation_summarizer.h"
#include <atomic>
#include <memory>

namespace aegisgate {

// CR1 scheme B: primary + fallback decorator.
//
// Semantics:
//   summarize(msgs):
//     out = primary->summarize(msgs)
//     if !out.empty(): return out
//     fallback_count_++ ; spdlog::warn(...) ; return fallback->summarize(msgs)
//
// Observability:
//   fallbackCount() returns the running counter; surfaced via
//   MetricsRegistry::conversation_summarizer_fallback_total (wired in Epic 1.5
//   / runtime assembly).
class CompositeSummarizer : public ConversationSummarizer {
public:
    CompositeSummarizer(std::unique_ptr<ConversationSummarizer> primary,
                        std::unique_ptr<ConversationSummarizer> fallback);

    std::string summarize(const std::vector<Message>& msgs) override;
    std::string name() const override;

    size_t fallbackCount() const { return fallback_count_.load(std::memory_order_relaxed); }

private:
    std::unique_ptr<ConversationSummarizer> primary_;
    std::unique_ptr<ConversationSummarizer> fallback_;
    std::atomic<size_t> fallback_count_{0};
};

} // namespace aegisgate
