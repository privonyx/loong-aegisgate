#include <gtest/gtest.h>
#include "gateway/geo_router.h"
#include "gateway/router.h"
#include "gateway/ab_test_router.h"
#include "gateway/connector/base.h"
#include "gateway/connector/registry.h"
#include <memory>
#include <string>

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
        for (const auto& m : models_) {
            if (m == model) return true;
        }
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

// A router that always returns a fixed model name (for distinguishing passthrough behaviour).
class FixedRouter : public Router {
public:
    explicit FixedRouter(std::string m) : model_(std::move(m)) {}
    std::string selectModel(RequestContext&, const ConnectorRegistry&) override {
        return model_;
    }

private:
    std::string model_;
};

// TASK-20260703-02 Epic 7 / C12：记录 reportOutcome 转发的 spy 路由（模拟被装饰的
// 学习型路由 MLRouter/BanditRouter）。Geo/AB 装饰器若不 override reportOutcome，
// 反馈会停在基类 no-op → 内层学习型路由永远收不到 EMA 更新。
class OutcomeSpyRouter : public Router {
public:
    std::string selectModel(RequestContext&, const ConnectorRegistry&) override {
        return "spy-model";
    }
    void reportOutcome(const std::string& model, double latency_ms,
                       bool success) override {
        last_model = model;
        last_latency = latency_ms;
        last_success = success;
        ++calls;
    }
    std::string last_model;
    double last_latency = -1.0;
    bool last_success = false;
    int calls = 0;
};

ModelInfo makeModel(const std::string& id, const std::string& provider,
                    const std::vector<std::string>& tags,
                    double in_cost = 1.0, double out_cost = 1.0) {
    ModelInfo m;
    m.id = id;
    m.provider = provider;
    m.cost_per_1k_input = in_cost;
    m.cost_per_1k_output = out_cost;
    m.max_context_tokens = 8192;
    m.tags = tags;
    return m;
}

// Build a registry with a useful mix of regions:
//  - gpt-4o-us   : region:us-east
//  - gpt-4o-eu   : region:eu-central
//  - claude-apac : region:ap-east
//  - qwen-multi  : region:us-east, region:eu-central
//  - legacy-any  : (no region tag, "unregionalized")
ConnectorRegistry buildRegistry() {
    ConnectorRegistry reg;
    reg.registerConnector(std::make_unique<StubConnector>(
        "openai-us", std::vector<std::string>{"gpt-4o-us"}));
    reg.registerConnector(std::make_unique<StubConnector>(
        "openai-eu", std::vector<std::string>{"gpt-4o-eu"}));
    reg.registerConnector(std::make_unique<StubConnector>(
        "anthropic-apac", std::vector<std::string>{"claude-apac"}));
    reg.registerConnector(std::make_unique<StubConnector>(
        "multi", std::vector<std::string>{"qwen-multi"}));
    reg.registerConnector(std::make_unique<StubConnector>(
        "legacy", std::vector<std::string>{"legacy-any"}));

    reg.registerModelInfo(makeModel("gpt-4o-us", "openai-us", {"region:us-east", "high-quality"}));
    reg.registerModelInfo(makeModel("gpt-4o-eu", "openai-eu", {"region:eu-central", "high-quality"}, 1.1, 1.1));
    reg.registerModelInfo(makeModel("claude-apac", "anthropic-apac", {"region:ap-east", "high-quality"}));
    reg.registerModelInfo(makeModel("qwen-multi", "multi", {"region:us-east", "region:eu-central", "cheap"}, 0.1, 0.1));
    reg.registerModelInfo(makeModel("legacy-any", "legacy", {"cheap"}, 0.1, 0.1));
    reg.setDefaultModel("gpt-4o-us");
    return reg;
}

GeoConfig defaultEnabledConfig() {
    GeoConfig gc;
    gc.enabled = true;
    gc.affinity = GeoConfig::Affinity::Prefer;
    gc.default_client_region = "us-east";
    gc.header_names = {"X-AegisGate-Region", "X-Client-Region"};
    return gc;
}

} // namespace

// =============================================================================
// Region inference — priority chain
// =============================================================================

