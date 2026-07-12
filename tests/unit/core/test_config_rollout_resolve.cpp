// Phase 9.3.4 Epic D.2 — resolveActiveConfigId per-request scope matching.
//
// Tests:
//   - No rollout → returns active_version_id
//   - Rollout scope hit (tenant + region + percentage) → returns target_version_id
//   - Rollout scope miss (tenant mismatch) → returns active_version_id
//   - Rollout scope miss (region mismatch) → returns active_version_id
//   - Rollout scope miss (percentage bucket outside) → returns active_version_id
//   - 100% rollout → always returns target_version_id
//   - Empty scope → matches all (100% of traffic)

#include "core/config.h"
#include "core/rollout_resolve.h"

#include <gtest/gtest.h>
#include <string>

namespace aegisgate {
namespace {

const char* kMergedWithRollout = R"yaml(
active_version_id: "01VER_OLD"
rollout:
  rollout_id: "01RL_001"
  target_version_id: "01VER_NEW"
  sticky_key: "tenant_id"
  current_stage:
    stage_index: 0
    name: "canary"
    scope:
      tenant_globs:
        - "tenant-a*"
      regions:
        - "us-east-1"
      percentage: 100
configs:
  "01VER_OLD":
    server:
      port: 8080
  "01VER_NEW":
    server:
      port: 9090
)yaml";

const char* kMergedNoRollout = R"yaml(
active_version_id: "01VER_ACTIVE"
configs:
  "01VER_ACTIVE":
    server:
      port: 8080
)yaml";

const char* kLegacy = R"yaml(
server:
  port: 8080
)yaml";

TEST(ResolveActiveConfigId, NoRolloutReturnsActiveVersionId) {
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(kMergedNoRollout));
    auto result = resolveActiveConfigId(cfg, "tenant-x", "us-east-1", "tenant-x");
    EXPECT_EQ(result, "01VER_ACTIVE");
}

TEST(ResolveActiveConfigId, LegacyReturnsInline) {
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(kLegacy));
    auto result = resolveActiveConfigId(cfg, "tenant-a", "us-east-1", "tenant-a");
    EXPECT_EQ(result, "inline");
}

TEST(ResolveActiveConfigId, RolloutScopeHitReturnsTarget) {
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(kMergedWithRollout));
    auto result = resolveActiveConfigId(cfg, "tenant-alpha", "us-east-1", "tenant-alpha");
    EXPECT_EQ(result, "01VER_NEW");
}

TEST(ResolveActiveConfigId, TenantMismatchReturnsActive) {
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(kMergedWithRollout));
    auto result = resolveActiveConfigId(cfg, "tenant-xyz", "us-east-1", "tenant-xyz");
    EXPECT_EQ(result, "01VER_OLD");
}

TEST(ResolveActiveConfigId, RegionMismatchReturnsActive) {
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(kMergedWithRollout));
    auto result = resolveActiveConfigId(cfg, "tenant-alpha", "eu-west-1", "tenant-alpha");
    EXPECT_EQ(result, "01VER_OLD");
}

TEST(ResolveActiveConfigId, EmptyScopeMatchesAll) {
    const char* yaml = R"yaml(
active_version_id: "01VER_OLD"
rollout:
  rollout_id: "01RL_002"
  target_version_id: "01VER_NEW"
  sticky_key: "tenant_id"
  current_stage:
    stage_index: 0
    name: "full"
    scope:
      percentage: 100
configs:
  "01VER_OLD":
    server:
      port: 80
  "01VER_NEW":
    server:
      port: 90
)yaml";
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(yaml));
    auto result = resolveActiveConfigId(cfg, "anyone", "any-region", "anyone");
    EXPECT_EQ(result, "01VER_NEW");
}

TEST(ResolveActiveConfigId, PercentageBucketMiss) {
    const char* yaml = R"yaml(
active_version_id: "01VER_OLD"
rollout:
  rollout_id: "01RL_003"
  target_version_id: "01VER_NEW"
  sticky_key: "tenant_id"
  current_stage:
    stage_index: 0
    name: "canary"
    scope:
      percentage: 1
configs:
  "01VER_OLD":
    server:
      port: 80
)yaml";
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(yaml));
    // With 1% rollout, most sticky values should miss.
    // Use a known sticky value that hashes outside bucket 0.
    int hits = 0;
    for (int i = 0; i < 100; ++i) {
        auto sv = "tenant-" + std::to_string(i);
        auto result = resolveActiveConfigId(cfg, sv, "", sv);
        if (result == "01VER_NEW") ++hits;
    }
    // Statistically ~1% should hit (allow 0-5)
    EXPECT_LE(hits, 5);
    EXPECT_GE(hits, 0);
}

}  // namespace
}  // namespace aegisgate
