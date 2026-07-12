#pragma once
#include "core/pipeline.h"
#include "rag/knowledge_base.h"
#include <string>

namespace aegisgate {

enum class InjectionPosition { BeforeSystem, AfterSystem, BeforeUser };

struct RetrievalConfig {
    bool enabled = false;
    int top_k = 3;
    float min_relevance = 0.7f;
    int max_context_tokens = 2000;
    InjectionPosition injection_position = InjectionPosition::BeforeUser;
};

class RetrievalStage : public PipelineStage {
public:
    RetrievalStage();
    explicit RetrievalStage(RetrievalConfig config);

    void setKnowledgeBase(KnowledgeBase* kb);
    void setConfig(const RetrievalConfig& cfg) { config_ = cfg; }
    const RetrievalConfig& retrievalConfig() const { return config_; }

    StageResult process(RequestContext& ctx) override;
    std::string name() const override { return "RetrievalStage"; }

private:
    std::string buildContextBlock(const std::vector<RetrievalResult>& results) const;
    void injectContext(RequestContext& ctx, const std::string& context_block) const;

    RetrievalConfig config_;
    KnowledgeBase* kb_ = nullptr;
};

} // namespace aegisgate
