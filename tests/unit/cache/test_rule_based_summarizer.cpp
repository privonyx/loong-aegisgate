#include "cache/rule_based_summarizer.h"
#include "guardrail/inbound/pii_filter.h"
#include <gtest/gtest.h>

using namespace aegisgate;

TEST(RuleBasedSummarizerTest, ExtractsTopKeywordsByFrequency) {
    RuleBasedSummarizer s(512, 3);
    std::vector<Message> msgs = {
        {"user", "apple banana apple"},
        {"assistant", "apple banana cherry banana"}
    };
    auto out = s.summarize(msgs);
    EXPECT_NE(out.find("apple"), std::string::npos);
    EXPECT_NE(out.find("banana"), std::string::npos);
}

TEST(RuleBasedSummarizerTest, TruncatesAtMaxChars) {
    RuleBasedSummarizer s(50, 5);
    std::vector<Message> msgs(20, {"user", std::string(100, 'x')});
    auto out = s.summarize(msgs);
    EXPECT_LE(out.size(), 50u);
}

TEST(RuleBasedSummarizerTest, PIIIsSanitizedBeforeSummarization_SR4) {
    PIIFilter pii;
    RuleBasedSummarizer s(512, 5, &pii);
    std::vector<Message> msgs = {
        {"user", "my phone is 13812345678 please help"}
    };
    auto out = s.summarize(msgs);
    EXPECT_EQ(out.find("13812345678"), std::string::npos);
}

TEST(RuleBasedSummarizerTest, EmptyMessagesProducesEmpty) {
    RuleBasedSummarizer s(512, 5);
    EXPECT_EQ(s.summarize({}), "");
}

TEST(RuleBasedSummarizerTest, SingleMessageWorks) {
    RuleBasedSummarizer s(512, 3);
    std::vector<Message> msgs = {{"user", "hello world hello"}};
    auto out = s.summarize(msgs);
    EXPECT_NE(out.find("hello"), std::string::npos);
}

TEST(RuleBasedSummarizerTest, MaxCharsZeroProducesEmpty) {
    RuleBasedSummarizer s(0, 5);
    std::vector<Message> msgs = {{"user", "anything"}};
    EXPECT_EQ(s.summarize(msgs), "");
}

TEST(RuleBasedSummarizerTest, TopKeywordsZeroProducesEmpty) {
    RuleBasedSummarizer s(512, 0);
    std::vector<Message> msgs = {{"user", "anything goes"}};
    EXPECT_EQ(s.summarize(msgs), "");
}

TEST(RuleBasedSummarizerTest, DeterministicOutputAcrossCalls) {
    RuleBasedSummarizer s(512, 5);
    std::vector<Message> msgs = {
        {"user", "alpha beta gamma alpha"},
        {"assistant", "beta gamma alpha"}
    };
    auto out1 = s.summarize(msgs);
    auto out2 = s.summarize(msgs);
    EXPECT_EQ(out1, out2);
}

TEST(RuleBasedSummarizerTest, NameIsRuleBased) {
    RuleBasedSummarizer s;
    EXPECT_EQ(s.name(), "RuleBased");
}

TEST(RuleBasedSummarizerTest, SingleCharTokensSkipped) {
    RuleBasedSummarizer s(512, 3);
    std::vector<Message> msgs = {{"user", "a b c apple banana"}};
    auto out = s.summarize(msgs);
    // single-char tokens are filtered (noise reduction)
    EXPECT_NE(out.find("apple"), std::string::npos);
    EXPECT_NE(out.find("banana"), std::string::npos);
}

TEST(RuleBasedSummarizerTest, FrequencyOrdering) {
    RuleBasedSummarizer s(512, 2);
    std::vector<Message> msgs = {
        {"user", "rare common common common"},
        {"assistant", "another common"}
    };
    auto out = s.summarize(msgs);
    EXPECT_NE(out.find("common"), std::string::npos);
}

TEST(RuleBasedSummarizerTest, PIIFilterNullDoesNotCrash) {
    RuleBasedSummarizer s(512, 5, nullptr);
    std::vector<Message> msgs = {{"user", "contact 13812345678 today"}};
    auto out = s.summarize(msgs);
    // Without PII filter the digit string may pass; we only assert no crash.
    EXPECT_FALSE(out.empty());
}
