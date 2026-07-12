#include "guardrail/inbound/prompt_template_stage.h"

namespace aegisgate {

StageResult PromptTemplateStage::process(RequestContext& ctx) {
    if (!service_) return StageResult::Continue;
    if (ctx.tenant_id.empty()) return StageResult::Continue;

    for (const auto& m : ctx.chat_request.messages) {
        if (m.role == "system") return StageResult::Continue;
    }

    std::optional<PromptTemplate> tpl;
    auto it = ctx.request_headers.find(kTemplateHeader);
    if (it != ctx.request_headers.end() && !it->second.empty()) {
        tpl = service_->selectTemplate(ctx.tenant_id, it->second);
    } else {
        tpl = service_->selectDefaultActive(ctx.tenant_id);
    }
    if (!tpl) return StageResult::Continue;

    Message sys;
    sys.role = "system";
    sys.content = tpl->content;
    ctx.chat_request.messages.insert(ctx.chat_request.messages.begin(),
                                     std::move(sys));
    ctx.response_headers[kAppliedHeader] =
        tpl->name.empty() ? tpl->id : tpl->name;
    return StageResult::Continue;
}

} // namespace aegisgate
