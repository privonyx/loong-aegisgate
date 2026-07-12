#include <gtest/gtest.h>
#include "guardrail/inbound/pii_filter.h"

using namespace aegisgate;

class PIIFilterTest : public ::testing::Test {
protected:
    PIIFilter filter_;
};

TEST_F(PIIFilterTest, MasksChinesePhoneNumbers) {
    EXPECT_EQ(filter_.mask("我的手机号是 13812345678"),
              "我的手机号是 [PHONE]");
}

TEST_F(PIIFilterTest, MasksMultiplePhoneNumbers) {
    EXPECT_EQ(filter_.mask("联系 13900001111 或 15688889999"),
              "联系 [PHONE] 或 [PHONE]");
}

TEST_F(PIIFilterTest, MasksIDCardNumbers) {
    EXPECT_EQ(filter_.mask("身份证 110101199001011234"),
              "身份证 [ID_CARD]");
}

TEST_F(PIIFilterTest, MasksEmailAddresses) {
    EXPECT_EQ(filter_.mask("邮箱 user@example.com"),
              "邮箱 [EMAIL]");
}

TEST_F(PIIFilterTest, MasksAPIKeys) {
    EXPECT_EQ(filter_.mask("key is sk-abcdef1234567890abcdef"),
              "key is [API_KEY]");
}

TEST_F(PIIFilterTest, MasksJWT) {
    std::string jwt = "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.abc123def456";
    EXPECT_EQ(filter_.mask("token: " + jwt), "token: [JWT]");
}

TEST_F(PIIFilterTest, PreservesNormalText) {
    EXPECT_EQ(filter_.mask("Hello, how are you?"), "Hello, how are you?");
}

TEST_F(PIIFilterTest, PreservesShortNumbers) {
    EXPECT_EQ(filter_.mask("订单号 12345"), "订单号 12345");
}

TEST_F(PIIFilterTest, PipelineMasksMessages) {
    RequestContext ctx;
    ctx.request_id = "test-pii";
    ctx.chat_request.messages = {
        {"user", "我的手机号是 13812345678"}
    };
    EXPECT_EQ(filter_.process(ctx), StageResult::Continue);
    EXPECT_EQ(ctx.chat_request.messages[0].content, "我的手机号是 [PHONE]");
}

TEST_F(PIIFilterTest, PipelineSkipsSystemMessages) {
    RequestContext ctx;
    ctx.request_id = "test-pii-sys";
    ctx.chat_request.messages = {
        {"system", "key is sk-abcdef1234567890abcdef"},
        {"user", "Hello"}
    };
    EXPECT_EQ(filter_.process(ctx), StageResult::Continue);
    EXPECT_EQ(ctx.chat_request.messages[0].content,
              "key is sk-abcdef1234567890abcdef");
}

// P1-C: when the input was preprocessed and a message was normalized,
// obfuscated PII that only surfaces after normalization must be redacted, and
// the outgoing content replaced with the masked normalized form so the raw
// obfuscated PII never reaches the upstream.
TEST_F(PIIFilterTest, ConsumesNormalizedToCatchObfuscatedPii) {
    RequestContext ctx;
    ctx.request_id = "test-pii-norm";
    // raw uses full-width digits → the ASCII phone regex does NOT match it
    ctx.chat_request.messages = {
        {"user", "我的手机号是 \xEF\xBC\x91\xEF\xBC\x93\xEF\xBC\x98"
                  "\xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x93\xEF\xBC\x94"
                  "\xEF\xBC\x95\xEF\xBC\x96\xEF\xBC\x97\xEF\xBC\x98"}
    };
    ctx.input_preprocessed = true;
    ctx.normalized_messages = {"我的手机号是 13812345678"};

    EXPECT_EQ(filter_.process(ctx), StageResult::Continue);
    EXPECT_EQ(ctx.chat_request.messages[0].content, "我的手机号是 [PHONE]");
}

// P1-C: normalization without PII must NOT rewrite the upstream payload — we
// only adopt the normalized form when it carries redactable PII the raw missed.
TEST_F(PIIFilterTest, DoesNotRewriteWhenNormalizedHasNoPii) {
    RequestContext ctx;
    ctx.request_id = "test-pii-norm-clean";
    const std::string raw = "\xEF\xBD\x88\xEF\xBD\x85\xEF\xBD\x8C"
                            "\xEF\xBD\x8C\xEF\xBD\x8F";  // full-width "hello"
    ctx.chat_request.messages = {{"user", raw}};
    ctx.input_preprocessed = true;
    ctx.normalized_messages = {"hello"};

    EXPECT_EQ(filter_.process(ctx), StageResult::Continue);
    EXPECT_EQ(ctx.chat_request.messages[0].content, raw);
}

// Test that processChunk filters PII and writes to chunk_output
TEST_F(PIIFilterTest, ProcessChunkWritesBackToCtx) {
    RequestContext ctx;
    auto result = filter_.processChunk(ctx, "My email is test@example.com");
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_FALSE(ctx.chunk_output.empty());
    // Email should be redacted
    EXPECT_TRUE(ctx.chunk_output.find("test@example.com") == std::string::npos);
    EXPECT_TRUE(ctx.chunk_output.find("[EMAIL]") != std::string::npos);
}

TEST_F(PIIFilterTest, ProcessChunkNoChange) {
    RequestContext ctx;
    auto result = filter_.processChunk(ctx, "Normal text without PII");
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_EQ(ctx.chunk_output, "Normal text without PII");
}