TEST(GeoRouterRegionInference, HeaderXAegisGateRegionHasHighestPriority) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["headers"] = {
        {"X-AegisGate-Region", "eu-central"},
        {"X-Client-Region", "us-east"}
    };
    ctx.chat_request.extra["client_region"] = "ap-east";
    ctx.chat_request.model = "gpt-4o-eu";

    auto picked = router.selectModel(ctx, reg);
    EXPECT_EQ(picked, "gpt-4o-eu");
    EXPECT_EQ(ctx.chat_request.extra["_geo_client_region"], "eu-central");
}

TEST(GeoRouterRegionInference, HeaderXClientRegionIsSecondPriority) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["headers"] = {
        {"X-Client-Region", "ap-east"}
    };
    ctx.chat_request.extra["client_region"] = "us-east";
    ctx.chat_request.model = "claude-apac";

    auto picked = router.selectModel(ctx, reg);
    EXPECT_EQ(picked, "claude-apac");
    EXPECT_EQ(ctx.chat_request.extra["_geo_client_region"], "ap-east");
}

TEST(GeoRouterRegionInference, ExtraClientRegionIsThirdPriority) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "eu-central";
    ctx.chat_request.model = "gpt-4o-eu";

    auto picked = router.selectModel(ctx, reg);
    EXPECT_EQ(picked, "gpt-4o-eu");
    EXPECT_EQ(ctx.chat_request.extra["_geo_client_region"], "eu-central");
}

TEST(GeoRouterRegionInference, IpCidrLookupIsFourthPriority) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.ip_region_map = {{"10.0.0.0/8", "us-east"}, {"172.16.0.0/12", "eu-central"}};
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_ip"] = "172.16.5.10";
    ctx.chat_request.model = "gpt-4o-eu";

    auto picked = router.selectModel(ctx, reg);
    EXPECT_EQ(picked, "gpt-4o-eu");
    EXPECT_EQ(ctx.chat_request.extra["_geo_client_region"], "eu-central");
}

TEST(GeoRouterRegionInference, DefaultClientRegionIsTheLastFallback) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.default_client_region = "ap-east";
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.model = "claude-apac";

    auto picked = router.selectModel(ctx, reg);
    EXPECT_EQ(picked, "claude-apac");
    EXPECT_EQ(ctx.chat_request.extra["_geo_client_region"], "ap-east");
}

TEST(GeoRouterRegionInference, IPv6AddressFallsThroughToDefault) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.default_client_region = "us-east";
    gc.ip_region_map = {{"10.0.0.0/8", "eu-central"}};
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_ip"] = "2001:db8::1";

    router.selectModel(ctx, reg);
    EXPECT_EQ(ctx.chat_request.extra["_geo_client_region"], "us-east");
}

TEST(GeoRouterRegionInference, InvalidCidrInConfigIsIgnored) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.ip_region_map = {{"not-a-cidr", "eu-central"}, {"10.0.0.0/8", "ap-east"}};
    gc.default_client_region = "us-east";
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_ip"] = "10.1.2.3";

    router.selectModel(ctx, reg);
    EXPECT_EQ(ctx.chat_request.extra["_geo_client_region"], "ap-east");
}

TEST(GeoRouterRegionInference, RegionAliasIsApplied) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.region_aliases = {{"us", "us-east"}, {"eu", "eu-central"}};
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "eu";
    ctx.chat_request.model = "gpt-4o-eu";

    auto picked = router.selectModel(ctx, reg);
    EXPECT_EQ(picked, "gpt-4o-eu");
    EXPECT_EQ(ctx.chat_request.extra["_geo_client_region"], "eu-central");
}

TEST(GeoRouterRegionInference, HeaderValueIsTrimmed) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["headers"] = {
        {"X-Client-Region", "   eu-central   "}
    };
    ctx.chat_request.model = "gpt-4o-eu";

    router.selectModel(ctx, reg);
    EXPECT_EQ(ctx.chat_request.extra["_geo_client_region"], "eu-central");
}

TEST(GeoRouterRegionInference, LowercaseRegionIsCanonicalized) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "EU-CENTRAL";
    ctx.chat_request.model = "gpt-4o-eu";

    auto picked = router.selectModel(ctx, reg);
    EXPECT_EQ(picked, "gpt-4o-eu");
    EXPECT_EQ(ctx.chat_request.extra["_geo_client_region"], "eu-central");
}

// =============================================================================
// modelRegions helper — static utility
// =============================================================================

