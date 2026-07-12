#include <gtest/gtest.h>
#include "gateway/prompt_compressor.h"

using namespace aegisgate;

class PromptCompressorTest : public ::testing::Test {
protected:
    void SetUp() override {
        PromptCompressor::Config cfg;
        cfg.enabled = true;
        cfg.max_context_messages = 5;
        cfg.compress_whitespace = true;
        cfg.dedup_system_prompts = true;
        compressor_ = std::make_unique<PromptCompressor>(cfg);
    }

    std::unique_ptr<PromptCompressor> compressor_;
};

TEST_F(PromptCompressorTest, DisabledPassesThrough) {
    PromptCompressor::Config cfg;
    cfg.enabled = false;
    PromptCompressor disabled(cfg);

    RequestContext ctx;
    ctx.chat_request.messages = {
        {"system", "You are helpful."},
        {"user", "Hello   world"}
    };

    auto result = disabled.process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_EQ(ctx.chat_request.messages[1].content, "Hello   world");
}

TEST_F(PromptCompressorTest, CompressesWhitespace) {
    RequestContext ctx;
    ctx.chat_request.messages = {
        {"user", "Hello    world   this   is    a   test"}
    };

    compressor_->process(ctx);
    EXPECT_EQ(ctx.chat_request.messages[0].content, "Hello world this is a test");
}

TEST_F(PromptCompressorTest, CompressesNewlines) {
    RequestContext ctx;
    ctx.chat_request.messages = {
        {"user", "Line1\n\n\n\nLine2"}
    };

    compressor_->process(ctx);
    EXPECT_EQ(ctx.chat_request.messages[0].content, "Line1\nLine2");
}

TEST_F(PromptCompressorTest, CompressesMixedWhitespace) {
    RequestContext ctx;
    ctx.chat_request.messages = {
        {"user", "Hello  \t  \n  world"}
    };

    compressor_->process(ctx);
    auto& content = ctx.chat_request.messages[0].content;
    EXPECT_TRUE(content.find("  ") == std::string::npos);
    EXPECT_TRUE(content.find("\t") == std::string::npos);
}

TEST_F(PromptCompressorTest, DedupSystemPrompts) {
    RequestContext ctx;
    ctx.chat_request.messages = {
        {"system", "You are a helpful assistant."},
        {"system", "You are a helpful assistant."},
        {"user", "Hello"}
    };

    compressor_->process(ctx);
    int system_count = 0;
    for (const auto& msg : ctx.chat_request.messages) {
        if (msg.role == "system") system_count++;
    }
    EXPECT_EQ(system_count, 1);
    EXPECT_EQ(ctx.chat_request.messages.size(), 2u);
}

TEST_F(PromptCompressorTest, KeepsDifferentSystemPrompts) {
    RequestContext ctx;
    ctx.chat_request.messages = {
        {"system", "You are a helpful assistant."},
        {"system", "Respond in Chinese."},
        {"user", "Hello"}
    };

    compressor_->process(ctx);
    int system_count = 0;
    for (const auto& msg : ctx.chat_request.messages) {
        if (msg.role == "system") system_count++;
    }
    EXPECT_EQ(system_count, 2);
}

TEST_F(PromptCompressorTest, TruncatesOldMessages) {
    RequestContext ctx;
    ctx.chat_request.messages = {
        {"system", "System prompt"},
        {"user", "Message 1"},
        {"assistant", "Reply 1"},
        {"user", "Message 2"},
        {"assistant", "Reply 2"},
        {"user", "Message 3"},
        {"assistant", "Reply 3"},
        {"user", "Message 4"},
    };

    compressor_->process(ctx);
    EXPECT_LE(ctx.chat_request.messages.size(), 5u);
    EXPECT_EQ(ctx.chat_request.messages[0].role, "system");
    EXPECT_EQ(ctx.chat_request.messages.back().content, "Message 4");
}

TEST_F(PromptCompressorTest, PreservesSystemPromptDuringTruncation) {
    RequestContext ctx;
    ctx.chat_request.messages = {
        {"system", "Important system prompt"},
        {"user", "Old message 1"},
        {"assistant", "Old reply 1"},
        {"user", "Old message 2"},
        {"assistant", "Old reply 2"},
        {"user", "Recent message"},
        {"assistant", "Recent reply"},
        {"user", "Latest question"},
    };

    compressor_->process(ctx);

    bool has_system = false;
    for (const auto& msg : ctx.chat_request.messages) {
        if (msg.role == "system" && msg.content == "Important system prompt") {
            has_system = true;
        }
    }
    EXPECT_TRUE(has_system);
}

TEST_F(PromptCompressorTest, RecordsTokensSaved) {
    RequestContext ctx;
    ctx.chat_request.messages = {
        {"user", "Hello     world     with     lots     of     spaces"}
    };

    compressor_->process(ctx);
    EXPECT_GE(ctx.tokens_saved_compression, 0);
    EXPECT_GT(ctx.tokens_estimated, 0);
}

TEST_F(PromptCompressorTest, EmptyMessagesNoOp) {
    RequestContext ctx;
    auto result = compressor_->process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
}

TEST_F(PromptCompressorTest, NoTruncationWhenUnderLimit) {
    RequestContext ctx;
    ctx.chat_request.messages = {
        {"user", "Question 1"},
        {"assistant", "Answer 1"},
        {"user", "Question 2"},
    };

    size_t original_size = ctx.chat_request.messages.size();
    compressor_->process(ctx);
    EXPECT_EQ(ctx.chat_request.messages.size(), original_size);
}

TEST_F(PromptCompressorTest, TrailingWhitespaceRemoved) {
    RequestContext ctx;
    ctx.chat_request.messages = {
        {"user", "Hello world   "}
    };

    compressor_->process(ctx);
    EXPECT_EQ(ctx.chat_request.messages[0].content, "Hello world");
}

TEST_F(PromptCompressorTest, UnlimitedMessagesWhenZero) {
    PromptCompressor::Config cfg;
    cfg.enabled = true;
    cfg.max_context_messages = 0;
    cfg.compress_whitespace = false;
    cfg.dedup_system_prompts = false;
    PromptCompressor unlimited(cfg);

    RequestContext ctx;
    for (int i = 0; i < 100; i++) {
        ctx.chat_request.messages.push_back({"user", "Message " + std::to_string(i)});
    }

    unlimited.process(ctx);
    EXPECT_EQ(ctx.chat_request.messages.size(), 100u);
}
