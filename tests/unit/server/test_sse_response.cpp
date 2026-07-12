#include <gtest/gtest.h>
#include "server/sse_response.h"
#include <nlohmann/json.hpp>

using namespace aegisgate;

namespace {

void parseSseDataJson(const std::string& formatted, nlohmann::json& out) {
    const std::string prefix = "data: ";
    ASSERT_GE(formatted.size(), prefix.size());
    ASSERT_EQ(formatted.substr(0, prefix.size()), prefix);
    auto end = formatted.find("\n\n");
    ASSERT_NE(end, std::string::npos);
    out = nlohmann::json::parse(
        formatted.substr(prefix.size(), end - prefix.size()));
}

} // namespace

TEST(SseResponseWriterStaticTest, FormatSseChunk_ContainsDataPrefix) {
    auto s = SseResponseWriter::formatSseChunk("hi", "m", "id-1");
    EXPECT_EQ(s.substr(0, 6), "data: ");
}

TEST(SseResponseWriterStaticTest, FormatSseChunk_ContainsModel) {
    auto s = SseResponseWriter::formatSseChunk("x", "my-model", "cid");
    nlohmann::json j;
    parseSseDataJson(s, j);
    EXPECT_EQ(j["model"], "my-model");
}

TEST(SseResponseWriterStaticTest, FormatSseChunk_ContainsContent) {
    auto s = SseResponseWriter::formatSseChunk("hello world", "m", "cid");
    nlohmann::json j;
    parseSseDataJson(s, j);
    ASSERT_TRUE(j["choices"].is_array());
    ASSERT_FALSE(j["choices"].empty());
    EXPECT_EQ(j["choices"][0]["delta"]["content"], "hello world");
}

TEST(SseResponseWriterStaticTest, FormatSseChunk_EndsWithDoubleNewline) {
    auto s = SseResponseWriter::formatSseChunk("a", "b", "c");
    ASSERT_GE(s.size(), 2u);
    EXPECT_EQ(s.substr(s.size() - 2), "\n\n");
}

TEST(SseResponseWriterStaticTest, FormatSseDone_IsCorrectFormat) {
    EXPECT_EQ(SseResponseWriter::formatSseDone(), "data: [DONE]\n\n");
}

TEST(SseResponseWriterStaticTest, FormatSseChunk_ValidJson) {
    auto s = SseResponseWriter::formatSseChunk("{}", "gpt-4", "chatcmpl-xyz");
    nlohmann::json j;
    EXPECT_NO_THROW(parseSseDataJson(s, j));
}

// P1-D: a mid-stream error must be serialised as a structured SSE event so the
// client can read the error code/type/message off the stream.
TEST(SseResponseWriterStaticTest, FormatSseError_ContainsErrorFields) {
    GatewayError err{503, "AEGIS-4007", "routing_error",
                     "No healthy API keys", ""};
    auto s = SseResponseWriter::formatSseError(err);
    nlohmann::json j;
    parseSseDataJson(s, j);
    ASSERT_TRUE(j.contains("error"));
    EXPECT_EQ(j["error"]["code"], "AEGIS-4007");
    EXPECT_EQ(j["error"]["type"], "routing_error");
    EXPECT_EQ(j["error"]["message"], "No healthy API keys");
}