TEST(GeoRouterModelRegions, NoRegionTagReturnsEmpty) {
    auto info = makeModel("m", "p", {"cheap", "fast"});
    auto regions = GeoRouter::modelRegions(info);
    EXPECT_TRUE(regions.empty());
}

TEST(GeoRouterModelRegions, SingleRegionTagParsed) {
    auto info = makeModel("m", "p", {"region:us-east", "fast"});
    auto regions = GeoRouter::modelRegions(info);
    ASSERT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions[0], "us-east");
}

TEST(GeoRouterModelRegions, MultipleRegionTagsParsed) {
    auto info = makeModel("m", "p", {"region:us-east", "cheap", "region:eu-central"});
    auto regions = GeoRouter::modelRegions(info);
    ASSERT_EQ(regions.size(), 2u);
    EXPECT_EQ(regions[0], "us-east");
    EXPECT_EQ(regions[1], "eu-central");
}

// =============================================================================
// Candidate filtering
// =============================================================================

TEST(GeoRouterCandidateFilter, MatchesSingleRegionModel) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "us-east";

    router.selectModel(ctx, reg);
    auto allowed = ctx.chat_request.extra["_geo_allowed_models"];
    ASSERT_TRUE(allowed.is_array());
    std::unordered_set<std::string> set;
    for (const auto& m : allowed) set.insert(m.get<std::string>());
    EXPECT_TRUE(set.count("gpt-4o-us"));
    EXPECT_TRUE(set.count("qwen-multi"));
    EXPECT_FALSE(set.count("gpt-4o-eu"));
    EXPECT_FALSE(set.count("claude-apac"));
    // Unregionalized model is allowed in Prefer affinity (residency not strict).
    EXPECT_TRUE(set.count("legacy-any"));
}

TEST(GeoRouterCandidateFilter, MultiRegionModelMatchesAnyRegion) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "eu-central";

    router.selectModel(ctx, reg);
    auto allowed = ctx.chat_request.extra["_geo_allowed_models"];
    std::unordered_set<std::string> set;
    for (const auto& m : allowed) set.insert(m.get<std::string>());
    EXPECT_TRUE(set.count("gpt-4o-eu"));
    EXPECT_TRUE(set.count("qwen-multi"));
}

TEST(GeoRouterCandidateFilter, NoRegionMatchLeavesAllowedEmpty) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.affinity = GeoConfig::Affinity::Strict;
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "antarctica";

    auto picked = router.selectModel(ctx, reg);
    EXPECT_EQ(picked, "");
}

TEST(GeoRouterCandidateFilter, ResidencyStrictExcludesUnregionalizedModels) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "us-east";
    ctx.chat_request.extra["residency"] = "strict";

    router.selectModel(ctx, reg);
    auto allowed = ctx.chat_request.extra["_geo_allowed_models"];
    std::unordered_set<std::string> set;
    for (const auto& m : allowed) set.insert(m.get<std::string>());
    EXPECT_TRUE(set.count("gpt-4o-us"));
    EXPECT_TRUE(set.count("qwen-multi"));
    EXPECT_FALSE(set.count("legacy-any"));  // unregionalized excluded by residency strict
}

// =============================================================================
// Affinity policy
// =============================================================================

TEST(GeoRouterAffinity, StrictPolicyReturnsEmptyWhenNoCandidates) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.affinity = GeoConfig::Affinity::Strict;
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "antarctica";
    ctx.chat_request.model = "legacy-any";

    EXPECT_EQ(router.selectModel(ctx, reg), "");
}

TEST(GeoRouterAffinity, PreferPolicyFallsBackToAllOnNoMatch) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.affinity = GeoConfig::Affinity::Prefer;
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "antarctica";
    ctx.chat_request.model = "legacy-any";

    EXPECT_EQ(router.selectModel(ctx, reg), "legacy-any");
}

TEST(GeoRouterAffinity, AnyPolicyFallsBackToAllOnNoMatch) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.affinity = GeoConfig::Affinity::Any;
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "antarctica";
    ctx.chat_request.model = "gpt-4o-us";

    EXPECT_EQ(router.selectModel(ctx, reg), "gpt-4o-us");
}

