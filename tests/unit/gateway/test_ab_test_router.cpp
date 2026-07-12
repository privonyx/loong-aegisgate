#include <gtest/gtest.h>
#include "gateway/ab_test_router.h"
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

TEST(ABTestRouterTest, DeterministicAssignment) {
    auto base = std::make_unique<BasicRouter>();
    ABExperiment exp;
    exp.name = "test_exp";
    exp.variants = {{"model-a", 50}, {"model-b", 50}};
    exp.enabled = true;

    ConnectorRegistry registry;
    ModelInfo a; a.id = "model-a"; a.provider = "p"; registry.registerModelInfo(a);
    ModelInfo b; b.id = "model-b"; b.provider = "p"; registry.registerModelInfo(b);
    registry.registerConnector(
        std::make_unique<StubConnector>("p",
            std::vector<std::string>{"model-a", "model-b"}));

    ABTestRouter router(std::move(base), {exp});

    RequestContext ctx;
    ctx.request_id = "req-001";
    ctx.chat_request.messages = {{"user", "hello"}};

    auto model1 = router.selectModel(ctx, registry);
    ctx.ab_experiment.clear();
    ctx.ab_variant.clear();
    auto model2 = router.selectModel(ctx, registry);
    EXPECT_EQ(model1, model2);
    EXPECT_FALSE(ctx.ab_experiment.empty());
    EXPECT_FALSE(ctx.ab_variant.empty());
}

TEST(ABTestRouterTest, WeightDistribution) {
    auto base = std::make_unique<BasicRouter>();
    ABExperiment exp;
    exp.name = "distribution_test";
    exp.variants = {{"model-a", 70}, {"model-b", 30}};
    exp.enabled = true;

    ConnectorRegistry registry;
    ModelInfo a; a.id = "model-a"; a.provider = "p"; registry.registerModelInfo(a);
    ModelInfo b; b.id = "model-b"; b.provider = "p"; registry.registerModelInfo(b);
    registry.registerConnector(
        std::make_unique<StubConnector>("p",
            std::vector<std::string>{"model-a", "model-b"}));

    ABTestRouter router(std::move(base), {exp});

    int count_a = 0, count_b = 0;
    for (int i = 0; i < 1000; ++i) {
        RequestContext ctx;
        ctx.request_id = "req-" + std::to_string(i);
        ctx.chat_request.messages = {{"user", "hi"}};
        auto model = router.selectModel(ctx, registry);
        if (model == "model-a") count_a++;
        else if (model == "model-b") count_b++;
    }
    EXPECT_GT(count_a, count_b);
    EXPECT_GT(count_a, 500);
    EXPECT_GT(count_b, 100);
}

TEST(ABTestRouterTest, ExplicitModelSkipsExperiment) {
    auto base = std::make_unique<BasicRouter>();
    ABExperiment exp;
    exp.name = "test";
    exp.variants = {{"model-a", 100}};
    exp.enabled = true;

    ConnectorRegistry registry;
    ModelInfo a; a.id = "model-a"; a.provider = "p"; registry.registerModelInfo(a);
    ModelInfo b; b.id = "model-b"; b.provider = "p"; registry.registerModelInfo(b);
    registry.registerConnector(
        std::make_unique<StubConnector>("p",
            std::vector<std::string>{"model-a", "model-b"}));
    registry.setDefaultModel("model-b");

    ABTestRouter router(std::move(base), {exp});

    RequestContext ctx;
    ctx.request_id = "req-1";
    ctx.chat_request.model = "model-b";
    EXPECT_EQ(router.selectModel(ctx, registry), "model-b");
    EXPECT_TRUE(ctx.ab_experiment.empty());
}

TEST(ABTestRouterTest, DisabledExperimentSkipped) {
    auto base = std::make_unique<BasicRouter>();
    ABExperiment exp;
    exp.name = "disabled";
    exp.variants = {{"model-a", 100}};
    exp.enabled = false;

    ConnectorRegistry registry;
    ModelInfo d; d.id = "model-default"; d.provider = "p"; registry.registerModelInfo(d);
    registry.registerConnector(
        std::make_unique<StubConnector>("p",
            std::vector<std::string>{"model-default"}));
    registry.setDefaultModel("model-default");

    ABTestRouter router(std::move(base), {exp});

    RequestContext ctx;
    ctx.request_id = "req-1";
    EXPECT_EQ(router.selectModel(ctx, registry), "model-default");
}

TEST(ABTestRouterTest, TenantFilterApplied) {
    auto base = std::make_unique<BasicRouter>();
    ABExperiment exp;
    exp.name = "tenant_test";
    exp.variants = {{"model-a", 100}};
    exp.enabled = true;
    exp.tenant_id = "tenant-1";

    ConnectorRegistry registry;
    ModelInfo a; a.id = "model-a"; a.provider = "p"; registry.registerModelInfo(a);
    ModelInfo d; d.id = "model-default"; d.provider = "p"; registry.registerModelInfo(d);
    registry.registerConnector(
        std::make_unique<StubConnector>("p",
            std::vector<std::string>{"model-a", "model-default"}));
    registry.setDefaultModel("model-default");

    ABTestRouter router(std::move(base), {exp});

    RequestContext ctx1;
    ctx1.request_id = "req-1";
    ctx1.tenant_id = "tenant-1";
    EXPECT_EQ(router.selectModel(ctx1, registry), "model-a");

    RequestContext ctx2;
    ctx2.request_id = "req-2";
    ctx2.tenant_id = "tenant-2";
    EXPECT_EQ(router.selectModel(ctx2, registry), "model-default");
}

TEST(ABTestRouterTest, UnavailableVariantFallsBack) {
    auto base = std::make_unique<BasicRouter>();
    ABExperiment exp;
    exp.name = "unavailable";
    exp.variants = {{"nonexistent-model", 100}};
    exp.enabled = true;

    ConnectorRegistry registry;
    ModelInfo d; d.id = "model-default"; d.provider = "p"; registry.registerModelInfo(d);
    registry.registerConnector(
        std::make_unique<StubConnector>("p",
            std::vector<std::string>{"model-default"}));
    registry.setDefaultModel("model-default");

    ABTestRouter router(std::move(base), {exp});

    RequestContext ctx;
    ctx.request_id = "req-1";
    EXPECT_EQ(router.selectModel(ctx, registry), "model-default");
}
