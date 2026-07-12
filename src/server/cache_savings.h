#pragma once
#include "core/context.h"
#include "observe/token_estimator.h"

namespace aegisgate {

// P1-10: tokens saved by a cache hit. A cache hit short-circuits the inbound
// pipeline and never sends the prompt upstream, so the saved tokens are the
// prompt tokens we avoided. Historically this was gated on ctx.tokens_estimated,
// whose only writer is the prompt compressor stage; with compression disabled
// (the default) the value stayed 0 and cache savings were silently under-
// reported. We now estimate the prompt directly and only fall back to the
// compressor's authoritative figure when it actually ran.
//
// Header-only so the savings logic is unit-testable without driving the full
// GatewayRuntime hot path (which dispatches to the network).
inline int cacheSavedPromptTokens(const RequestContext& ctx) {
    if (ctx.tokens_estimated > 0) {
        return ctx.tokens_estimated;
    }
    return TokenEstimator::estimateMessages(ctx.chat_request.messages);
}

} // namespace aegisgate