TEST(GeoRouterAffinity, StrictForcesRePickWhenBaseReturnsNonCompliant) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.affinity = GeoConfig::Affinity::Strict;
    GeoRouter router(std::make_unique<FixedRouter>("claude-apac"), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "us-east";

    auto picked = router.selectModel(ctx, reg);
    // Strict must not accept claude-apac (ap-east) when client is us-east.
    EXPECT_NE(picked, "claude-apac");
    EXPECT_FALSE(picked.empty());
    // The chosen model must be one of the allowed set.
    auto allowed = ctx.chat_request.extra["_geo_allowed_models"];
    bool in_allowed = false;
    for (const auto& m : allowed) {
        if (m.get<std::string>() == picked) { in_allowed = true; break; }
    }
    EXPECT_TRUE(in_allowed);
}

TEST(GeoRouterAffinity, PreferAllowsNonCompliantFromBase) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.affinity = GeoConfig::Affinity::Prefer;
    GeoRouter router(std::make_unique<FixedRouter>("claude-apac"), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "us-east";

    EXPECT_EQ(router.selectModel(ctx, reg), "claude-apac");
}

// =============================================================================
// Decorator integration
// =============================================================================

TEST(GeoRouterDecorator, WrapsBasicRouterPassthrough) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.model = "gpt-4o-us";
    ctx.chat_request.extra["client_region"] = "us-east";

    EXPECT_EQ(router.selectModel(ctx, reg), "gpt-4o-us");
}

TEST(GeoRouterDecorator, WrapsCostAwareRouterPassthrough) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    GeoRouter router(std::make_unique<CostAwareRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "hello"}};
    ctx.chat_request.extra["client_region"] = "us-east";

    auto picked = router.selectModel(ctx, reg);
    EXPECT_FALSE(picked.empty());
}

TEST(GeoRouterDecorator, WrapsABTestRouterPassthrough) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.affinity = GeoConfig::Affinity::Prefer;

    std::vector<ABExperiment> exps;
    ABExperiment exp;
    exp.name = "us-exp";
    exp.variants = {{"gpt-4o-us", 1}};
    exp.enabled = true;
    exps.push_back(exp);
    auto ab = std::make_unique<ABTestRouter>(std::make_unique<BasicRouter>(), std::move(exps));
    GeoRouter router(std::move(ab), gc);

    RequestContext ctx;
    ctx.request_id = "req-1";
    ctx.chat_request.extra["client_region"] = "us-east";
    ctx.chat_request.model = "gpt-4o-us";

    EXPECT_EQ(router.selectModel(ctx, reg), "gpt-4o-us");
}

TEST(GeoRouterDecorator, BaseReturnsEmptyGeoReturnsEmpty) {
    ConnectorRegistry reg;  // empty
    GeoConfig gc = defaultEnabledConfig();
    gc.affinity = GeoConfig::Affinity::Prefer;
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "us-east";

    EXPECT_EQ(router.selectModel(ctx, reg), "");
}

// =============================================================================
// Residency
// =============================================================================

TEST(GeoRouterResidency, StrictOverridesAffinityPrefer) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.affinity = GeoConfig::Affinity::Prefer;
    GeoRouter router(std::make_unique<FixedRouter>("claude-apac"), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "us-east";
    ctx.chat_request.extra["residency"] = "strict";

    auto picked = router.selectModel(ctx, reg);
    // Residency=strict should force compliance, overriding affinity=prefer.
    EXPECT_NE(picked, "claude-apac");
}

TEST(GeoRouterResidency, NotSetUsesAffinity) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.affinity = GeoConfig::Affinity::Prefer;
    GeoRouter router(std::make_unique<FixedRouter>("claude-apac"), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "us-east";
    // residency not set → affinity=prefer should allow non-compliant claude-apac.

    EXPECT_EQ(router.selectModel(ctx, reg), "claude-apac");
}

// =============================================================================
// Observability
// =============================================================================

TEST(GeoRouterObservability, WritesClientAndSelectedRegion) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "eu-central";
    ctx.chat_request.model = "gpt-4o-eu";

    router.selectModel(ctx, reg);
    EXPECT_EQ(ctx.chat_request.extra["_geo_client_region"], "eu-central");
    EXPECT_EQ(ctx.chat_request.extra["_geo_selected_region"], "eu-central");
}

TEST(GeoRouterObservability, SelectedRegionIsUnknownForUnregionalizedModel) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.affinity = GeoConfig::Affinity::Any;
    GeoRouter router(std::make_unique<FixedRouter>("legacy-any"), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "us-east";

    router.selectModel(ctx, reg);
    EXPECT_EQ(ctx.chat_request.extra["_geo_selected_region"], "unknown");
}

