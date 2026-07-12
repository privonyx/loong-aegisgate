#include <gtest/gtest.h>
#include "gateway/router.h"
#include "gateway/connector/base.h"
#include <memory>

using namespace aegisgate;

// P2-#6: the self-evolving stack names are classified as offline/shadow-only so
// the runtime warns instead of silently degrading; live-wired and unknown types
// are not flagged as such.
TEST(RouterTypeClassificationTest, OfflineOnlyStrategiesAreFlagged) {
    EXPECT_TRUE(isOfflineOnlyRouterType("multi_objective"));
    EXPECT_TRUE(isOfflineOnlyRouterType("bandit"));
    EXPECT_TRUE(isOfflineOnlyRouterType("catalog"));
    EXPECT_TRUE(isOfflineOnlyRouterType("self_evolving"));
}

TEST(RouterTypeClassificationTest, LiveAndUnknownTypesAreNotFlagged) {
    EXPECT_FALSE(isOfflineOnlyRouterType("ml"));
    EXPECT_FALSE(isOfflineOnlyRouterType("basic"));
    EXPECT_FALSE(isOfflineOnlyRouterType("cost_aware"));
    EXPECT_FALSE(isOfflineOnlyRouterType("typo_xyz"));
}

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

class RouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_.registerConnector(
            std::make_unique<StubConnector>("openai", std::vector<std::string>{"gpt-4o", "gpt-4o-mini"}));
        registry_.registerConnector(
            std::make_unique<StubConnector>("local", std::vector<std::string>{"qwen2.5:7b"}));

        ModelInfo gpt4o{"gpt-4o", "openai", 0.005, 0.015, 128000,
                        {"high-quality", "expensive"}, {}};
        ModelInfo gpt4omini{"gpt-4o-mini", "openai", 0.00015, 0.0006, 128000,
                            {"fast", "cheap"}, {}};
        ModelInfo qwen{"qwen2.5:7b", "local", 0, 0, 32000,
                       {"free", "local", "fast"}, {}};

        registry_.registerModelInfo(gpt4o);
        registry_.registerModelInfo(gpt4omini);
        registry_.registerModelInfo(qwen);
        registry_.setDefaultModel("gpt-4o-mini");
    }

    ConnectorRegistry registry_;
    BasicRouter router_;
};

TEST_F(RouterTest, DirectModelRouting) {
    RequestContext ctx;
    ctx.chat_request.model = "gpt-4o";
    EXPECT_EQ(router_.selectModel(ctx, registry_), "gpt-4o");
}

// P1-7: BasicRouter records why it picked the model.
TEST_F(RouterTest, PinnedModelSetsRoutingReasonP1_7) {
    RequestContext ctx;
    ctx.chat_request.model = "gpt-4o";
    router_.selectModel(ctx, registry_);
    EXPECT_EQ(ctx.routing_decision_reason, "user_pinned");
}

TEST_F(RouterTest, DefaultModelSetsRoutingReasonP1_7) {
    RequestContext ctx;
    router_.selectModel(ctx, registry_);
    EXPECT_EQ(ctx.routing_decision_reason, "router_default");
}

TEST_F(RouterTest, DefaultModelWhenEmpty) {
    RequestContext ctx;
    EXPECT_EQ(router_.selectModel(ctx, registry_), "gpt-4o-mini");
}

TEST_F(RouterTest, TagBasedRouting) {
    RequestContext ctx;
    ctx.chat_request.extra = {{"preferred_tag", "free"}};
    auto model = router_.selectModel(ctx, registry_);
    EXPECT_EQ(model, "qwen2.5:7b");
}

TEST_F(RouterTest, UnknownModelFallsToDefault) {
    RequestContext ctx;
    ctx.chat_request.model = "nonexistent-model";
    EXPECT_EQ(router_.selectModel(ctx, registry_), "gpt-4o-mini");
}

TEST_F(RouterTest, TagRoutingSortedByCost) {
    // Both gpt-4o-mini and qwen2.5:7b have "fast" tag
    RequestContext ctx;
    ctx.chat_request.extra = {{"preferred_tag", "fast"}};
    auto model = router_.selectModel(ctx, registry_);
    // qwen2.5:7b is free (cost=0), so it should be selected over gpt-4o-mini
    EXPECT_EQ(model, "qwen2.5:7b");
}

// --- CostAwareRouter tests ---

class CostAwareRouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_.registerConnector(
            std::make_unique<StubConnector>("openai", std::vector<std::string>{"gpt-4o", "gpt-4o-mini"}));
        registry_.registerConnector(
            std::make_unique<StubConnector>("local", std::vector<std::string>{"qwen2.5:7b"}));

        ModelInfo gpt4o{"gpt-4o", "openai", 0.005, 0.015, 128000,
                        {"high-quality", "expensive"}, {}};
        ModelInfo gpt4omini{"gpt-4o-mini", "openai", 0.00015, 0.0006, 128000,
                            {"fast", "cheap"}, {}};
        ModelInfo qwen{"qwen2.5:7b", "local", 0, 0, 32000,
                       {"free", "local", "fast"}, {}};

        registry_.registerModelInfo(gpt4o);
        registry_.registerModelInfo(gpt4omini);
        registry_.registerModelInfo(qwen);
        registry_.setDefaultModel("gpt-4o-mini");
    }

    ConnectorRegistry registry_;
    CostAwareRouter router_;
};

