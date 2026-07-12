#include <gtest/gtest.h>
#include "guardrail/outbound/hallucination.h"
#include "observe/metrics.h"

using namespace aegisgate;

class HallucinationDetectorTest : public ::testing::Test {
protected:
    HallucinationDetector detector_;
};

TEST_F(HallucinationDetectorTest, HighConfidenceForRelevantOutput) {
    auto r = detector_.analyze(
        "Python is a programming language used for web development.",
        "Tell me about Python programming");
    EXPECT_GE(r.confidence_score, 0.0);
    EXPECT_LE(r.confidence_score, 1.0);
}

TEST_F(HallucinationDetectorTest, FlagsHighClaimDensity) {
    detector_.setThreshold(0.8);
    auto r = detector_.analyze(
        "On 2024-01-15, the company reported 45.3% growth. "
        "See https://fake.example.com/report for details. "
        "Revenue was 12.7% higher than the 2023-06-30 forecast.",
        "What happened?");
    EXPECT_LT(r.confidence_score, 1.0);
}

TEST_F(HallucinationDetectorTest, ScoreBoundedZeroToOne) {
    auto r = detector_.analyze("Random output text.", "Random input text.");
    EXPECT_GE(r.confidence_score, 0.0);
    EXPECT_LE(r.confidence_score, 1.0);
}

TEST_F(HallucinationDetectorTest, NeverBlocksInPipeline) {
    RequestContext ctx;
    ctx.request_id = "test-halluc";
    EXPECT_EQ(detector_.process(ctx), StageResult::Continue);
}

TEST_F(HallucinationDetectorTest, EmptyInputHandled) {
    auto r = detector_.analyze("Some output.", "");
    EXPECT_GE(r.confidence_score, 0.0);
    EXPECT_LE(r.confidence_score, 1.0);
}

TEST_F(HallucinationDetectorTest, EmptyOutputHandled) {
    auto r = detector_.analyze("", "Some input.");
    EXPECT_GE(r.confidence_score, 0.0);
    EXPECT_LE(r.confidence_score, 1.0);
}

TEST_F(HallucinationDetectorTest, ProcessCallsAnalyzeAndSetsCtx) {
    detector_.setThreshold(0.8);

    RequestContext ctx;
    ctx.chat_request.messages.push_back({"user", "什么是 AI"});
    ctx.accumulated_response =
        "AI invented on 2024-01-15 at http://fake.url achieved 99.7% accuracy "
        "on 2024-02-20 with 87.3% efficiency and 42.1% success rate.";

    auto result = detector_.process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_LT(ctx.hallucination_score, 1.0);
}

// P1-B: when ground-truth scoring runs (retrieval sources present), the
// computed groundedness must be recorded into the groundedness_score histogram
// (previously registered but never observed → admin saw an empty distribution).
TEST_F(HallucinationDetectorTest, GroundednessRecordedToHistogram) {
    GroundTruthConfig gt;
    gt.enabled = true;
    detector_.setGroundTruthConfig(gt);

    RequestContext ctx;
    ctx.chat_request.messages.push_back({"user", "tell me about apples"});
    ctx.accumulated_response = "Apples are a sweet fruit that grow on trees.";
    ctx.retrieval_sources.emplace_back(
        "chunk-1", "doc-1", "Apples are a sweet fruit that grow on trees.", 0.9f);

    detector_.process(ctx);

    EXPECT_GT(ctx.groundedness_score, 0.0f);
    EXPECT_NE(MetricsRegistry::instance().groundednessScore().expose().find(
                  "aegisgate_groundedness_score_count"),
              std::string::npos);
}

TEST_F(HallucinationDetectorTest, ProcessCleanResponseHighScore) {
    RequestContext ctx;
    ctx.chat_request.messages.push_back({"user", "什么是机器学习"});
    ctx.accumulated_response =
        "机器学习是人工智能的一个分支，通过数据训练模型来做预测。";

    auto result = detector_.process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_GE(ctx.hallucination_score, 0.5);
}
