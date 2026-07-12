#pragma once
#include "core/pipeline.h"
#include <re2/re2.h>
#include <string>
#include <vector>
#include <memory>

namespace aegisgate {

enum class FilterAction { Replace, Truncate, Warn };

struct ContentPattern {
    std::string name;
    std::unique_ptr<RE2> regex;
    FilterAction action;
    std::string replacement;
};

struct FilterResult {
    bool filtered = false;
    std::string modified_text;
    std::vector<std::string> matched_patterns;
    FilterAction action_taken = FilterAction::Warn;
};

class ContentFilter : public PipelineStage {
public:
    ContentFilter();

    void addPattern(const std::string& name, const std::string& pattern,
                    FilterAction action, const std::string& replacement = "[FILTERED]");
    void addDefaultPatterns();

    FilterResult filter(const std::string& text) const;
    FilterResult filterChunk(const std::string& chunk) const;

    StageResult process(RequestContext& ctx) override;
    StageResult processChunk(RequestContext& ctx,
                             std::string_view chunk) override;
    std::string name() const override { return "ContentFilter"; }

private:
    std::vector<ContentPattern> patterns_;
    static constexpr size_t kChunkOverlap = 40;
    std::string prev_tail_;
};

} // namespace aegisgate
