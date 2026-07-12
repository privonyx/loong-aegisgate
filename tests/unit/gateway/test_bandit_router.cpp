// Phase 11.2 TASK-20260521-03 — BanditRouter unit tests.
//
// BanditRouter is a decorator (D7=C) that wraps a base Router. In Shadow
// mode (default per SR5) it forwards to the base and records the bandit
// candidate decision for offline analysis. In Live mode (only entered
// via transitionToLive() which requires isAutonomyEnabled()) it serves
// traffic based on ε-greedy or Thompson Sampling.
//
// Test coverage anchors mutation candidates M1 (default Shadow ctor) and
// M2 (transitionToLive must check isAutonomyEnabled).

#include <gtest/gtest.h>
#include "gateway/bandit_router.h"
#include "gateway/connector/base.h"
#include "gateway/connector/registry.h"

#include <cstdlib>

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

// Records what model the base picks so we can verify shadow forwarding.
class FixedBaseRouter : public Router {
public:
    explicit FixedBaseRouter(std::string pick) : pick_(std::move(pick)) {}
    std::string selectModel(RequestContext&,
                             const ConnectorRegistry&) override {
        invocations_++;
        return pick_;
    }
    int invocations_ = 0;
    std::string pick_;
};

}  // namespace

class BanditRouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Tests assume autonomy is enabled.
        unsetenv("AEGISGATE_DISABLE_AUTONOMY");

        registry_.registerConnector(std::make_unique<StubConnector>(
            "openai", std::vector<std::string>{"gpt-4o", "gpt-4o-mini"}));
        registry_.registerConnector(std::make_unique<StubConnector>(
            "local", std::vector<std::string>{"qwen2.5:7b"}));

        ModelInfo gpt4o;
        gpt4o.id = "gpt-4o"; gpt4o.provider = "openai";
        gpt4o.cost_per_1k_input = 0.005; gpt4o.cost_per_1k_output = 0.015;
        registry_.registerModelInfo(gpt4o);

        ModelInfo gpt4omini;
        gpt4omini.id = "gpt-4o-mini"; gpt4omini.provider = "openai";
        gpt4omini.cost_per_1k_input = 0.00015;
        gpt4omini.cost_per_1k_output = 0.0006;
        registry_.registerModelInfo(gpt4omini);

        ModelInfo qwen;
        qwen.id = "qwen2.5:7b"; qwen.provider = "local";
        qwen.cost_per_1k_input = 0; qwen.cost_per_1k_output = 0;
        registry_.registerModelInfo(qwen);
        registry_.setDefaultModel("gpt-4o-mini");
    }
    ConnectorRegistry registry_;
};

// === M1 anchor: BanditRouter ctor MUST default to Shadow mode (SR5). ===
// Mutation injection log (TASK-20260521-02 P1-B independent-commit rule):
//   Inject:   bandit_router.h Config{ mode = BanditMode::Live }
//   Expected: this test FAILs
//   Verified: 2026-05-22, exit_code=1, "Which is: 4-byte object <01-00 00-00>"
//             vs expected BanditMode::Shadow <00-00 00-00>
//   Restore:  Config{ mode = BanditMode::Shadow }
//   Verified: 2026-05-22, exit_code=0, PASS
TEST_F(BanditRouterTest, bandit_router_defaults_to_shadow) {
    FixedBaseRouter base("gpt-4o-mini");
    BanditRouter router(&base, BanditRouter::Config{});
    EXPECT_EQ(router.getMode(), BanditMode::Shadow);
}

// Shadow mode forwards to base router (decision recorded but base wins).
TEST_F(BanditRouterTest, ShadowModeForwardsToBaseRouter) {
    FixedBaseRouter base("gpt-4o-mini");
    BanditRouter router(&base, BanditRouter::Config{});
    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "hi"}};
    EXPECT_EQ(router.selectModel(ctx, registry_), "gpt-4o-mini");
    EXPECT_EQ(base.invocations_, 1);
}

// === M2 anchor: transitionToLive MUST consult isAutonomyEnabled (SR5/SR17). ===
// Mutation injection log (TASK-20260521-02 P1-B independent-commit rule):
//   Inject:   bandit_router.cpp::transitionToLive — remove isAutonomyEnabled()
//             guard (just keep mutex_ + cfg_.mode = Live)
//   Expected: this test FAILs (Live mode reached even with env=1)
//   Verified: 2026-05-22, exit_code=1
//   Restore:  re-add the if(!isAutonomyEnabled()) return false guard
//   Verified: 2026-05-22, exit_code=0, PASS
TEST_F(BanditRouterTest, bandit_transition_requires_autonomy_enabled) {
    FixedBaseRouter base("gpt-4o-mini");
    BanditRouter router(&base, BanditRouter::Config{});

    setenv("AEGISGATE_DISABLE_AUTONOMY", "1", 1);
    bool ok = router.transitionToLive(0.05);
    EXPECT_FALSE(ok);
    EXPECT_EQ(router.getMode(), BanditMode::Shadow);
    unsetenv("AEGISGATE_DISABLE_AUTONOMY");

    bool ok2 = router.transitionToLive(0.05);
    EXPECT_TRUE(ok2);
    EXPECT_EQ(router.getMode(), BanditMode::Live);
}

