#pragma once
#include "aegisgate/types.h"
#include "core/context.h"
#include "cache/semantic_cache.h"
#include "cache/conversation_id_resolver.h"
#include <string>

namespace aegisgate {

// P0-3 (SR-3, D3=B2): outbound stages (e.g. ContentFilter) redact
// ctx.accumulated_response in place. This helper reflects that filtered content
// into the non-streaming response body AND caches the filtered content so cache
// hits stay redacted too. It MUST be invoked AFTER the outbound pipeline
// succeeds (so accumulated_response already carries any redactions) and the
// cache write replaces the previous pre-outbound write of unfiltered content.
//
// Extracted as a header-only helper so the SR-3 invariant is unit-testable
// without spinning up the Drogon-coupled GatewayRuntime (no end-to-end
// processRequest harness exists). The production gateway calls this exact
// function, so a mutation here is caught by the unit tests.
//
// The conversation_id is sourced from the cache's injected resolver to stay
// consistent with the read path in SemanticCache::process().
inline void finalizeNonStreamingResponse(ChatResponse& response,
                                         SemanticCache* cache,
                                         const RequestContext& ctx) {
    response.content = ctx.accumulated_response;
    if (cache && !ctx.chat_request.messages.empty() && !ctx.has_tools) {
        std::string conv_id = cache->conversationIdResolver()
            ? cache->conversationIdResolver()->resolve(ctx.chat_request) : "";
        cache->putFromContext(ctx.chat_request.messages, ctx.accumulated_response,
                              ctx.target_model, ctx.tenant_id, conv_id);
    }
}

} // namespace aegisgate
