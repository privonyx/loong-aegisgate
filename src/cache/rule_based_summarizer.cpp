#include "cache/rule_based_summarizer.h"
#include "guardrail/inbound/pii_filter.h"
#include <algorithm>
#include <map>

namespace aegisgate {

RuleBasedSummarizer::RuleBasedSummarizer(size_t max_chars, size_t top_keywords,
                                         const PIIFilter* pii)
    : max_chars_(max_chars), top_keywords_(top_keywords), pii_(pii) {}

std::string RuleBasedSummarizer::maskPii(const std::string& text) const {
    return pii_ ? pii_->mask(text) : text;
}

std::vector<std::string> RuleBasedSummarizer::tokenize(const std::string& text) const {
    std::vector<std::string> tokens;
    std::string cur;
    for (unsigned char c : text) {
        bool is_word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || (c >= 0x80);
        if (is_word) {
            cur.push_back(static_cast<char>(c));
        } else if (!cur.empty()) {
            tokens.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

std::string RuleBasedSummarizer::summarize(const std::vector<Message>& msgs) {
    if (msgs.empty() || top_keywords_ == 0 || max_chars_ == 0) return "";

    std::map<std::string, size_t> freq;
    for (const auto& m : msgs) {
        auto masked = maskPii(m.content);
        for (auto& tok : tokenize(masked)) {
            // Skip single-character tokens to reduce noise.
            if (tok.size() >= 2) ++freq[tok];
        }
    }

    std::vector<std::pair<std::string, size_t>> sorted(freq.begin(), freq.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });

    std::string out;
    const size_t k = std::min(sorted.size(), top_keywords_);
    for (size_t i = 0; i < k; ++i) {
        const size_t sep = out.empty() ? 0 : 1;
        if (out.size() + sep + sorted[i].first.size() > max_chars_) break;
        if (!out.empty()) out.push_back(' ');
        out.append(sorted[i].first);
    }
    return out;
}

} // namespace aegisgate
