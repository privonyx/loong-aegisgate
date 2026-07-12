#include "prompt_compressor.h"
#include "observe/token_estimator.h"
#include "gateway/connector/registry.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <unordered_set>

namespace aegisgate {

PromptCompressor::PromptCompressor() = default;

PromptCompressor::PromptCompressor(Config cfg) : cfg_(std::move(cfg)) {}

StageResult PromptCompressor::process(RequestContext& ctx) {
    if (!cfg_.enabled) return StageResult::Continue;

    auto& messages = ctx.chat_request.messages;
    if (messages.empty()) return StageResult::Continue;

    int tokens_before = TokenEstimator::estimateMessages(messages);

    if (cfg_.dedup_system_prompts) {
        dedupSystemPrompts(ctx);
    }
    if (cfg_.compress_whitespace) {
        compressWhitespace(ctx);
    }
    if (cfg_.max_context_messages > 0) {
        truncateMessages(ctx);
    }

    int tokens_after = TokenEstimator::estimateMessages(messages);
    last_tokens_saved_ = std::max(0, tokens_before - tokens_after);
    ctx.tokens_saved_compression = last_tokens_saved_;
    ctx.tokens_estimated = tokens_after;

    if (last_tokens_saved_ > 0) {
        spdlog::debug("PromptCompressor: saved {} tokens ({} → {})",
                      last_tokens_saved_, tokens_before, tokens_after);
    }

    return StageResult::Continue;
}

void PromptCompressor::truncateMessages(RequestContext& ctx) {
    auto& messages = ctx.chat_request.messages;
    size_t max_msgs = cfg_.max_context_messages;

    if (messages.size() <= max_msgs) return;

    std::vector<Message> preserved_msgs;
    std::vector<Message> non_preserved_msgs;
    for (auto& msg : messages) {
        if (msg.role == "system" || isToolMessage(msg)) {
            preserved_msgs.push_back(std::move(msg));
        } else {
            non_preserved_msgs.push_back(std::move(msg));
        }
    }

    size_t keep_count = max_msgs > preserved_msgs.size()
                            ? max_msgs - preserved_msgs.size()
                            : 0;

    if (non_preserved_msgs.size() > keep_count) {
        size_t drop = non_preserved_msgs.size() - keep_count;
        non_preserved_msgs.erase(non_preserved_msgs.begin(),
                              non_preserved_msgs.begin() + static_cast<ptrdiff_t>(drop));
    }

    messages.clear();
    for (auto& msg : preserved_msgs) {
        messages.push_back(std::move(msg));
    }
    for (auto& msg : non_preserved_msgs) {
        messages.push_back(std::move(msg));
    }
}

void PromptCompressor::compressWhitespace(RequestContext& ctx) {
    for (auto& msg : ctx.chat_request.messages) {
        if (isToolMessage(msg)) continue;
        std::string result;
        result.reserve(msg.content.size());
        bool last_was_space = false;
        for (char c : msg.content) {
            if (c == ' ' || c == '\t') {
                if (!last_was_space) {
                    result.push_back(' ');
                    last_was_space = true;
                }
            } else if (c == '\n' || c == '\r') {
                if (!last_was_space) {
                    result.push_back('\n');
                    last_was_space = true;
                }
            } else {
                result.push_back(c);
                last_was_space = false;
            }
        }
        if (!result.empty() && (result.back() == ' ' || result.back() == '\n')) {
            result.pop_back();
        }
        msg.content = std::move(result);
    }
}

void PromptCompressor::dedupSystemPrompts(RequestContext& ctx) {
    auto& messages = ctx.chat_request.messages;
    if (messages.size() < 2) return;

    std::unordered_set<std::string> seen;
    std::vector<Message> deduped;
    deduped.reserve(messages.size());

    for (auto& msg : messages) {
        if (msg.role == "system") {
            if (seen.find(msg.content) != seen.end()) {
                continue;
            }
            seen.insert(msg.content);
        }
        deduped.push_back(std::move(msg));
    }
    messages = std::move(deduped);
}

} // namespace aegisgate
