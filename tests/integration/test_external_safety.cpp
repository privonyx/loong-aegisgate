#include <gtest/gtest.h>
#include "guardrail/inbound/external_safety_stage.h"
#include "guardrail/inbound/openai_moderation.h"
#include "guardrail/inbound/perspective_api.h"
#include "core/config.h"
#include <atomic>

using namespace aegisgate;

namespace {

HttpPostFn makeOpenAIMock(bool flagged, const std::string& category = "violence") {
    return [flagged, category](
        const std::string&, const std::string&,
        const std::vector<std::pair<std::string, std::string>>&,
        int) -> std::pair<int, std::string> {
        std::string flag_str = flagged ? "true" : "false";
        return {200, R"({"results": [{"flagged": )" + flag_str + R"(, "categories": {")" +
            category + R"(": )" + flag_str + R"(}, "category_scores": {")" +
            category + R"(": )" + (flagged ? "0.99" : "0.01") + R"(}}]})"};
    };
}

HttpPostFn makePerspectiveMock(double toxicity) {
    return [toxicity](
        const std::string&, const std::string&,
        const std::vector<std::pair<std::string, std::string>>&,
        int) -> std::pair<int, std::string> {
        return {200, R"({"attributeScores": {"TOXICITY": {"summaryScore": {"value": )" +
            std::to_string(toxicity) + R"(, "type": "PROBABILITY"}}}})"};
    };
}

HttpPostFn makeErrorMock(int status = 500) {
    return [status](
        const std::string&, const std::string&,
        const std::vector<std::pair<std::string, std::string>>&,
        int) -> std::pair<int, std::string> {
        return {status, "Server Error"};
    };
}

} // namespace

class ExternalSafetyIntegrationTest : public ::testing::Test {
protected:
    RequestContext makeCtx(const std::string& text) {
        RequestContext ctx;
        ctx.request_id = "int-" + std::to_string(seq_++);
        ctx.chat_request.model = "gpt-4";
        ctx.chat_request.messages = {{"user", text}};
        ctx.start_time = std::chrono::steady_clock::now();
        return ctx;
    }

    static std::atomic<int> seq_;
};

std::atomic<int> ExternalSafetyIntegrationTest::seq_{0};

TEST_F(ExternalSafetyIntegrationTest, DualProviderBothClean) {
    ExternalSafetyStageConfig cfg;
    cfg.mode = ExternalSafetyMode::Any;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);

    OpenAIModerationConfig oai_cfg;
    oai_cfg.api_key = "test-key";
    stage.addProvider(std::make_unique<OpenAIModeration>(oai_cfg, makeOpenAIMock(false)));

    PerspectiveConfig persp_cfg;
    persp_cfg.api_key = "test-key";
    persp_cfg.threshold = 0.7;
    stage.addProvider(std::make_unique<PerspectiveApi>(persp_cfg, makePerspectiveMock(0.1)));

    auto ctx = makeCtx("What is the weather today?");
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
    EXPECT_TRUE(ctx.external_safety_checked);
    EXPECT_FALSE(ctx.external_safety_flagged);
}

TEST_F(ExternalSafetyIntegrationTest, DualProviderOneFlagged) {
    ExternalSafetyStageConfig cfg;
    cfg.mode = ExternalSafetyMode::Any;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);

    OpenAIModerationConfig oai_cfg;
    oai_cfg.api_key = "test-key";
    stage.addProvider(std::make_unique<OpenAIModeration>(oai_cfg, makeOpenAIMock(true, "hate")));

    PerspectiveConfig persp_cfg;
    persp_cfg.api_key = "test-key";
    persp_cfg.threshold = 0.7;
    stage.addProvider(std::make_unique<PerspectiveApi>(persp_cfg, makePerspectiveMock(0.1)));

    auto ctx = makeCtx("hateful content");
    EXPECT_EQ(stage.process(ctx), StageResult::Reject);
    EXPECT_TRUE(ctx.external_safety_flagged);
}

TEST_F(ExternalSafetyIntegrationTest, AllModeRequiresBothFlagged) {
    ExternalSafetyStageConfig cfg;
    cfg.mode = ExternalSafetyMode::All;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);

    OpenAIModerationConfig oai_cfg;
    oai_cfg.api_key = "test-key";
    stage.addProvider(std::make_unique<OpenAIModeration>(oai_cfg, makeOpenAIMock(true)));

    PerspectiveConfig persp_cfg;
    persp_cfg.api_key = "test-key";
    persp_cfg.threshold = 0.7;
    stage.addProvider(std::make_unique<PerspectiveApi>(persp_cfg, makePerspectiveMock(0.3)));

    auto ctx = makeCtx("test content");
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
}

TEST_F(ExternalSafetyIntegrationTest, FailOpenWithApiError) {
    ExternalSafetyStageConfig cfg;
    cfg.fail_policy = ExternalSafetyFailPolicy::Open;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);

    OpenAIModerationConfig oai_cfg;
    oai_cfg.api_key = "test-key";
    stage.addProvider(std::make_unique<OpenAIModeration>(oai_cfg, makeErrorMock(500)));

    auto ctx = makeCtx("test");
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
}

