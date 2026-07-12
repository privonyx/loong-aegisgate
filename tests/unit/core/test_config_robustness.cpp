#include <gtest/gtest.h>

#include "core/config.h"

using namespace aegisgate;

namespace {

void loadMinimalConfig(Config& config, const std::string& extra_yaml = "") {
    const std::string yaml = std::string("edition: community\n") + extra_yaml;
    EXPECT_TRUE(config.loadFromString(yaml));
}

} // namespace

TEST(ConfigRobustnessTest, LoggingSectionMissingReturnsDefaults) {
    Config config;
    loadMinimalConfig(config, "server:\n  port: 8080\n");

    EXPECT_NO_THROW({
        EXPECT_EQ(config.logLevel(), "info");
        EXPECT_EQ(config.logFile(), "");
    });
}

TEST(ConfigRobustnessTest, StorageSectionMissingReturnsDefaults) {
    Config config;
    loadMinimalConfig(config, "logging:\n  level: info\n");

    EXPECT_NO_THROW({
        EXPECT_EQ(config.cacheBackend(), "memory");
        EXPECT_EQ(config.persistentBackend(), "sqlite");
        EXPECT_EQ(config.sqlitePath(), "data/aegisgate.db");
        EXPECT_TRUE(config.sqliteWalMode());
        EXPECT_TRUE(config.strictBackends());
    });
}

TEST(ConfigRobustnessTest, StorageMissingSqliteSubsectionReturnsDefaults) {
    Config config;
    loadMinimalConfig(config, R"(
storage:
  persistent_backend: sqlite
  cache_backend: memory
)");

    EXPECT_NO_THROW({
        EXPECT_EQ(config.sqlitePath(), "data/aegisgate.db");
        EXPECT_TRUE(config.sqliteWalMode());
    });
}

TEST(ConfigRobustnessTest, StartupSequenceMinimalConfigNoThrow) {
    Config config;
    loadMinimalConfig(config, "server:\n  port: 8080\n");

    EXPECT_NO_THROW({
        (void)config.persistentBackend();
        (void)config.sqlitePath();
        (void)config.sqliteWalMode();
        (void)config.logLevel();
        (void)config.logFile();
    });
}

TEST(ConfigRobustnessTest, TlsSectionMissingReturnsDefaults) {
    Config config;
    loadMinimalConfig(config, "server:\n  port: 8080\n");

    EXPECT_NO_THROW({
        EXPECT_FALSE(config.tlsEnabled());
        EXPECT_EQ(config.tlsPort(), 0);
        EXPECT_EQ(config.tlsCertPath(), "");
        EXPECT_EQ(config.tlsKeyPath(), "");
    });
}

TEST(ConfigRobustnessTest, SecuritySectionMissingGuardDefaults) {
    Config config;
    loadMinimalConfig(config, "server:\n  port: 8080\n");

    EXPECT_NO_THROW({
        EXPECT_TRUE(config.unicodeNormalizationEnabled());
        EXPECT_TRUE(config.encodingDetectionEnabled());
        EXPECT_EQ(config.encodingMinBase64Length(), 20);
        EXPECT_FALSE(config.guardModelEnabled());
        EXPECT_EQ(config.guardModelPath(), "");
        EXPECT_EQ(config.guardModelVocabPath(), "");
        EXPECT_EQ(config.guardModelSpmPath(), "");
        EXPECT_EQ(config.guardModelFailPolicy(), "open");
        EXPECT_DOUBLE_EQ(config.guardModelThreshold(), 0.5);
    });
}

TEST(ConfigRobustnessTest, CacheSectionMissingReturnsDefaults) {
    Config config;
    loadMinimalConfig(config, "server:\n  port: 8080\n");

    EXPECT_NO_THROW({
        EXPECT_FLOAT_EQ(config.cacheThreshold(), 0.95f);
        EXPECT_EQ(config.cacheTtlSeconds(), 3600);
        EXPECT_EQ(config.cacheMaxEntries(), 10000);
        EXPECT_EQ(config.cacheMaxPartitions(), 64);
        EXPECT_TRUE(config.cacheContextAware());
        EXPECT_EQ(config.cacheWarmupFile(), "");
        EXPECT_FALSE(config.conversationCacheEnabled());
        EXPECT_EQ(config.conversationSummarizerType(), "rule_based");
        EXPECT_EQ(config.conversationSummarizerOnnxModelPath(), "");
        EXPECT_EQ(config.conversationSummarizerMaxSummaryMs(), 200);
        EXPECT_EQ(config.conversationSummarizerMaxInputTokens(), 4096);
        EXPECT_TRUE(config.conversationIdResolverEnabled());
        EXPECT_FALSE(config.cacheAdaptiveEnabled());
        EXPECT_FLOAT_EQ(config.cacheAdaptiveMinThreshold(), 0.85f);
        EXPECT_FLOAT_EQ(config.cacheAdaptiveMaxThreshold(), 0.98f);
        EXPECT_FLOAT_EQ(config.cacheAdaptiveAdjustmentRate(), 0.01f);
        EXPECT_EQ(config.cacheAdaptiveWindowSize(), 100);
        EXPECT_TRUE(config.cachePolicyEnabled());
        EXPECT_TRUE(config.cachePolicySkipModels().empty());
        EXPECT_DOUBLE_EQ(config.cachePolicyMaxTemperature(), 1.0);
        EXPECT_FALSE(config.cachePolicySkipStreaming());
    });
}