TEST_F(CostAwareRouterTest, ExplicitModelRespected) {
    RequestContext ctx;
    ctx.chat_request.model = "gpt-4o";
    EXPECT_EQ(router_.selectModel(ctx, registry_), "gpt-4o");
}

TEST_F(CostAwareRouterTest, ShortMessageRoutesCheap) {
    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "Hello"}};
    auto model = router_.selectModel(ctx, registry_);
    // Short message should get cheapest available model
    EXPECT_EQ(model, "qwen2.5:7b");
}

TEST_F(CostAwareRouterTest, LongComplexMessageRoutesExpensive) {
    RequestContext ctx;
    // Generate a long message (>2000 chars) simulating complex request
    std::string long_msg(2500, 'x');
    ctx.chat_request.messages = {
        {"system", "You are an expert software architect."},
        {"user", long_msg}
    };
    auto model = router_.selectModel(ctx, registry_);
    // Long complex message should route to high-quality model
    EXPECT_EQ(model, "gpt-4o");
}

TEST_F(CostAwareRouterTest, MediumMessageRoutesMidTier) {
    RequestContext ctx;
    std::string medium_msg(800, 'y');
    ctx.chat_request.messages = {{"user", medium_msg}};
    auto model = router_.selectModel(ctx, registry_);
    // Medium message should route to mid-tier model
    EXPECT_EQ(model, "gpt-4o-mini");
}

TEST_F(CostAwareRouterTest, TagOverridesCostRouting) {
    RequestContext ctx;
    std::string long_msg(3000, 'z');
    ctx.chat_request.messages = {{"user", long_msg}};
    ctx.chat_request.extra = {{"preferred_tag", "free"}};
    auto model = router_.selectModel(ctx, registry_);
    EXPECT_EQ(model, "qwen2.5:7b");
}

// 独立 suite：不能与 TEST_F(RouterTest, ...) 混用同一 suite 名（GTest 限制）
TEST(RouterQualityTierTest, SelectsCheapestModelForEconomyTier) {
    CostAwareRouter router;
    ConnectorRegistry registry;

    registry.registerConnector(
        std::make_unique<StubConnector>("openai", std::vector<std::string>{"gpt-4o-mini", "gpt-4o"}));

    ModelInfo cheap;
    cheap.id = "gpt-4o-mini";
    cheap.provider = "openai";
    cheap.cost_per_1k_input = 0.15;
    cheap.cost_per_1k_output = 0.60;
    registry.registerModelInfo(cheap);

    ModelInfo expensive;
    expensive.id = "gpt-4o";
    expensive.provider = "openai";
    expensive.cost_per_1k_input = 5.00;
    expensive.cost_per_1k_output = 15.00;
    registry.registerModelInfo(expensive);

    RequestContext ctx;
    ctx.chat_request.extra["quality_tier"] = "economy";
    ctx.chat_request.messages.push_back({"user", "hello"});

    auto model = router.selectModel(ctx, registry);
    EXPECT_EQ(model, "gpt-4o-mini");
    // P1-7: economy tier records router_economy.
    EXPECT_EQ(ctx.routing_decision_reason, "router_economy");
}

TEST(RouterQualityTierTest, SelectsMostExpensiveForPremiumTier) {
    CostAwareRouter router;
    ConnectorRegistry registry;

    registry.registerConnector(
        std::make_unique<StubConnector>("openai", std::vector<std::string>{"gpt-4o-mini", "gpt-4o"}));

    ModelInfo cheap;
    cheap.id = "gpt-4o-mini";
    cheap.provider = "openai";
    cheap.cost_per_1k_input = 0.15;
    cheap.cost_per_1k_output = 0.60;
    registry.registerModelInfo(cheap);

    ModelInfo expensive;
    expensive.id = "gpt-4o";
    expensive.provider = "openai";
    expensive.cost_per_1k_input = 5.00;
    expensive.cost_per_1k_output = 15.00;
    registry.registerModelInfo(expensive);

    RequestContext ctx;
    ctx.chat_request.extra["quality_tier"] = "premium";
    ctx.chat_request.messages.push_back({"user", "hello"});

    auto model = router.selectModel(ctx, registry);
    EXPECT_EQ(model, "gpt-4o");
    // P1-7: premium tier records router_quality.
    EXPECT_EQ(ctx.routing_decision_reason, "router_quality");
}

TEST(RouterQualityTierTest, FallsBackToCharCountWithoutTier) {
    CostAwareRouter router;
    ConnectorRegistry registry;
    RequestContext ctx;
    ctx.chat_request.messages.push_back({"user", "hello"});
    EXPECT_NO_THROW({
        auto model = router.selectModel(ctx, registry);
        (void)model;
    });
}