TEST_F(ExternalSafetyIntegrationTest, FailClosedWithApiError) {
    ExternalSafetyStageConfig cfg;
    cfg.fail_policy = ExternalSafetyFailPolicy::Closed;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);

    OpenAIModerationConfig oai_cfg;
    oai_cfg.api_key = "test-key";
    stage.addProvider(std::make_unique<OpenAIModeration>(oai_cfg, makeErrorMock(500)));

    auto ctx = makeCtx("test");
    EXPECT_EQ(stage.process(ctx), StageResult::Reject);
}

TEST_F(ExternalSafetyIntegrationTest, ParallelDualProvider) {
    ExternalSafetyStageConfig cfg;
    cfg.mode = ExternalSafetyMode::Any;
    cfg.async_parallel = true;
    ExternalSafetyStage stage(cfg);

    OpenAIModerationConfig oai_cfg;
    oai_cfg.api_key = "test-key";
    stage.addProvider(std::make_unique<OpenAIModeration>(oai_cfg, makeOpenAIMock(false)));

    PerspectiveConfig persp_cfg;
    persp_cfg.api_key = "test-key";
    persp_cfg.threshold = 0.7;
    stage.addProvider(std::make_unique<PerspectiveApi>(persp_cfg, makePerspectiveMock(0.95)));

    auto ctx = makeCtx("toxic content");
    EXPECT_EQ(stage.process(ctx), StageResult::Reject);
    EXPECT_EQ(stage.lastResults().size(), 2);
}

TEST_F(ExternalSafetyIntegrationTest, MajorityVotingWith3Providers) {
    ExternalSafetyStageConfig cfg;
    cfg.mode = ExternalSafetyMode::Majority;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);

    OpenAIModerationConfig oai1;
    oai1.api_key = "key1";
    stage.addProvider(std::make_unique<OpenAIModeration>(oai1, makeOpenAIMock(true)));

    OpenAIModerationConfig oai2;
    oai2.api_key = "key2";
    stage.addProvider(std::make_unique<OpenAIModeration>(oai2, makeOpenAIMock(true)));

    PerspectiveConfig persp_cfg;
    persp_cfg.api_key = "key3";
    persp_cfg.threshold = 0.7;
    stage.addProvider(std::make_unique<PerspectiveApi>(persp_cfg, makePerspectiveMock(0.1)));

    auto ctx = makeCtx("test content");
    EXPECT_EQ(stage.process(ctx), StageResult::Reject);
}

TEST_F(ExternalSafetyIntegrationTest, ConfigParsesExternalSafety) {
    Config config;
    EXPECT_FALSE(config.externalSafetyEnabled());
    EXPECT_EQ(config.externalSafetyMode(), "any");
    EXPECT_EQ(config.externalSafetyFailPolicy(), "open");
    EXPECT_TRUE(config.externalSafetyParallel());
    EXPECT_FALSE(config.openaiModerationEnabled());
    EXPECT_FALSE(config.perspectiveApiEnabled());
    EXPECT_EQ(config.openaiModerationModel(), "omni-moderation-latest");
    EXPECT_DOUBLE_EQ(config.perspectiveThreshold(), 0.7);
}

TEST_F(ExternalSafetyIntegrationTest, ContextFlagsSet) {
    ExternalSafetyStageConfig cfg;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);

    OpenAIModerationConfig oai_cfg;
    oai_cfg.api_key = "test-key";
    stage.addProvider(std::make_unique<OpenAIModeration>(oai_cfg, makeOpenAIMock(false)));

    auto ctx = makeCtx("safe content");
    EXPECT_FALSE(ctx.external_safety_checked);
    EXPECT_FALSE(ctx.external_safety_flagged);

    stage.process(ctx);
    EXPECT_TRUE(ctx.external_safety_checked);
    EXPECT_FALSE(ctx.external_safety_flagged);
}

TEST_F(ExternalSafetyIntegrationTest, MultipleMessagesCombined) {
    ExternalSafetyStageConfig cfg;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);

    std::string captured_text;
    HttpPostFn capture_fn = [&captured_text](
        const std::string&, const std::string& body,
        const std::vector<std::pair<std::string, std::string>>&,
        int) -> std::pair<int, std::string> {
        auto j = nlohmann::json::parse(body);
        if (j.contains("input")) captured_text = j["input"].get<std::string>();
        return {200, R"({"results": [{"flagged": false, "categories": {}, "category_scores": {}}]})"};
    };

    OpenAIModerationConfig oai_cfg;
    oai_cfg.api_key = "test-key";
    stage.addProvider(std::make_unique<OpenAIModeration>(oai_cfg, capture_fn));

    RequestContext ctx;
    ctx.request_id = "multi-msg";
    ctx.chat_request.messages = {
        {"system", "You are helpful"},
        {"user", "Hello there"}
    };
    stage.process(ctx);
    EXPECT_TRUE(captured_text.find("You are helpful") != std::string::npos);
    EXPECT_TRUE(captured_text.find("Hello there") != std::string::npos);
}