TEST(ConfigRobustnessTest, AbuseDetectionMissingSectionReturnsDefaults) {
    Config config;
    loadMinimalConfig(config, "security:\n  unicode_normalization: true\n");

    EXPECT_NO_THROW({
        EXPECT_TRUE(config.abuseDetectionEnabled());
        EXPECT_EQ(config.abuseWindowSeconds(), 300);
        EXPECT_EQ(config.abuseWarnThreshold(), 5);
        EXPECT_EQ(config.abuseThrottleThreshold(), 10);
        EXPECT_EQ(config.abuseBlockThreshold(), 20);
        EXPECT_EQ(config.abuseBlockDurationSeconds(), 1800);
    });
}

TEST(ConfigRobustnessTest, MultimodalSubsectionsMissingNoThrow) {
    Config config;
    loadMinimalConfig(config, R"(
multimodal:
  enabled: true
)");

    EXPECT_NO_THROW({
        EXPECT_TRUE(config.multimodalEnabled());
        EXPECT_EQ(config.multimodalRoutingPolicy(), "cheapest");
        EXPECT_TRUE(config.multimodalCostAttributionEnabled());
        EXPECT_FALSE(config.multimodalRateLimitEnabled());
        EXPECT_TRUE(config.multimodalRateLimitQuotas().empty());
    });
}

// --- TASK-20260701-02: alerting.rules parsing (P0-G 残留闭环) ---

TEST(ConfigRobustnessTest, AlertRulesParsedFromConfig) {
    Config config;
    loadMinimalConfig(config, R"(
alerting:
  rules:
    - id: high_guardrail_blocks
      description: "Guardrail block rate exceeded"
      metric: aegisgate_guardrail_blocks_total
      threshold: 100.0
      severity: critical
      enabled: true
      cooldown_seconds: 60
    - id: too_many_fallbacks
      metric: fallback_total
      threshold: 5
)");

    auto rules = config.alertRules();
    ASSERT_EQ(rules.size(), 2u);

    EXPECT_EQ(rules[0].id, "high_guardrail_blocks");
    EXPECT_EQ(rules[0].description, "Guardrail block rate exceeded");
    EXPECT_EQ(rules[0].metric_name, "aegisgate_guardrail_blocks_total");
    EXPECT_DOUBLE_EQ(rules[0].threshold, 100.0);
    EXPECT_EQ(rules[0].severity, "critical");
    EXPECT_TRUE(rules[0].enabled);
    EXPECT_EQ(rules[0].cooldown_seconds, 60);

    // Second rule exercises defaults (severity=warning, enabled=true, cooldown=0).
    EXPECT_EQ(rules[1].id, "too_many_fallbacks");
    EXPECT_EQ(rules[1].metric_name, "fallback_total");
    EXPECT_DOUBLE_EQ(rules[1].threshold, 5.0);
    EXPECT_EQ(rules[1].severity, "warning");
    EXPECT_TRUE(rules[1].enabled);
    EXPECT_EQ(rules[1].cooldown_seconds, 0);
}

TEST(ConfigRobustnessTest, AlertRulesMissingSectionReturnsEmpty) {
    Config config;
    loadMinimalConfig(config, "server:\n  port: 8080\n");

    EXPECT_NO_THROW({
        EXPECT_TRUE(config.alertRules().empty());
    });
}

TEST(ConfigRobustnessTest, AlertRulesSkipsInvalidEntry) {
    Config config;
    // First entry missing metric, second missing id -> both skipped; third valid.
    loadMinimalConfig(config, R"(
alerting:
  rules:
    - id: no_metric
      threshold: 1
    - metric: requests_total
      threshold: 2
    - id: valid_rule
      metric: requests_total
      threshold: 3
)");

    auto rules = config.alertRules();
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_EQ(rules[0].id, "valid_rule");
}

