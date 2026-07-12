#include <gtest/gtest.h>
#include "gateway/ml_router.h"
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

class MLRouterTest : public ::testing::Test {
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

TEST_F(MLRouterTest, ExplicitModelRespected) {
    MLRouter router;
    RequestContext ctx;
    ctx.chat_request.model = "gpt-4o";
    EXPECT_EQ(router.selectModel(ctx, registry_), "gpt-4o");
}

TEST_F(MLRouterTest, ColdStartPrefersCheapest) {
    MLRouter router(MLRouter::Weights{1.0, 0.0, 0.0});
    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "hello"}};
    auto model = router.selectModel(ctx, registry_);
    EXPECT_EQ(model, "qwen2.5:7b");
}

TEST_F(MLRouterTest, ReportOutcomeAffectsSelection) {
    MLRouter router(MLRouter::Weights{0.0, 0.5, 0.5});
    for (int i = 0; i < 20; ++i) {
        router.reportOutcome("qwen2.5:7b", 500.0, false);
        router.reportOutcome("gpt-4o-mini", 50.0, true);
    }
    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "hello"}};
    auto model = router.selectModel(ctx, registry_);
    EXPECT_EQ(model, "gpt-4o-mini");
}

TEST_F(MLRouterTest, CustomWeightsApplied) {
    MLRouter router(MLRouter::Weights{0.0, 0.0, 1.0});
    router.reportOutcome("gpt-4o", 10.0, true);
    router.reportOutcome("gpt-4o-mini", 500.0, true);
    router.reportOutcome("qwen2.5:7b", 200.0, true);

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "hello"}};
    auto model = router.selectModel(ctx, registry_);
    EXPECT_EQ(model, "gpt-4o");
}

TEST_F(MLRouterTest, QualityTierEconomyFiltersCostly) {
    MLRouter router;
    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "hello"}};
    ctx.chat_request.extra["quality_tier"] = "economy";
    auto model = router.selectModel(ctx, registry_);
    EXPECT_NE(model, "gpt-4o");
}

TEST_F(MLRouterTest, QualityTierPremiumFiltersCheap) {
    MLRouter router;
    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "hello"}};
    ctx.chat_request.extra["quality_tier"] = "premium";
    auto model = router.selectModel(ctx, registry_);
    EXPECT_NE(model, "qwen2.5:7b");
}

TEST_F(MLRouterTest, EmptyRegistryReturnsEmpty) {
    MLRouter router;
    ConnectorRegistry empty_registry;
    RequestContext ctx;
    EXPECT_EQ(router.selectModel(ctx, empty_registry), "");
}

// Phase 11.5 E2.0 — per-tenant quality_tier override coverage.

TEST_F(MLRouterTest, OverrideQualityTierForcesEconomy) {
    MLRouter router;
    router.overrideQualityTier("tenant-A", "economy");
    ASSERT_EQ(router.getQualityTierOverride("tenant-A"),
              std::optional<std::string>("economy"));

    RequestContext ctx;
    ctx.tenant_id = "tenant-A";
    ctx.chat_request.messages = {{"user", "hi"}};
    // Even when client explicitly asks for premium, the override wins.
    ctx.chat_request.extra["quality_tier"] = "premium";
    auto model = router.selectModel(ctx, registry_);
    EXPECT_NE(model, "gpt-4o") << "premium model must be filtered out by economy override";
}

TEST_F(MLRouterTest, ClearOverrideRestoresClientChoice) {
    MLRouter router;
    router.overrideQualityTier("tenant-A", "economy");
    router.clearQualityTierOverride("tenant-A");
    EXPECT_FALSE(router.getQualityTierOverride("tenant-A").has_value());

    RequestContext ctx;
    ctx.tenant_id = "tenant-A";
    ctx.chat_request.messages = {{"user", "hi"}};
    ctx.chat_request.extra["quality_tier"] = "premium";
    auto model = router.selectModel(ctx, registry_);
    EXPECT_NE(model, "qwen2.5:7b")
        << "after clear, client-asked premium filter applies";
}

TEST_F(MLRouterTest, OverrideIsolatedPerTenant) {
    MLRouter router;
    router.overrideQualityTier("tenant-A", "economy");

    // tenant-B has no override; client asked premium and should get it.
    RequestContext ctx_b;
    ctx_b.tenant_id = "tenant-B";
    ctx_b.chat_request.messages = {{"user", "hi"}};
    ctx_b.chat_request.extra["quality_tier"] = "premium";
    auto model_b = router.selectModel(ctx_b, registry_);
    EXPECT_NE(model_b, "qwen2.5:7b");

    // tenant-A is overridden to economy.
    RequestContext ctx_a;
    ctx_a.tenant_id = "tenant-A";
    ctx_a.chat_request.messages = {{"user", "hi"}};
    auto model_a = router.selectModel(ctx_a, registry_);
    EXPECT_NE(model_a, "gpt-4o");

    EXPECT_FALSE(router.getQualityTierOverride("tenant-B").has_value());
    EXPECT_TRUE(router.getQualityTierOverride("tenant-A").has_value());
}

TEST_F(MLRouterTest, OverrideEmptyTenantNoop) {
    MLRouter router;
    router.overrideQualityTier("", "economy");   // ignored
    router.overrideQualityTier("t1", "");        // ignored
    EXPECT_FALSE(router.getQualityTierOverride("").has_value());
    EXPECT_FALSE(router.getQualityTierOverride("t1").has_value());
}
