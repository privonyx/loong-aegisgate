#include "cache/summarizer_factory.h"
#include "guardrail/inbound/pii_filter.h"
#include <gtest/gtest.h>

using namespace aegisgate;

TEST(SummarizerFactoryTest, DefaultIsRuleBased) {
    SummarizerConfig cfg;
    auto s = makeSummarizer(cfg, nullptr);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->name(), "RuleBased");
}

TEST(SummarizerFactoryTest, OnnxTypeWithoutModelPathFallsBackToRule) {
    SummarizerConfig cfg;
    cfg.type = "onnx";
    cfg.onnx_model_path = "";
    auto s = makeSummarizer(cfg, nullptr);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->name(), "RuleBased");
}

TEST(SummarizerFactoryTest, OnnxLoadFailsFallsBackToRule) {
    SummarizerConfig cfg;
    cfg.type = "onnx";
    cfg.onnx_model_path = "/nonexistent/never-loads.onnx";
    auto s = makeSummarizer(cfg, nullptr);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->name(), "RuleBased");
}

TEST(SummarizerFactoryTest, PiiFilterPropagates_SR4) {
    PIIFilter pii;
    SummarizerConfig cfg;
    auto s = makeSummarizer(cfg, &pii);
    ASSERT_NE(s, nullptr);
    auto out = s->summarize({{"user", "contact 13812345678 today"}});
    EXPECT_EQ(out.find("13812345678"), std::string::npos);
}

TEST(SummarizerFactoryTest, UnknownTypeFallsBackToRule) {
    SummarizerConfig cfg;
    cfg.type = "magic-summarizer";
    auto s = makeSummarizer(cfg, nullptr);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->name(), "RuleBased");
}
