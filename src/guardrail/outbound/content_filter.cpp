#include "guardrail/outbound/content_filter.h"
#include "guardrail/re2_replacement_escape.h"
#include <spdlog/spdlog.h>

namespace aegisgate {

ContentFilter::ContentFilter() = default;

void ContentFilter::addPattern(const std::string& name,
                                const std::string& pattern,
                                FilterAction action,
                                const std::string& replacement) {
    ContentPattern cp;
    cp.name = name;
    cp.action = action;
    cp.replacement = replacement;
    cp.regex = std::make_unique<RE2>(pattern);
    if (cp.regex->ok()) {
        patterns_.push_back(std::move(cp));
    } else {
        spdlog::warn("Invalid content filter regex '{}': {}", name, cp.regex->error());
    }
}

void ContentFilter::addDefaultPatterns() {
    addPattern("profanity_en",
               "(?i)\\b(fuck|shit|damn|bitch|asshole)\\b",
               FilterAction::Replace, "[FILTERED]");
    addPattern("harmful_instructions",
               "(?i)(how\\s+to\\s+(kill|murder|poison|hack\\s+into))",
               FilterAction::Replace, "[FILTERED]");
    addPattern("pii_leak",
               "(?:sk|pk|api|key|token|secret)[_\\-]?[a-zA-Z0-9]{20,}",
               FilterAction::Replace, "[REDACTED]");
    // Stripe-style keys: sk_test_ / sk_live_ with segmented secret (not 20+ contiguous alnum)
    addPattern("api_key_stripe",
               "(?:sk|pk)_(?:test|live)_[a-zA-Z0-9]{10,}",
               FilterAction::Replace, "[REDACTED]");
}

FilterResult ContentFilter::filter(const std::string& text) const {
    FilterResult result;
    result.modified_text = text;

    auto actionPriority = [](FilterAction a) -> int {
        switch (a) {
            case FilterAction::Replace: return 2;
            case FilterAction::Truncate: return 1;
            case FilterAction::Warn: return 0;
        }
        return 0;
    };

    for (const auto& pat : patterns_) {
        if (RE2::PartialMatch(result.modified_text, *pat.regex)) {
            result.filtered = true;
            result.matched_patterns.push_back(pat.name);
            if (actionPriority(pat.action) > actionPriority(result.action_taken)) {
                result.action_taken = pat.action;
            }

            switch (pat.action) {
                case FilterAction::Replace: {
                    const std::string safe_rep = escapeRe2Replacement(pat.replacement);
                    RE2::GlobalReplace(&result.modified_text, *pat.regex, safe_rep);
                    break;
                }
                case FilterAction::Truncate: {
                    re2::StringPiece input(result.modified_text);
                    re2::StringPiece match;
                    if (pat.regex->Match(input, 0, input.size(),
                                         RE2::UNANCHORED, &match, 1)) {
                        auto pos = static_cast<size_t>(match.data() - input.data());
                        result.modified_text =
                            result.modified_text.substr(0, pos) + "[TRUNCATED]";
                    }
                    break;
                }
                case FilterAction::Warn:
                    break;
            }
        }
    }

    return result;
}

FilterResult ContentFilter::filterChunk(const std::string& chunk) const {
    return filter(chunk);
}

StageResult ContentFilter::process(RequestContext& ctx) {
    if (ctx.accumulated_response.empty()) return StageResult::Continue;

    auto result = filter(ctx.accumulated_response);
    if (result.filtered) {
        ctx.accumulated_response = std::move(result.modified_text);
        spdlog::info("Content filtered in response {}: patterns={}",
                     ctx.request_id, result.matched_patterns.size());
    }
    return StageResult::Continue;
}

StageResult ContentFilter::processChunk(RequestContext& ctx,
                                         std::string_view chunk) {
    std::string combined = prev_tail_ + std::string(chunk);
    auto result = filterChunk(combined);

    size_t overlap_start = prev_tail_.size();
    if (result.filtered) {
        if (result.modified_text.size() >= overlap_start) {
            ctx.chunk_output = result.modified_text.substr(overlap_start);
        } else {
            ctx.chunk_output = result.modified_text;
        }
        spdlog::info("Content filtered in response {}: patterns={}",
                     ctx.request_id, result.matched_patterns.size());
    } else {
        ctx.chunk_output = std::string(chunk);
    }

    if (chunk.size() >= kChunkOverlap) {
        prev_tail_ = std::string(chunk.substr(chunk.size() - kChunkOverlap));
    } else {
        prev_tail_ = std::string(chunk);
    }
    return StageResult::Continue;
}

} // namespace aegisgate
