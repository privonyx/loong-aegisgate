#pragma once
#include "core/pipeline.h"
#include <cstddef>

namespace aegisgate {

class ConnectorRegistry;

class PromptCompressor : public PipelineStage {
public:
    struct Config {
        bool enabled = true;
        size_t max_context_messages = 20;
        bool compress_whitespace = true;
        bool dedup_system_prompts = true;
    };

    PromptCompressor();
    explicit PromptCompressor(Config cfg);

    void setConnectorRegistry(const ConnectorRegistry* registry) {
        registry_ = registry;
    }

    StageResult process(RequestContext& ctx) override;
    std::string name() const override { return "PromptCompressor"; }

    int lastTokensSaved() const { return last_tokens_saved_; }

private:
    void truncateMessages(RequestContext& ctx);
    void compressWhitespace(RequestContext& ctx);
    void dedupSystemPrompts(RequestContext& ctx);

    Config cfg_;
    const ConnectorRegistry* registry_ = nullptr;
    int last_tokens_saved_ = 0;
};

} // namespace aegisgate
