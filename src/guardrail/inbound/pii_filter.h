#pragma once
#include "core/pipeline.h"
#include <re2/re2.h>
#include <re2/set.h>
#include <shared_mutex>
#include <string>
#include <vector>
#include <memory>

namespace aegisgate {

struct PIIPattern {
    std::string name;
    std::string replacement;
    std::unique_ptr<RE2> regex;
};

class PIIFilter : public PipelineStage {
public:
    PIIFilter();

    void loadPatterns(const std::string& yaml_path);
    void reloadPatterns(const std::string& yaml_path);
    void addPattern(const std::string& name, const std::string& pattern,
                    const std::string& replacement);

    std::string mask(const std::string& text) const;

    // P1-3 / SR-2: when outbound, process() masks ctx.accumulated_response
    // (the model response) instead of the inbound request messages. Streaming
    // is handled by processChunk(), which already operates outbound.
    void setOutbound(bool outbound) { outbound_ = outbound; }

    StageResult process(RequestContext& ctx) override;
    StageResult processChunk(RequestContext& ctx,
                             std::string_view chunk) override;
    std::string name() const override { return "PIIFilter"; }

private:
    void addDefaultPatterns();
    std::vector<PIIPattern> patterns_;
    bool outbound_ = false;  // P1-3: false=mask request msgs, true=mask response
    static constexpr size_t kChunkOverlap = 40;
    std::string prev_tail_;
    mutable std::shared_mutex patterns_mutex_;  // Lock Layer 1
};

} // namespace aegisgate
