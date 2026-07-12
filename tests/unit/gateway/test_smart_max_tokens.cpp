#include <gtest/gtest.h>
#include "gateway/smart_max_tokens.h"
#include "gateway/connector/registry.h"
#include "gateway/connector/base.h"

using namespace aegisgate;

class SmartMaxTokensTest : public ::testing::Test {
protected:
    void SetUp() override {
        SmartMaxTokens::Config cfg;
        cfg.enabled = true;
        cfg.default_max_output = 2048;
        cfg.max_output_ratio = 2.0;
        cfg.min_output_tokens = 100;
        stage_ = std::make_unique<SmartMaxTokens>(cfg);

        ModelInfo model;
        model.id = "gpt-4o-mini";
        model.provider = "openai";
        model.max_context_tokens = 128000;
        registry_.registerModelInfo(model);

        ModelInfo small_model;
        small_model.id = "small-model";
        small_model.provider = "local";
        small_model.max_context_tokens = 4096;
        registry_.registerModelInfo(small_model);

        stage_->setConnectorRegistry(&registry_);
    }

    std::unique_ptr<SmartMaxTokens> stage_;
    ConnectorRegistry registry_;
};

TEST_F(SmartMaxTokensTest, SkipsWhenMaxTokensAlreadySet) {
    RequestContext ctx;
    ctx.chat_request.max_tokens = 500;
    ctx.chat_request.messages = {{"user", "Hello"}};

    auto result = stage_->process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_EQ(ctx.chat_request.max_tokens.value(), 500);
}

TEST_F(SmartMaxTokensTest, SetsMaxTokensForShortInput) {
    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "Hi!"}};
    ctx.chat_request.model = "gpt-4o-mini";

    auto result = stage_->process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_TRUE(ctx.chat_request.max_tokens.has_value());
    EXPECT_GT(ctx.chat_request.max_tokens.value(), 0);
    EXPECT_GE(ctx.chat_request.max_tokens.value(), 100);
}

TEST_F(SmartMaxTokensTest, RespectDefaultMaxOutput) {
    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "Hi!"}};
    ctx.chat_request.model = "gpt-4o-mini";

    stage_->process(ctx);
    EXPECT_LE(ctx.chat_request.max_tokens.value(), 2048);
}

TEST_F(SmartMaxTokensTest, RespectsMinOutputTokens) {
    SmartMaxTokens::Config cfg;
    cfg.enabled = true;
    cfg.default_max_output = 50;
    cfg.max_output_ratio = 0.1;
    cfg.min_output_tokens = 100;
    SmartMaxTokens conservative(cfg);
    conservative.setConnectorRegistry(&registry_);

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "Hi!"}};
    ctx.chat_request.model = "gpt-4o-mini";

    conservative.process(ctx);
    EXPECT_GE(ctx.chat_request.max_tokens.value(), 100);
}

TEST_F(SmartMaxTokensTest, DisabledPassesThrough) {
    SmartMaxTokens::Config cfg;
    cfg.enabled = false;
    SmartMaxTokens disabled(cfg);

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "Hello world"}};

    auto result = disabled.process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_FALSE(ctx.chat_request.max_tokens.has_value());
}

TEST_F(SmartMaxTokensTest, UsesEstimatedTokensFromContext) {
    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "Hello"}};
    ctx.chat_request.model = "gpt-4o-mini";
    ctx.tokens_estimated = 50;

    stage_->process(ctx);
    EXPECT_TRUE(ctx.chat_request.max_tokens.has_value());
    int max_t = ctx.chat_request.max_tokens.value();
    EXPECT_LE(max_t, 2048);
    EXPECT_LE(max_t, 50 * 2);
}

TEST_F(SmartMaxTokensTest, FallsBackToTargetModel) {
    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "Hello"}};
    ctx.target_model = "small-model";

    stage_->process(ctx);
    EXPECT_TRUE(ctx.chat_request.max_tokens.has_value());
}

TEST_F(SmartMaxTokensTest, WorksWithoutRegistry) {
    SmartMaxTokens::Config cfg;
    cfg.enabled = true;
    cfg.default_max_output = 2048;
    cfg.max_output_ratio = 2.0;
    cfg.min_output_tokens = 100;
    SmartMaxTokens no_registry(cfg);

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "Hello world"}};

    auto result = no_registry.process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_TRUE(ctx.chat_request.max_tokens.has_value());
}

TEST_F(SmartMaxTokensTest, LargeInputGetsReasonableOutput) {
    RequestContext ctx;
    ctx.chat_request.model = "small-model";
    std::string long_text(10000, 'a');
    ctx.chat_request.messages = {{"user", long_text}};

    stage_->process(ctx);
    EXPECT_TRUE(ctx.chat_request.max_tokens.has_value());
    EXPECT_GE(ctx.chat_request.max_tokens.value(), 100);
}
