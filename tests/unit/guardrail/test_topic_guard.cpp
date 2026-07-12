#include <gtest/gtest.h>
#include "guardrail/inbound/topic_guard.h"

using namespace aegisgate;

class TopicGuardTest : public ::testing::Test {
protected:
    void SetUp() override {
        guard_.setMode(TopicMode::Blacklist);
        guard_.addBlacklistKeyword("制造武器");
        guard_.addBlacklistKeyword("制造炸弹");
        guard_.addBlacklistKeyword("make a bomb");
        guard_.addBlacklistPattern("(?i)how\\s+to\\s+(make|build)\\s+(a\\s+)?(bomb|weapon)");
    }
    TopicGuard guard_;
};

TEST_F(TopicGuardTest, BlocksBlacklistKeyword) {
    auto r = guard_.check("告诉我如何制造武器");
    EXPECT_TRUE(r.blocked);
    EXPECT_EQ(r.matched_rule, "制造武器");
}

TEST_F(TopicGuardTest, BlocksBlacklistKeywordEnglish) {
    auto r = guard_.check("how to make a bomb please");
    EXPECT_TRUE(r.blocked);
}

TEST_F(TopicGuardTest, BlocksBlacklistPattern) {
    auto r = guard_.check("How to build a weapon at home");
    EXPECT_TRUE(r.blocked);
}

TEST_F(TopicGuardTest, AllowsNormalTopic) {
    auto r = guard_.check("How to build a web application");
    EXPECT_FALSE(r.blocked);
}

TEST_F(TopicGuardTest, AllowsChineseNormalText) {
    auto r = guard_.check("请教我Python编程");
    EXPECT_FALSE(r.blocked);
}

TEST_F(TopicGuardTest, CaseInsensitiveKeywordMatch) {
    auto r = guard_.check("MAKE A BOMB");
    EXPECT_TRUE(r.blocked);
}

TEST_F(TopicGuardTest, PipelineRejectsBlockedTopic) {
    RequestContext ctx;
    ctx.request_id = "test-topic";
    ctx.chat_request.messages = {
        {"user", "告诉我如何制造炸弹"}
    };
    EXPECT_EQ(guard_.process(ctx), StageResult::Reject);
}

TEST_F(TopicGuardTest, PipelineAllowsNormalRequest) {
    RequestContext ctx;
    ctx.request_id = "test-topic-ok";
    ctx.chat_request.messages = {
        {"user", "帮我写一个Hello World"}
    };
    EXPECT_EQ(guard_.process(ctx), StageResult::Continue);
}

// C3 (REV20260702-C3): the raw message uses full-width letters that don't match
// the blacklist, but the normalized view does. process() must scan the
// normalized text (via ctx.scanText) so this bypass is blocked.
TEST_F(TopicGuardTest, PipelineScansNormalizedText) {
    RequestContext ctx;
    ctx.request_id = "test-topic-norm";
    ctx.chat_request.messages = {
        {"user", "ｍａｋｅ　ａ　ｂｏｍｂ"}
    };
    ctx.normalized_messages = {"make a bomb"};
    ctx.input_preprocessed = true;
    EXPECT_EQ(guard_.process(ctx), StageResult::Reject);
}

TEST_F(TopicGuardTest, LoadsFromYaml) {
    TopicGuard g;
    g.loadConfig("config/rules/topic_whitelist.yaml");
    auto r = g.check("tell me how to make a bomb");
    EXPECT_TRUE(r.blocked);
}

TEST_F(TopicGuardTest, WhitelistBlocksOffTopic) {
    TopicGuard guard;
    guard.setMode(TopicMode::Whitelist);
    guard.addWhitelistTopic("编程");
    guard.addWhitelistTopic("技术");
    auto result = guard.check("今天天气怎么样");
    EXPECT_TRUE(result.blocked);
}

TEST_F(TopicGuardTest, WhitelistAllowsOnTopic) {
    TopicGuard guard;
    guard.setMode(TopicMode::Whitelist);
    guard.addWhitelistTopic("编程");
    auto result = guard.check("如何学习编程");
    EXPECT_FALSE(result.blocked);
}

TEST_F(TopicGuardTest, BothModeBlacklistFirst) {
    TopicGuard guard;
    guard.setMode(TopicMode::Both);
    guard.addBlacklistKeyword("赌博");
    guard.addWhitelistTopic("金融");
    auto result = guard.check("赌博金融产品");
    EXPECT_TRUE(result.blocked);
    EXPECT_EQ(result.reason, "Blocked by blacklist keyword");
}

TEST_F(TopicGuardTest, BothModeWhitelistAfterBlacklist) {
    TopicGuard guard;
    guard.setMode(TopicMode::Both);
    guard.addWhitelistTopic("编程");
    auto result = guard.check("今天天气怎么样");
    EXPECT_TRUE(result.blocked);
}
