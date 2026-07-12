#pragma once
#include "core/pipeline.h"
#include "auth/prompt_template_service.h"

namespace aegisgate {

// TASK-20260709-01 / REV20260707-I5: inject a tenant prompt template as a
// leading system message when the request has no system role yet.
class PromptTemplateStage : public PipelineStage {
public:
    static constexpr const char* kTemplateHeader = "X-AegisGate-Template";
    static constexpr const char* kAppliedHeader = "X-AegisGate-Template-Applied";

    void setService(PromptTemplateService* service) { service_ = service; }

    StageResult process(RequestContext& ctx) override;
    std::string name() const override { return "PromptTemplateStage"; }

private:
    PromptTemplateService* service_ = nullptr;
};

} // namespace aegisgate
