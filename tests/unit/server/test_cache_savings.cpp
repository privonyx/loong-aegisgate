#include "server/cache_savings.h"

#include <gtest/gtest.h>

using namespace aegisgate;

namespace {

RequestContext makeCacheHitCtx() {
    RequestContext ctx;
    ctx.cache_hit = true;
    ctx.cached_response = "Paris is the capital of France.";
    ctx.chat_request.model = "gpt-4o";
    ctx.chat_request.messages = {
        {"system", "You are a helpful assistant."},
        {"user", "What is the capital of France?"},
    };
    return ctx;
}

} // namespace

// P1-10: with the prompt compressor disabled (default), tokens_estimated is
// never populated, yet a cache hit still avoids sending the prompt upstream.
// The saved-token count must be derived from the prompt directly, not gated on
// the compressor's tokens_estimated.
TEST(CacheSavingsP1_10, SavingsNonZeroWhenCompressionOff) {
    RequestContext ctx = makeCacheHitCtx();
    ctx.tokens_estimated = 0;  // compressor off / never ran

    EXPECT_GT(cacheSavedPromptTokens(ctx), 0);
}

// When the compressor did run and recorded an estimate, that authoritative
// value is preserved rather than re-estimated.
TEST(CacheSavingsP1_10, UsesCompressorEstimateWhenPresent) {
    RequestContext ctx = makeCacheHitCtx();
    ctx.tokens_estimated = 12345;

    EXPECT_EQ(cacheSavedPromptTokens(ctx), 12345);
}

// Decoupling contract: without a compressor estimate the saved count equals a
// direct estimate of the prompt messages (independent of any compression stage).
TEST(CacheSavingsP1_10, EqualsDirectPromptEstimateWhenNoCompressorValue) {
    RequestContext ctx = makeCacheHitCtx();
    ctx.tokens_estimated = 0;

    EXPECT_EQ(cacheSavedPromptTokens(ctx),
              TokenEstimator::estimateMessages(ctx.chat_request.messages));
}