// =============================================================================
// Enabled switch & transparency
// =============================================================================

TEST(GeoRouterDisabled, TransparentPassthroughWhenDisabled) {
    auto reg = buildRegistry();
    GeoConfig gc;  // enabled=false by default
    GeoRouter router(std::make_unique<FixedRouter>("claude-apac"), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "us-east";
    ctx.chat_request.extra["residency"] = "strict";

    // Disabled => transparent passthrough regardless of residency / client_region.
    EXPECT_EQ(router.selectModel(ctx, reg), "claude-apac");
    EXPECT_FALSE(ctx.chat_request.extra.contains("_geo_client_region"));
    EXPECT_FALSE(ctx.chat_request.extra.contains("_geo_allowed_models"));
}

// =============================================================================
// Edge cases and robustness
// =============================================================================

TEST(GeoRouterEdge, EmptyRegistryReturnsEmpty) {
    ConnectorRegistry reg;
    GeoConfig gc = defaultEnabledConfig();
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_region"] = "us-east";
    EXPECT_EQ(router.selectModel(ctx, reg), "");
}

TEST(GeoRouterEdge, InvalidIpInRequestDoesNotCrash) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.ip_region_map = {{"10.0.0.0/8", "us-east"}};
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_ip"] = "999.999.999.999";
    ctx.chat_request.model = "gpt-4o-us";

    EXPECT_NO_THROW({
        auto picked = router.selectModel(ctx, reg);
        (void)picked;
    });
    EXPECT_EQ(ctx.chat_request.extra["_geo_client_region"],
              gc.default_client_region);
}

TEST(GeoRouterEdge, IpCidrMatchesExactBoundary) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.ip_region_map = {{"192.168.1.0/24", "eu-central"}};
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_ip"] = "192.168.1.255";
    ctx.chat_request.model = "gpt-4o-eu";

    router.selectModel(ctx, reg);
    EXPECT_EQ(ctx.chat_request.extra["_geo_client_region"], "eu-central");
}

TEST(GeoRouterEdge, IpCidrDoesNotMatchOutOfRange) {
    auto reg = buildRegistry();
    GeoConfig gc = defaultEnabledConfig();
    gc.default_client_region = "ap-east";
    gc.ip_region_map = {{"192.168.1.0/24", "eu-central"}};
    GeoRouter router(std::make_unique<BasicRouter>(), gc);

    RequestContext ctx;
    ctx.chat_request.extra["client_ip"] = "192.168.2.1";
    ctx.chat_request.model = "claude-apac";

    router.selectModel(ctx, reg);
    EXPECT_EQ(ctx.chat_request.extra["_geo_client_region"], "ap-east");
}

// =============================================================================
// TASK-20260703-02 Epic 7 / C12 — 装饰器 reportOutcome 转发到内层学习型路由。
// 根因：GeoRouter / ABTestRouter 未 override reportOutcome → 反馈停在基类 no-op
// → 被装饰的 MLRouter/BanditRouter 的 EMA/统计永不更新（路由学习失效）。
// =============================================================================

TEST(GeoRouterOutcome, ForwardsReportOutcomeToBase) {
    auto spy = std::make_unique<OutcomeSpyRouter>();
    auto* spy_ptr = spy.get();
    GeoConfig gc;  // disabled 亦须转发（reportOutcome 与 enabled 无关）
    GeoRouter router(std::move(spy), gc);

    router.reportOutcome("gpt-4", 123.0, true);
    EXPECT_EQ(spy_ptr->calls, 1);
    EXPECT_EQ(spy_ptr->last_model, "gpt-4");
    EXPECT_DOUBLE_EQ(spy_ptr->last_latency, 123.0);
    EXPECT_TRUE(spy_ptr->last_success);
}

TEST(ABTestRouterOutcome, ForwardsReportOutcomeToBase) {
    auto spy = std::make_unique<OutcomeSpyRouter>();
    auto* spy_ptr = spy.get();
    ABTestRouter router(std::move(spy), {});

    router.reportOutcome("claude", 45.0, false);
    EXPECT_EQ(spy_ptr->calls, 1);
    EXPECT_EQ(spy_ptr->last_model, "claude");
    EXPECT_DOUBLE_EQ(spy_ptr->last_latency, 45.0);
    EXPECT_FALSE(spy_ptr->last_success);
}
