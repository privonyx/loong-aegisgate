#include <gtest/gtest.h>
#include "observe/quality_scorer.h"
#include "observe/quality_monitor.h"
#include "core/context.h"

using namespace aegisgate;

// P0-5: QualityScorer computes ctx.quality_score but historically never fed
// QualityMonitor, leaving admin quality trends (which read getTrends()) empty.
// Wiring a monitor must record one sample per processed response.
TEST(QualityScorerWiringTest, FeedsQualityMonitorWhenWired) {
    QualityMonitor monitor;
    QualityScorer scorer;
    scorer.setQualityMonitor(&monitor);

    RequestContext ctx;
    ctx.target_model = "gpt-4o";
    ctx.accumulated_response = "A complete, well-formed answer to the question.";
    scorer.process(ctx);

    auto trend = monitor.getTrend("gpt-4o");
    ASSERT_TRUE(trend.has_value());
    EXPECT_EQ(trend->sample_count, 1u);
    EXPECT_GT(trend->current_ema, 0.0);
}

TEST(QualityScorerWiringTest, NoMonitorIsSafe) {
    QualityScorer scorer;  // no monitor wired
    RequestContext ctx;
    ctx.target_model = "gpt-4o";
    ctx.accumulated_response = "hello world.";
    EXPECT_EQ(scorer.process(ctx), StageResult::Continue);
}

TEST(QualityScorerTest, NormalResponseScoresHigh) {
    QualityScorer scorer;
    RequestContext ctx;
    ctx.accumulated_response = "This is a well-formed response that provides "
        "a comprehensive answer to the question asked. It covers the main points "
        "and concludes properly.";
    ctx.chat_request.messages = {{"user", "What is AI?"}};
    auto result = scorer.process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_GT(ctx.quality_score, 0.5);
    EXPECT_LE(ctx.quality_score, 1.0);
}

TEST(QualityScorerTest, EmptyResponseScoresZero) {
    QualityScorer scorer;
    RequestContext ctx;
    ctx.accumulated_response = "";
    auto result = scorer.process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_DOUBLE_EQ(ctx.quality_score, 0.0);
}

TEST(QualityScorerTest, TruncatedResponseScoresLower) {
    QualityScorer scorer;
    RequestContext ctx;
    ctx.accumulated_response = "Here are the steps to solve this problem: 1. First you need to";
    auto result = scorer.process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_LT(ctx.quality_score, 0.9);
}

TEST(QualityScorerTest, HighRepetitionScoresLow) {
    QualityScorer scorer;
    RequestContext ctx;
    std::string repetitive;
    for (int i = 0; i < 50; ++i) repetitive += "abc";
    ctx.accumulated_response = repetitive;
    auto result = scorer.process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_LT(ctx.quality_score, 0.7);
}

TEST(QualityScorerTest, JsonFormatComplianceWhenExpected) {
    QualityScorer scorer;
    RequestContext ctx;
    ctx.chat_request.extra["response_format"] = "json_object";
    ctx.accumulated_response = R"({"answer": "hello world", "confidence": 0.95})";
    auto result = scorer.process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_GT(ctx.quality_score, 0.7);
}

TEST(QualityScorerTest, NonJsonWhenJsonExpectedScoresLow) {
    QualityScorer scorer;
    RequestContext ctx;
    ctx.chat_request.extra["response_format"] = "json_object";
    ctx.accumulated_response = "This is plain text, not JSON at all.";
    auto result = scorer.process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_LT(ctx.quality_score, 0.8);
}

TEST(QualityScorerTest, CustomConfigRespected) {
    QualityScorer::Config cfg;
    cfg.min_response_length = 50;
    cfg.max_response_length = 100;
    QualityScorer scorer(cfg);
    RequestContext ctx;
    ctx.accumulated_response = "Short.";
    scorer.process(ctx);
    EXPECT_LT(ctx.quality_score, 0.8);
}

TEST(QualityScorerTest, AlwaysReturnsContinue) {
    QualityScorer scorer;
    RequestContext ctx;
    ctx.accumulated_response = "x";
    EXPECT_EQ(scorer.process(ctx), StageResult::Continue);
}
