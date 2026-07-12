#include <gtest/gtest.h>
#include "core/pipeline.h"
#include "core/context.h"
#include "server/sse_response.h"
#include "guardrail/outbound/content_filter.h"
#include "guardrail/inbound/pii_filter.h"

using namespace aegisgate;

class StreamingIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto cf = std::make_unique<ContentFilter>();
        cf->addDefaultPatterns();
        outbound_.addStage(std::move(cf));
    }

    Pipeline outbound_;
};

TEST_F(StreamingIntegrationTest, ExecuteChunkPipelinePassesCleanChunks) {
    RequestContext ctx;
    ctx.is_streaming = true;

    auto [result, output] = outbound_.executeChunk(ctx, "Hello world");
    EXPECT_EQ(result, PipelineResult::Success);
    EXPECT_EQ(output, "Hello world");
}

TEST_F(StreamingIntegrationTest, ExecuteChunkPipelineFiltersContent) {
    RequestContext ctx;
    ctx.is_streaming = true;

    auto [result, output] = outbound_.executeChunk(
        ctx, "Key: token_DEMOABCDEFGHIJKLMNOPQRSTUV");
    EXPECT_EQ(result, PipelineResult::Success);
    EXPECT_TRUE(output.find("token_DEMOABCDEFGHIJKLMNOPQRSTUV") == std::string::npos);
}

TEST_F(StreamingIntegrationTest, MultipleChunksAccumulate) {
    RequestContext ctx;
    ctx.is_streaming = true;

    std::vector<std::string> chunks = {"Hello ", "world, ", "how are you?"};
    for (const auto& chunk : chunks) {
        ctx.accumulated_response += chunk;
        auto [result, output] = outbound_.executeChunk(ctx, chunk);
        EXPECT_EQ(result, PipelineResult::Success);
        EXPECT_FALSE(output.empty());
    }
    EXPECT_EQ(ctx.accumulated_response, "Hello world, how are you?");
}

TEST_F(StreamingIntegrationTest, SseFormatIsValid) {
    auto formatted = SseResponseWriter::formatSseChunk(
        "test content", "gpt-4o", "chatcmpl-001");
    EXPECT_TRUE(formatted.find("data: ") == 0);
    EXPECT_TRUE(formatted.find("\"content\":\"test content\"") != std::string::npos);
    EXPECT_TRUE(formatted.find("\"model\":\"gpt-4o\"") != std::string::npos);
    EXPECT_TRUE(formatted.find("\n\n") != std::string::npos);
}

TEST_F(StreamingIntegrationTest, SseDoneFormat) {
    auto done = SseResponseWriter::formatSseDone();
    EXPECT_EQ(done, "data: [DONE]\n\n");
}

TEST_F(StreamingIntegrationTest, PIIFilterChunkInPipeline) {
    Pipeline inbound;
    inbound.addStage(std::make_unique<PIIFilter>());

    RequestContext ctx;
    ctx.is_streaming = true;
    auto [result, output] = inbound.executeChunk(
        ctx, "Email me at user@example.com");
    EXPECT_EQ(result, PipelineResult::Success);
    EXPECT_TRUE(output.find("user@example.com") == std::string::npos);
    EXPECT_TRUE(output.find("[EMAIL]") != std::string::npos);
}

TEST_F(StreamingIntegrationTest, FullStreamSimulation) {
    RequestContext ctx;
    ctx.is_streaming = true;
    ctx.stream_model = "gpt-4o";
    ctx.request_id = "test-stream-001";

    std::vector<std::string> llm_chunks = {
        "The answer ",
        "to your question ",
        "is 42."
    };

    std::string sse_output;
    int chunk_idx = 0;

    for (const auto& chunk : llm_chunks) {
        ctx.accumulated_response += chunk;
        auto [result, filtered] = outbound_.executeChunk(ctx, chunk);
        ASSERT_EQ(result, PipelineResult::Success);

        auto sse = SseResponseWriter::formatSseChunk(
            filtered, ctx.stream_model,
            "chatcmpl-" + std::to_string(chunk_idx++));
        sse_output += sse;
    }
    sse_output += SseResponseWriter::formatSseDone();

    EXPECT_EQ(ctx.accumulated_response, "The answer to your question is 42.");
    EXPECT_TRUE(sse_output.find("data: [DONE]") != std::string::npos);
    int data_count = 0;
    size_t pos = 0;
    while ((pos = sse_output.find("data: ", pos)) != std::string::npos) {
        data_count++;
        pos += 6;
    }
    EXPECT_EQ(data_count, 4);
}