// Live mode + ε-greedy: ε=1.0 (always explore) should serve qwen at least
// once across 30 trials given the 3-arm setup.
TEST_F(BanditRouterTest, LiveModeEpsilonGreedyExplores) {
    FixedBaseRouter base("gpt-4o-mini");
    BanditRouter::Config cfg;
    cfg.algorithm = BanditAlgorithm::EpsilonGreedy;
    cfg.epsilon = 1.0;
    BanditRouter router(&base, cfg);
    ASSERT_TRUE(router.transitionToLive(0.05));

    std::unordered_map<std::string, int> picks;
    for (int i = 0; i < 30; ++i) {
        RequestContext ctx;
        ctx.request_id = "req-" + std::to_string(i);
        ctx.chat_request.messages = {{"user", "hi"}};
        picks[router.selectModel(ctx, registry_)]++;
    }
    // All three arms should have appeared at least once with ε=1.0.
    EXPECT_GE(picks["gpt-4o"], 1);
    EXPECT_GE(picks["gpt-4o-mini"], 1);
    EXPECT_GE(picks["qwen2.5:7b"], 1);
    // Base is NOT consulted in Live mode.
    EXPECT_EQ(base.invocations_, 0);
}

// Live mode + ε-greedy with ε=0: pure exploit. After 50 wins on gpt-4o
// and 50 losses on qwen, gpt-4o must dominate.
TEST_F(BanditRouterTest, LiveModeEpsilonGreedyExploits) {
    FixedBaseRouter base("gpt-4o-mini");
    BanditRouter::Config cfg;
    cfg.algorithm = BanditAlgorithm::EpsilonGreedy;
    cfg.epsilon = 0.0;
    BanditRouter router(&base, cfg);
    ASSERT_TRUE(router.transitionToLive(0.05));

    for (int i = 0; i < 50; ++i) {
        router.recordOutcome("gpt-4o", true, 1.0);
        router.recordOutcome("qwen2.5:7b", false, 0.0);
        router.recordOutcome("gpt-4o-mini", false, 0.0);
    }
    int gpt4o_picks = 0;
    for (int i = 0; i < 50; ++i) {
        RequestContext ctx;
        ctx.request_id = "req-" + std::to_string(i);
        ctx.chat_request.messages = {{"user", "hi"}};
        if (router.selectModel(ctx, registry_) == "gpt-4o") gpt4o_picks++;
    }
    EXPECT_EQ(gpt4o_picks, 50);
}

// Live mode + Thompson Sampling: gpt-4o wins 100 times, qwen loses 100
// times; Thompson posteriors should heavily favor gpt-4o.
TEST_F(BanditRouterTest, LiveModeThompsonSamplingConverges) {
    FixedBaseRouter base("gpt-4o-mini");
    BanditRouter::Config cfg;
    cfg.algorithm = BanditAlgorithm::ThompsonSampling;
    BanditRouter router(&base, cfg);
    ASSERT_TRUE(router.transitionToLive(0.05));

    for (int i = 0; i < 100; ++i) {
        router.recordOutcome("gpt-4o", true, 1.0);
        router.recordOutcome("qwen2.5:7b", false, 0.0);
        router.recordOutcome("gpt-4o-mini", false, 0.0);
    }
    int gpt4o_picks = 0;
    for (int i = 0; i < 100; ++i) {
        RequestContext ctx;
        ctx.request_id = "req-" + std::to_string(i);
        ctx.chat_request.messages = {{"user", "hi"}};
        if (router.selectModel(ctx, registry_) == "gpt-4o") gpt4o_picks++;
    }
    // Thompson with 100 wins vs 100 losses each should pick gpt-4o > 80%.
    EXPECT_GE(gpt4o_picks, 80)
        << "Thompson posterior should heavily favor 100-0 winner";
}

// revertToShadow returns to forwarding to base.
TEST_F(BanditRouterTest, RevertToShadowGoesBackToBase) {
    FixedBaseRouter base("gpt-4o-mini");
    BanditRouter router(&base, BanditRouter::Config{});
    ASSERT_TRUE(router.transitionToLive(0.05));
    router.revertToShadow();
    EXPECT_EQ(router.getMode(), BanditMode::Shadow);

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "hi"}};
    EXPECT_EQ(router.selectModel(ctx, registry_), "gpt-4o-mini");
    EXPECT_EQ(base.invocations_, 1);
}
