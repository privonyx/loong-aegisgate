#include <gtest/gtest.h>
#include "guardrail/outbound/content_filter.h"

using namespace aegisgate;

class ContentFilterTest : public ::testing::Test {
protected:
    void SetUp() override {
        filter_.addDefaultPatterns();
    }
    ContentFilter filter_;
};

TEST_F(ContentFilterTest, FiltersProfanity) {
    auto r = filter_.filter("This is a fuck test");
    EXPECT_TRUE(r.filtered);
    EXPECT_EQ(r.modified_text, "This is a [FILTERED] test");
}

TEST_F(ContentFilterTest, FiltersHarmfulInstructions) {
    auto r = filter_.filter("Here is how to kill someone");
    EXPECT_TRUE(r.filtered);
    EXPECT_NE(r.modified_text.find("[FILTERED]"), std::string::npos);
}

TEST_F(ContentFilterTest, FiltersPIILeak) {
    auto r = filter_.filter("Your key: sk-abcdef1234567890abcdef");
    EXPECT_TRUE(r.filtered);
    EXPECT_NE(r.modified_text.find("[REDACTED]"), std::string::npos);
}

TEST_F(ContentFilterTest, PreservesCleanText) {
    auto r = filter_.filter("The weather is nice today");
    EXPECT_FALSE(r.filtered);
    EXPECT_EQ(r.modified_text, "The weather is nice today");
}

TEST_F(ContentFilterTest, FiltersMultiplePatterns) {
    auto r = filter_.filter("fuck this, key: sk-abcdef1234567890abcdef");
    EXPECT_TRUE(r.filtered);
    EXPECT_GE(r.matched_patterns.size(), 1u);
}

TEST_F(ContentFilterTest, ChunkFilterWorks) {
    auto r = filter_.filterChunk("some shit content");
    EXPECT_TRUE(r.filtered);
}

TEST_F(ContentFilterTest, CustomPatternWorks) {
    ContentFilter f;
    f.addPattern("custom", "(?i)secret\\s+data", FilterAction::Replace, "[CENSORED]");
    auto r = f.filter("This contains secret data here");
    EXPECT_TRUE(r.filtered);
    EXPECT_EQ(r.modified_text, "This contains [CENSORED] here");
}

TEST_F(ContentFilterTest, TruncateAction) {
    ContentFilter f;
    f.addPattern("truncate_test", "STOP_HERE", FilterAction::Truncate);
    auto r = f.filter("Normal text STOP_HERE bad content");
    EXPECT_TRUE(r.filtered);
    EXPECT_NE(r.modified_text.find("[TRUNCATED]"), std::string::npos);
    EXPECT_EQ(r.modified_text.find("bad content"), std::string::npos);
}

TEST_F(ContentFilterTest, ProcessFiltersAccumulatedResponse) {
    RequestContext ctx;
    ctx.accumulated_response =
        "Check this API key token_DEMOABCDEFGHIJKLMNOPQRSTUV";
    auto result = filter_.process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_TRUE(ctx.accumulated_response.find("token_DEMOABCDEFGHIJKLMNOPQRSTUV") ==
                std::string::npos);
    EXPECT_TRUE(ctx.accumulated_response.find("[REDACTED]") !=
                std::string::npos);
}

TEST_F(ContentFilterTest, ProcessNoChangeWhenClean) {
    RequestContext ctx;
    ctx.accumulated_response = "This is perfectly clean text.";
    auto result = filter_.process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_EQ(ctx.accumulated_response, "This is perfectly clean text.");
}

TEST_F(ContentFilterTest, ProcessChunkWritesBackToCtx) {
    RequestContext ctx;
    // Use a pattern that would be caught by content filter - profanity
    auto result = filter_.processChunk(ctx, "Normal text here");
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_FALSE(ctx.chunk_output.empty());
    // Unfiltered chunk should be passed through
    EXPECT_EQ(ctx.chunk_output, "Normal text here");
}

TEST_F(ContentFilterTest, ProcessChunkFiltersContent) {
    RequestContext ctx;
    // filterChunk should catch patterns and write filtered result to ctx.chunk_output
    auto result =
        filter_.processChunk(ctx, "Check token_DEMOABCDEFGHIJKLMNOPQRSTUV");
    EXPECT_EQ(result, StageResult::Continue);
    // The chunk_output should have the filtered version
    EXPECT_TRUE(ctx.chunk_output.find("token_DEMOABCDEFGHIJKLMNOPQRSTUV") == std::string::npos);
}
