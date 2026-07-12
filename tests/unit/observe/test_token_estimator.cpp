#include <gtest/gtest.h>
#include "observe/token_estimator.h"

using namespace aegisgate;

TEST(TokenEstimatorTest, EmptyStringReturnsZero) {
    EXPECT_EQ(TokenEstimator::estimateTokens(""), 0);
}

TEST(TokenEstimatorTest, EnglishTextEstimate) {
    std::string text = "Hello world, this is a test sentence.";
    int tokens = TokenEstimator::estimateTokens(text);
    EXPECT_GT(tokens, 0);
    EXPECT_LE(tokens, static_cast<int>(text.size()));
}

TEST(TokenEstimatorTest, ChineseTextEstimate) {
    std::string text = "你好世界，这是一个测试句子。";
    int tokens = TokenEstimator::estimateTokens(text);
    EXPECT_GT(tokens, 0);
}

TEST(TokenEstimatorTest, ChineseRatioForPureEnglish) {
    double ratio = TokenEstimator::chineseRatio("Hello world");
    EXPECT_NEAR(ratio, 0.0, 0.01);
}

TEST(TokenEstimatorTest, ChineseRatioForPureChinese) {
    double ratio = TokenEstimator::chineseRatio("你好世界");
    EXPECT_GT(ratio, 0.9);
}

TEST(TokenEstimatorTest, ChineseRatioForMixedText) {
    double ratio = TokenEstimator::chineseRatio("Hello你好");
    EXPECT_GT(ratio, 0.1);
    EXPECT_LT(ratio, 0.9);
}

TEST(TokenEstimatorTest, EstimateMessagesIncludesOverhead) {
    std::vector<Message> messages = {
        {"system", "You are a helpful assistant."},
        {"user", "Hello!"}
    };
    int tokens = TokenEstimator::estimateMessages(messages);
    int content_only = TokenEstimator::estimateTokens("You are a helpful assistant.")
                     + TokenEstimator::estimateTokens("Hello!");
    EXPECT_GT(tokens, content_only);
}

TEST(TokenEstimatorTest, EmptyMessagesReturnsMinimal) {
    std::vector<Message> messages;
    int tokens = TokenEstimator::estimateMessages(messages);
    EXPECT_EQ(tokens, 3);
}

TEST(TokenEstimatorTest, LongTextProducesReasonableEstimate) {
    std::string text(10000, 'a');
    int tokens = TokenEstimator::estimateTokens(text);
    EXPECT_GE(tokens, 2000);
    EXPECT_LE(tokens, 5000);
}

TEST(TokenEstimatorTest, SingleCharMinimumOneToken) {
    EXPECT_GE(TokenEstimator::estimateTokens("a"), 1);
}
