#pragma once
#include "cache/conversation_summarizer.h"

namespace aegisgate {

class PIIFilter;

// Zero-dependency rule-based summarizer: tokenize -> frequency rank -> top-K join.
// Always-available fallback path for the D3=B dual-path summarization design.
//
// PII handling (SR4): when `pii` is non-null, each message's content is masked
// before tokenization, so phone numbers / IDs never reach the partition_key
// or any downstream metric/log.
class RuleBasedSummarizer : public ConversationSummarizer {
public:
    explicit RuleBasedSummarizer(size_t max_chars = 512,
                                 size_t top_keywords = 5,
                                 const PIIFilter* pii = nullptr);

    std::string summarize(const std::vector<Message>& msgs) override;
    std::string name() const override { return "RuleBased"; }

private:
    std::vector<std::string> tokenize(const std::string& text) const;
    std::string maskPii(const std::string& text) const;

    size_t max_chars_;
    size_t top_keywords_;
    const PIIFilter* pii_;
};

} // namespace aegisgate
