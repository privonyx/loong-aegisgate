// TASK-20260708-02 / REV20260707-C1 Epic 3 — SR-1 predicate contract.
//
// The aegisgate::shouldWireRedisRateLimiter(config, cluster_gate_enabled)
// helper is the single source of truth deciding whether the assembler
// should try to wire a RedisStateStore into the RateLimiter. Its truth
// table (rows below) implements Option A from creative:
//   1. rate_limit.backend == "redis"                     -> true
//   2. rate_limiter.backend == "redis" (legacy alias)    -> true
//   3. deployment.mode == "cluster" AND cluster_gate_on  -> true
//   4. deployment.mode == "cluster" AND !cluster_gate_on -> false
//   5. all defaults (standalone, no backend key)         -> false
//   6. multiple triggers all true                        -> true (OR)
//   7. explicit "memory" on both keys                    -> false

#include <gtest/gtest.h>
#include <memory>

#include "core/config.h"
#include "server/gateway_runtime.h"

using namespace aegisgate;

namespace {

// Config is non-copyable / non-movable (owns a shared_mutex), so we hand
// it back via unique_ptr for the truth-table driver.
std::unique_ptr<Config> makeConfig(const std::string& yaml) {
    auto cfg = std::make_unique<Config>();
    EXPECT_TRUE(cfg->loadFromString(yaml));
    return cfg;
}

} // namespace

TEST(RedisRateLimiterWiringPredicate, EnabledByNewBackendKey) {
    auto cfg = makeConfig("rate_limit:\n  backend: redis\n");
    EXPECT_TRUE(shouldWireRedisRateLimiter(*cfg, /*cluster_gate_enabled=*/false));
}

TEST(RedisRateLimiterWiringPredicate, EnabledByLegacyBackendKey) {
    auto cfg = makeConfig("rate_limiter:\n  backend: redis\n");
    EXPECT_TRUE(shouldWireRedisRateLimiter(*cfg, /*cluster_gate_enabled=*/false));
}

TEST(RedisRateLimiterWiringPredicate, EnabledByClusterModeWithGateOn) {
    auto cfg = makeConfig("deployment:\n  mode: cluster\n");
    EXPECT_TRUE(shouldWireRedisRateLimiter(*cfg, /*cluster_gate_enabled=*/true));
}

TEST(RedisRateLimiterWiringPredicate, ClusterModeWithoutGateStaysDisabled) {
    auto cfg = makeConfig("deployment:\n  mode: cluster\n");
    // FeatureGate::ClusterDeployment off (community edition): the cluster
    // branch alone must NOT trigger Redis wiring. Preserves the historical
    // gate that pre-existed cluster wiring.
    EXPECT_FALSE(shouldWireRedisRateLimiter(*cfg, /*cluster_gate_enabled=*/false));
}

TEST(RedisRateLimiterWiringPredicate, DisabledByDefault) {
    auto cfg = makeConfig("server:\n  port: 8080\n");
    EXPECT_FALSE(shouldWireRedisRateLimiter(*cfg, /*cluster_gate_enabled=*/false));
    EXPECT_FALSE(shouldWireRedisRateLimiter(*cfg, /*cluster_gate_enabled=*/true));
}

TEST(RedisRateLimiterWiringPredicate, MultipleTriggersAllHonored) {
    auto cfg = makeConfig(
        "rate_limit:\n"
        "  backend: redis\n"
        "rate_limiter:\n"
        "  backend: redis\n"
        "deployment:\n"
        "  mode: cluster\n");
    EXPECT_TRUE(shouldWireRedisRateLimiter(*cfg, /*cluster_gate_enabled=*/true));
    // Even without the cluster gate, the backend-key triggers still fire.
    EXPECT_TRUE(shouldWireRedisRateLimiter(*cfg, /*cluster_gate_enabled=*/false));
}

TEST(RedisRateLimiterWiringPredicate, MemoryBackendKeyDoesNotEnable) {
    auto cfg = makeConfig(
        "rate_limit:\n"
        "  backend: memory\n"
        "rate_limiter:\n"
        "  backend: memory\n");
    EXPECT_FALSE(shouldWireRedisRateLimiter(*cfg, /*cluster_gate_enabled=*/false));
}
