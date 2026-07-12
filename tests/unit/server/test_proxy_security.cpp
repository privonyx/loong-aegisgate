#include <gtest/gtest.h>
#include "server/gateway_runtime.h"
#include "core/config.h"
#include "core/feature_gate.h"
#include "auth/auth_service.h"
#include "storage/memory_persistent_store.h"

using namespace aegisgate;

class ProxySecurityTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        auto& rt = GatewayRuntime::instance();
        rt.resetShutdownForTesting();
        if (!rt.isInitialized()) {
            static Config config;
            config.loadFromFile("config/aegisgate.yaml");
            rt.initialize(config);
        }
    }
    void TearDown() override {
        GatewayRuntime::instance().resetShutdownForTesting();
    }
};

TEST_F(ProxySecurityTest, UnknownEndpointReturns404) {
    auto& rt = GatewayRuntime::instance();
    ProxyRequest req;
    req.endpoint = "/v1/nonexistent";
    req.raw_body = "{}";
    auto result = rt.processProxyRequest(std::move(req), "test-key");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error.http_status, 404);
}

// SR-2 (P0-2): with RBAC enabled, an unresolved/invalid token presented to a
// proxy endpoint must be rejected with 401 *before* upstream dispatch — mirror
// of the chat path. Without the auth check, an invalid token would reach the
// connector lookup (404/connector error), bypassing authentication.
TEST_F(ProxySecurityTest, RbacRejectsInvalidProxyToken_SR2) {
    auto& rt = GatewayRuntime::instance();

    // Process-lifetime deps: AuthService holds raw pointers to these.
    static MemoryPersistentStore store;
    store.initialize();
    static Config cfg;  // RBAC gating comes from the FeatureGate, not config.
    static FeatureGate gate = FeatureGate::createUnlocked(Edition::Enterprise);
    ASSERT_TRUE(gate.isEnabled(Feature::RBAC));

    rt.setAuthService(std::make_unique<AuthService>(&store, &cfg, &gate));

    ProxyRequest req;
    req.endpoint = "/v1/embeddings";
    req.raw_body = R"({"input":"x","model":"gpt-4o"})";
    req.model = "gpt-4o";
    auto result = rt.processProxyRequest(std::move(req), "invalid-token-xyz");

    // Restore default (null) auth service so sibling tests keep their behavior.
    rt.setAuthService(nullptr);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error.http_status, 401);
}

// P0-A (TASK-20260701-01): auxiliary read endpoints (models/metrics/cache
// stats) used validateApiKey(), which fail-opens under RBAC on the assumption
// a later resolve() enforces auth — but those endpoints never resolve. This
// pins that authorizeApiRequest() actually verifies the token under RBAC,
// while documenting the validateApiKey() fail-open it replaces.
TEST_F(ProxySecurityTest, RbacAuthorizeApiRequestRejectsInvalidToken_P0A) {
    auto& rt = GatewayRuntime::instance();

    static MemoryPersistentStore store;
    store.initialize();
    static Config cfg;
    static FeatureGate gate = FeatureGate::createUnlocked(Edition::Enterprise);
    ASSERT_TRUE(gate.isEnabled(Feature::RBAC));

    rt.setAuthService(std::make_unique<AuthService>(&store, &cfg, &gate));

    // The fail-open being fixed: validateApiKey trusts any token under RBAC.
    EXPECT_TRUE(rt.validateApiKey("invalid-token-xyz"));
    // The fix: the read-endpoint authorizer must reject an unresolvable token.
    EXPECT_FALSE(rt.authorizeApiRequest("invalid-token-xyz"));
    EXPECT_FALSE(rt.authorizeApiRequest(""));

    rt.setAuthService(nullptr);
}

TEST_F(ProxySecurityTest, ProxyRequestRateLimited) {
    auto& rt = GatewayRuntime::instance();
    auto rl = rt.rateLimiterSnapshot();
    if (!rl) {
        GTEST_SKIP() << "Rate limiter not configured";
    }

    const std::string rl_key = "proxy-rl-exhaust-test";
    // Deterministically exhaust this key's bucket. A zero-capacity, zero-refill
    // config cannot recover between here and the proxy call below, so the
    // gateway's internal allow(rl_key, 1.0) is guaranteed to fail.
    //
    // The previous approach (draining with cost 100 against the global
    // max_tokens=10000/refill_rate=100 config) left a residual in [0, 100) that
    // refill could push back above the 1.0 request cost, making the assertion
    // flaky — on slower CI the request slipped through to upstream dispatch and
    // returned 502 ("No healthy API keys available") instead of 429.
    rl->setKeyConfig(rl_key, RateLimiter::Config{/*max_tokens=*/0.0,
                                                 /*refill_rate=*/0.0});
    ASSERT_FALSE(rl->allow(rl_key, 1.0));

    ProxyRequest req;
    req.endpoint = "/v1/embeddings";
    req.raw_body = R"({"input":"test","model":"gpt-4o"})";
    req.model = "gpt-4o";
    auto result = rt.processProxyRequest(std::move(req), rl_key);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error.http_status, 429);
}
