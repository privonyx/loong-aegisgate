#include "rag/retrieval_stage.h"
#include "observe/metrics.h"
#include <spdlog/spdlog.h>

namespace aegisgate {

RetrievalStage::RetrievalStage() = default;

RetrievalStage::RetrievalStage(RetrievalConfig config)
    : config_(std::move(config)) {}

void RetrievalStage::setKnowledgeBase(KnowledgeBase* kb) {
    kb_ = kb;
}

StageResult RetrievalStage::process(RequestContext& ctx) {
    if (!config_.enabled || kb_ == nullptr) {
        return StageResult::Continue;
    }

    std::string user_query;
    for (auto it = ctx.chat_request.messages.rbegin();
         it != ctx.chat_request.messages.rend(); ++it) {
        if (it->role == "user") {
            user_query = it->content;
            break;
        }
    }

    if (user_query.empty()) {
        return StageResult::Continue;
    }

    auto results = kb_->search(user_query, config_.top_k, config_.min_relevance);
    if (results.empty()) {
        spdlog::debug("RetrievalStage: no results for query");
        return StageResult::Continue;
    }

    // P1-B: count each retrieval that actually injects knowledge into the prompt.
    MetricsRegistry::instance().ragRetrievalsTotal().inc();

    std::string context_block = buildContextBlock(results);
    injectContext(ctx, context_block);

    for (const auto& r : results) {
        ctx.retrieval_sources.emplace_back(r.chunk_id, r.document_id,
                                           r.content, r.relevance);
        ctx.citations.emplace_back(r.chunk_id, r.document_id,
                                   r.relevance, r.content.substr(0, 100));
    }

    spdlog::debug("RetrievalStage: injected {} sources for req={}",
                  results.size(), ctx.request_id);
    return StageResult::Continue;
}

std::string RetrievalStage::buildContextBlock(
    const std::vector<RetrievalResult>& results) const {
    std::string block = "Based on the following knowledge:\n";
    // I32 (TASK-20260703-04)：max_context_tokens 预算生效（此前无限拼接，长文档
    // 可撑爆上游上下文窗口 / 抬高成本）。粗略 token≈4 字符（与网关既有估算一致）；
    // <=0 视为不限。逐条累加，超预算即停并标注截断。
    const int budget_tokens = config_.max_context_tokens;
    const std::size_t budget_chars =
        budget_tokens > 0 ? static_cast<std::size_t>(budget_tokens) * 4
                          : std::string::npos;
    bool truncated = false;
    for (size_t i = 0; i < results.size(); ++i) {
        std::string entry =
            "[" + std::to_string(i + 1) + "] " + results[i].content + "\n";
        if (budget_chars != std::string::npos &&
            block.size() + entry.size() > budget_chars) {
            truncated = true;
            break;
        }
        block += entry;
    }
    if (truncated) {
        block += "[context truncated to fit token budget]\n";
    }
    return block;
}

void RetrievalStage::injectContext(RequestContext& ctx,
                                   const std::string& context_block) const {
    Message context_msg("system", context_block);

    auto& msgs = ctx.chat_request.messages;

    switch (config_.injection_position) {
    case InjectionPosition::BeforeSystem:
        msgs.insert(msgs.begin(), std::move(context_msg));
        break;

    case InjectionPosition::AfterSystem: {
        auto it = msgs.begin();
        while (it != msgs.end() && it->role == "system") {
            ++it;
        }
        msgs.insert(it, std::move(context_msg));
        break;
    }

    case InjectionPosition::BeforeUser: {
        auto it = msgs.end();
        for (auto rit = msgs.rbegin(); rit != msgs.rend(); ++rit) {
            if (rit->role == "user") {
                it = rit.base();
                --it;
                break;
            }
        }
        msgs.insert(it, std::move(context_msg));
        break;
    }
    }
}

} // namespace aegisgate
