// Phase 9.3.4 Epic D.1 — Config merged-yaml format parsing.
//
// Tests:
//   1. Legacy format (no `active_version_id` key) → loadFromString succeeds,
//      active_version_id == "inline", rolloutConfig == nullopt
//   2. New format with active_version_id + configs map → correct parsing
//   3. New format + rollout section → correct RolloutConfigView
//   4. New format missing configs key → loadFromString still succeeds (graceful)
//   5. Existing config getters still work through the "inline" path
//   6. Existing config getters work through the "configs" multi-version path

#include "core/config.h"
#include "core/rollout_config.h"

#include <gtest/gtest.h>
#include <string>

namespace aegisgate {
namespace {

const char* kLegacyYaml = R"yaml(
server:
  port: 8080
  threads: 4
auth:
  enabled: false
)yaml";

const char* kMergedYamlNoRollout = R"yaml(
active_version_id: "01VER_ACTIVE"
configs:
  "01VER_ACTIVE":
    server:
      port: 9090
      threads: 2
    auth:
      enabled: true
  "01VER_OLD":
    server:
      port: 8080
)yaml";

const char* kMergedYamlWithRollout = R"yaml(
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
        - "tenant-b"
      regions:
        - "us-east-1"
      percentage: 10
configs:
  "01VER_OLD":
    server:
      port: 8080
  "01VER_NEW":
    server:
      port: 9090
)yaml";

// ---- Legacy (backward-compatible) -----------------------------------------

TEST(ConfigRollout, LegacyFormatLoadsSuccessfully) {
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(kLegacyYaml));
    EXPECT_EQ(cfg.activeVersionId(), "inline");
    EXPECT_FALSE(cfg.rolloutConfig().has_value());
}

TEST(ConfigRollout, LegacyFormatGettersWork) {
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(kLegacyYaml));
    EXPECT_EQ(cfg.serverPort(), 8080);
    EXPECT_EQ(cfg.serverThreads(), 4);
    EXPECT_FALSE(cfg.authEnabled());
}

// ---- Merged format without rollout ----------------------------------------

TEST(ConfigRollout, MergedNoRolloutParsesActiveVersionId) {
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(kMergedYamlNoRollout));
    EXPECT_EQ(cfg.activeVersionId(), "01VER_ACTIVE");
    EXPECT_FALSE(cfg.rolloutConfig().has_value());
}

TEST(ConfigRollout, MergedNoRolloutGettersFromActiveConfig) {
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(kMergedYamlNoRollout));
    EXPECT_EQ(cfg.serverPort(), 9090);
    EXPECT_EQ(cfg.serverThreads(), 2);
    EXPECT_TRUE(cfg.authEnabled());
}

// ---- Merged format with rollout -------------------------------------------

TEST(ConfigRollout, MergedWithRolloutParsesRolloutSection) {
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(kMergedYamlWithRollout));
    EXPECT_EQ(cfg.activeVersionId(), "01VER_OLD");

    auto rc = cfg.rolloutConfig();
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(rc->rollout_id, "01RL_001");
    EXPECT_EQ(rc->target_version_id, "01VER_NEW");
    EXPECT_EQ(rc->sticky_key, "tenant_id");
    EXPECT_EQ(rc->current_stage.stage_index, 0);
    EXPECT_EQ(rc->current_stage.name, "canary");
    ASSERT_EQ(rc->current_stage.scope.tenant_globs.size(), 2u);
    EXPECT_EQ(rc->current_stage.scope.tenant_globs[0], "tenant-a*");
    EXPECT_EQ(rc->current_stage.scope.tenant_globs[1], "tenant-b");
    ASSERT_EQ(rc->current_stage.scope.regions.size(), 1u);
    EXPECT_EQ(rc->current_stage.scope.regions[0], "us-east-1");
    EXPECT_EQ(rc->current_stage.scope.percentage, 10);
}

TEST(ConfigRollout, MergedWithRolloutGettersFromActiveConfig) {
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(kMergedYamlWithRollout));
    EXPECT_EQ(cfg.serverPort(), 8080);
}

TEST(ConfigRollout, MergedWithRolloutCanResolveTargetConfig) {
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(kMergedYamlWithRollout));
    auto target = cfg.configForVersion("01VER_NEW");
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->serverPort(), 9090);
}

TEST(ConfigRollout, ConfigForVersionReturnsNullptrIfMissing) {
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(kMergedYamlWithRollout));
    EXPECT_EQ(cfg.configForVersion("01VER_NONEXISTENT"), nullptr);
}

TEST(ConfigRollout, ConfigForVersionOnLegacyReturnsNullptr) {
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(kLegacyYaml));
    EXPECT_EQ(cfg.configForVersion("anything"), nullptr);
}

// ---- Edge cases -----------------------------------------------------------

TEST(ConfigRollout, MergedFormatEmptyConfigsMapStillLoads) {
    const char* yaml = R"yaml(
active_version_id: "01VER_X"
configs: {}
)yaml";
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(yaml));
    EXPECT_EQ(cfg.activeVersionId(), "01VER_X");
    EXPECT_FALSE(cfg.rolloutConfig().has_value());
}

TEST(ConfigRollout, MergedFormatNoConfigsKeyStillLoads) {
    const char* yaml = R"yaml(
active_version_id: "01VER_X"
)yaml";
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(yaml));
    EXPECT_EQ(cfg.activeVersionId(), "01VER_X");
}

TEST(ConfigRollout, RolloutSectionWithoutScopeDefaultsGracefully) {
    const char* yaml = R"yaml(
active_version_id: "01VER_OLD"
rollout:
  rollout_id: "01RL_SIMPLE"
  target_version_id: "01VER_NEW"
  sticky_key: "user_id"
  current_stage:
    stage_index: 1
    name: "full"
configs:
  "01VER_OLD":
    server:
      port: 80
)yaml";
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString(yaml));
    auto rc = cfg.rolloutConfig();
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(rc->current_stage.stage_index, 1);
    EXPECT_EQ(rc->current_stage.name, "full");
    EXPECT_TRUE(rc->current_stage.scope.tenant_globs.empty());
    EXPECT_TRUE(rc->current_stage.scope.regions.empty());
    EXPECT_EQ(rc->current_stage.scope.percentage, 0);
}

}  // namespace
}  // namespace aegisgate
