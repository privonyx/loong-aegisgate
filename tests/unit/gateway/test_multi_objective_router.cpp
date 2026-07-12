// Phase 11.2 TASK-20260521-03 — MultiObjectiveRouter unit tests.
//
// MultiObjectiveRouter is the Pareto-front cost/quality/latency router that
// honors per-strategy weight overrides installed by RoutingStrategyCatalog
// + BanditAutonomyApplier. It does NOT do online learning — that is the
// BanditRouter decorator's job. The router exposes reportOutcome() for
// callers (RolloutController feedback path) but uses safe defaults
// (success_rate=1.0, avg_latency=100ms) when stats are absent.

#include <gtest/gtest.h>
#include "gateway/multi_objective_router.h"
#include "gateway/connector/base.h"
#include "gateway/connector/registry.h"

using namespace aegisgate;

namespace {

class StubConnector : public ModelConnector {
public:
    StubConnector(std::string name, std::vector<std::string> models)
        : name_(std::move(name)), models_(std::move(models)) {}
    ChatResponse complete(const ChatRequest&) override { return {}; }
    void streamComplete(const ChatRequest&,
                        std::function<void(const StreamDelta&)>,
                        std::function<void(const TokenUsage&)>,
                        std::function<void(const GatewayError&)>) override {}
    std::string provider() const override { return name_; }
    bool supportsModel(const std::string& model) const override {
        for (const auto& m : models_) if (m == model) return true;
        return false;
    }
    ModelCapabilities capabilities(const std::string&) const override { return {}; }
protected:
    nlohmann::json buildRequestBody(const ChatRequest&) override { return {}; }
    ChatResponse parseResponse(const nlohmann::json&) override { return {}; }
    StreamDelta parseStreamChunk(std::string_view) override { return {}; }
    TokenUsage parseUsage(const nlohmann::json&) override { return {}; }
    std::string name_;
    std::vector<std::string> models_;
};

}  // namespace

class MultiObjectiveRouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_.registerConnector(
            std::make_unique<StubConnector>("openai",
                std::vector<std::string>{"gpt-4o", "gpt-4o-mini"}));
        registry_.registerConnector(
            std::make_unique<StubConnector>("local",
                std::vector<std::string>{"qwen2.5:7b"}));

        ModelInfo gpt4o;
        gpt4o.id = "gpt-4o";
        gpt4o.provider = "openai";
        gpt4o.cost_per_1k_input = 0.005;
        gpt4o.cost_per_1k_output = 0.015;
        gpt4o.max_context_tokens = 128000;
        gpt4o.tags = {"high-quality"};

        ModelInfo gpt4omini;
        gpt4omini.id = "gpt-4o-mini";
        gpt4omini.provider = "openai";
        gpt4omini.cost_per_1k_input = 0.00015;
        gpt4omini.cost_per_1k_output = 0.0006;
        gpt4omini.max_context_tokens = 128000;
        gpt4omini.tags = {"fast"};

        ModelInfo qwen;
        qwen.id = "qwen2.5:7b";
        qwen.provider = "local";
        qwen.cost_per_1k_input = 0;
        qwen.cost_per_1k_output = 0;
        qwen.max_context_tokens = 32000;
        qwen.tags = {"free"};

        registry_.registerModelInfo(gpt4o);
        registry_.registerModelInfo(gpt4omini);
        registry_.registerModelInfo(qwen);
        registry_.setDefaultModel("gpt-4o-mini");
    }
    ConnectorRegistry registry_;
};

// T1: explicit model in request body bypasses Pareto scoring entirely.
TEST_F(MultiObjectiveRouterTest, ExplicitModelRespected) {
    MultiObjectiveRouter router;
    RequestContext ctx;
    ctx.chat_request.model = "gpt-4o";
    EXPECT_EQ(router.selectModel(ctx, registry_), "gpt-4o");
}

// T2: with default weights (cost 0.4 / quality 0.35 / latency 0.25) and
// cold-start (success_rate=1.0 latency=100ms for all), the qwen model
// wins on cost alone because cost has the largest weight tying everything
// else.
TEST_F(MultiObjectiveRouterTest, DefaultStrategySelectsCheapestOnColdStart) {
    MultiObjectiveRouter router;
    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "hi"}};
    EXPECT_EQ(router.selectModel(ctx, registry_), "qwen2.5:7b");
}

// T3: setActiveStrategy with extreme cost weight still picks cheapest;
// this exercises the override path (different from default weights).
TEST_F(MultiObjectiveRouterTest, CostFirstStrategyOverridesWeights) {
    MultiObjectiveRouter router;
    router.setActiveStrategy("cost-first",
                              MultiObjectiveRouter::Weights{0.7, 0.2, 0.1});
    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "hi"}};
    EXPECT_EQ(router.selectModel(ctx, registry_), "qwen2.5:7b");
}

// T4: quality-first strategy + reported outcomes: the model with the
// highest success_rate dominates when quality weight is high and cost
// weight is low.
TEST_F(MultiObjectiveRouterTest, QualityFirstStrategyHonorsOutcomes) {
    MultiObjectiveRouter router;
    router.setActiveStrategy("quality-first",
                              MultiObjectiveRouter::Weights{0.0, 1.0, 0.0});
    // gpt-4o always succeeds; qwen always fails.
    for (int i = 0; i < 20; ++i) {
        router.reportOutcome("gpt-4o", 50.0, true);
        router.reportOutcome("qwen2.5:7b", 500.0, false);
        router.reportOutcome("gpt-4o-mini", 100.0, true);
    }
    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "hi"}};
    auto model = router.selectModel(ctx, registry_);
    EXPECT_NE(model, "qwen2.5:7b")
        << "qwen failed 100% — must not be selected under quality-first";
}

// T5: setActiveStrategy / getActiveStrategy / clearActiveStrategy roundtrip.
TEST_F(MultiObjectiveRouterTest, ActiveStrategyAccessor) {
    MultiObjectiveRouter router;
    EXPECT_FALSE(router.getActiveStrategy().has_value());

    router.setActiveStrategy("quality-first",
                              MultiObjectiveRouter::Weights{0.2, 0.6, 0.2});
    ASSERT_TRUE(router.getActiveStrategy().has_value());
    EXPECT_EQ(*router.getActiveStrategy(), "quality-first");

    router.clearActiveStrategy();
    EXPECT_FALSE(router.getActiveStrategy().has_value());
}

// T6: empty registry returns empty (no crash, no default).
TEST_F(MultiObjectiveRouterTest, EmptyRegistryReturnsEmpty) {
    MultiObjectiveRouter router;
    ConnectorRegistry empty;
    RequestContext ctx;
    EXPECT_EQ(router.selectModel(ctx, empty), "");
}
