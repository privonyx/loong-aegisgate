#include <gtest/gtest.h>
#include "core/config.h"
#include "core/crypto.h"
#include "core/feature_gate.h"
#include "server/response_finalizer.h"
#include "cache/hnsw_vector_store.h"
#include "cache/embedder.h"
#include <chrono>

using namespace aegisgate;
using namespace std::chrono_literals;

// --- Auth logic tests ---
// We test the auth contract without instantiating GatewayRuntime,
// which requires Drogon's event loop. The validateApiKey logic matches
// GatewayRuntime: SHA-256 hash of the presented key vs stored key hashes.

namespace {

bool simulateValidateApiKey(const Config& config, const std::string& key) {
    if (!config.authEnabled()) return true;
    auto keys = config.authApiKeys();
    if (keys.empty()) return false;
    auto hash = aegisgate::crypto::sha256(key);
    for (const auto& k : keys) {
        auto stored_hash = aegisgate::crypto::sha256(k);
        if (aegisgate::crypto::constantTimeEquals(hash, stored_hash)) return true;
    }
    return false;
}

} // namespace

TEST(GatewayAuthTest, AuthDisabledAllowsAnyKey) {
    Config config;
    // Not loaded → authEnabled() == false
    EXPECT_TRUE(simulateValidateApiKey(config, ""));
    EXPECT_TRUE(simulateValidateApiKey(config, "any-key"));
}

TEST(GatewayAuthTest, AuthEnabledEmptyKeysRejectsAll) {
    Config config;
    config.loadFromFile("config/aegisgate.yaml");
    // Config has auth.enabled=true but ${AEGISGATE_API_KEY} is not set → empty
    EXPECT_TRUE(config.authEnabled());
    EXPECT_TRUE(config.authApiKeys().empty());

    EXPECT_FALSE(simulateValidateApiKey(config, ""));
    EXPECT_FALSE(simulateValidateApiKey(config, "any-key"));
    EXPECT_FALSE(simulateValidateApiKey(config, "sk-test"));
}

TEST(GatewayAuthTest, AuthDisabledWhenConfigNotLoaded) {
    Config config;
    EXPECT_FALSE(config.isLoaded());
    EXPECT_FALSE(config.authEnabled());
    EXPECT_TRUE(simulateValidateApiKey(config, "anything"));
}

// --- Config load failure ---

TEST(GatewayAuthTest, ConfigLoadFailureReturnsFalse) {
    Config config;
    EXPECT_FALSE(config.loadFromFile("nonexistent.yaml"));
    EXPECT_FALSE(config.isLoaded());
    // Auth disabled when not loaded → all requests pass (but main should exit)
    EXPECT_FALSE(config.authEnabled());
}

TEST(GatewayAuthTest, ConfigLoadSuccessReturnsTrue) {
    Config config;
    EXPECT_TRUE(config.loadFromFile("config/aegisgate.yaml"));
    EXPECT_TRUE(config.isLoaded());
}

// --- Feature Gate license ---

TEST(GatewayAuthTest, FeatureGateEnterpriseRequiresLicense) {
    FeatureGate gate(Edition::Enterprise);
    // Without loading a license, enterprise should be disabled
    EXPECT_FALSE(gate.isEnterprise());
    EXPECT_FALSE(gate.isEnabled(Feature::CustomRules));
}

TEST(GatewayAuthTest, FeatureGateCommunityNoLicenseNeeded) {
    FeatureGate gate(Edition::Community);
    EXPECT_TRUE(gate.loadLicense(""));
    EXPECT_FALSE(gate.isEnterprise());
}

TEST(GatewayAuthTest, FeatureGateUnlockedForTesting) {
    auto gate = FeatureGate::createUnlocked(Edition::Enterprise);
    EXPECT_TRUE(gate.isEnterprise());
    EXPECT_TRUE(gate.isEnabled(Feature::CustomRules));
    EXPECT_TRUE(gate.isEnabled(Feature::Alerting));
}

// --- License path in config ---

TEST(GatewayAuthTest, ConfigLicensePathDefault) {
    Config config;
    EXPECT_EQ(config.licensePath(), "");
}

TEST(GatewayAuthTest, ConfigLicensePathFromYaml) {
    Config config;
    config.loadFromFile("config/aegisgate.yaml");
    // aegisgate.yaml has license_file: ""
    EXPECT_EQ(config.licensePath(), "");
}

// --- GAP-015: Gateway request timeout config ---

TEST(GatewayConfigTest, RequestTimeoutDefaultIs120) {
    Config config;
    config.loadFromFile("config/aegisgate.yaml");
    EXPECT_GT(config.requestTimeoutSeconds(), 0);
    EXPECT_EQ(config.requestTimeoutSeconds(), 120);
}

TEST(GatewayConfigTest, RequestTimeoutDefaultWhenNotLoaded) {
    Config config;
    EXPECT_EQ(config.requestTimeoutSeconds(), 120);
}

// --- P0-3 / SR-3: non-streaming outbound redaction write-back + filtered cache ---
//
// Verifies the exact helper the gateway calls after the outbound pipeline:
// the redacted ctx.accumulated_response must overwrite the response body, and
// the cache must store the filtered content so cache hits stay redacted.

TEST(GatewaySR3Test, WriteBackReflectsRedactedAccumulatedResponse_SR3) {
    RequestContext ctx;
    ctx.target_model = "gpt-4";
    // Simulates ContentFilter having redacted the upstream content in-place.
    ctx.accumulated_response = "Here is the safe value: [REDACTED]";

    ChatResponse response;
    response.content = "Here is the safe value: token_LEAKEDSECRET1234567890";

    finalizeNonStreamingResponse(response, nullptr, ctx);

    EXPECT_EQ(response.content, "Here is the safe value: [REDACTED]");
    EXPECT_EQ(response.content.find("token_LEAKEDSECRET1234567890"),
              std::string::npos);
}

TEST(GatewaySR3Test, CachesFilteredContentSoHitsStayRedacted_SR3) {
    HashEmbedder embedder(32);
    HnswVectorStore store(32, 50000);
    store.initialize();
    SemanticCache cache(embedder, store, 0.99f, 3600s, 100);

    RequestContext ctx;
    ctx.tenant_id = "tenant-a";
    ctx.target_model = "gpt-4";
    ctx.chat_request.model = "gpt-4";
    ctx.chat_request.messages = {{"user", "show me the api key"}};
    ctx.accumulated_response = "The key is [REDACTED]";

    ChatResponse response;
    response.content = "The key is sk-RAWSECRET0123456789abcdef";

    finalizeNonStreamingResponse(response, &cache, ctx);

    // A subsequent identical request (same tenant) must hit cache and receive
    // the filtered content, never the raw upstream secret.
    RequestContext read_ctx;
    read_ctx.tenant_id = "tenant-a";
    read_ctx.chat_request.model = "gpt-4";
    read_ctx.chat_request.messages = {{"user", "show me the api key"}};

    EXPECT_EQ(cache.process(read_ctx), StageResult::ShortCircuit);
    EXPECT_TRUE(read_ctx.cache_hit);
    EXPECT_EQ(read_ctx.cached_response, "The key is [REDACTED]");
    EXPECT_EQ(read_ctx.cached_response.find("sk-RAWSECRET"), std::string::npos);
}
