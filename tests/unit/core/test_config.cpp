#include <gtest/gtest.h>
#include "core/config.h"
#include <cstdlib>

using namespace aegisgate;

TEST(ConfigTest, DefaultValues) {
    Config config;
    EXPECT_FALSE(config.isLoaded());
    EXPECT_EQ(config.edition(), Edition::Community);
    EXPECT_EQ(config.serverPort(), 8080);
    EXPECT_EQ(config.serverHost(), "0.0.0.0");
    EXPECT_EQ(config.maxRequestBodySize(), 65536u);
    EXPECT_EQ(config.logLevel(), "info");
}

TEST(ConfigTest, LoadFromYaml) {
    Config config;
    EXPECT_TRUE(config.loadFromFile("config/aegisgate.yaml"));
    EXPECT_TRUE(config.isLoaded());
    EXPECT_EQ(config.edition(), Edition::Community);
    EXPECT_EQ(config.serverPort(), 8080);
    EXPECT_EQ(config.serverHost(), "0.0.0.0");
}

TEST(ConfigTest, LoadNonExistentFileReturnsFalse) {
    Config config;
    EXPECT_FALSE(config.loadFromFile("nonexistent.yaml"));
    EXPECT_FALSE(config.isLoaded());
    EXPECT_EQ(config.edition(), Edition::Community);
    EXPECT_EQ(config.serverPort(), 8080);
}

TEST(ConfigTest, AuthDisabledByDefault) {
    Config config;
    EXPECT_FALSE(config.authEnabled());
    EXPECT_TRUE(config.authApiKeys().empty());
}

TEST(ConfigTest, StorageDefaults) {
    Config config;
    EXPECT_EQ(config.cacheBackend(), "memory");
    EXPECT_EQ(config.persistentBackend(), "sqlite");
    EXPECT_EQ(config.sqlitePath(), "data/aegisgate.db");
    EXPECT_TRUE(config.sqliteWalMode());
}

// TASK-20260617-02: dashboard persistence defaults — on, 30-day reload window.
TEST(ConfigTest, DashboardPersistenceDefaults) {
    Config config;
    EXPECT_TRUE(config.dashboardPersistenceEnabled());
    EXPECT_EQ(config.dashboardReloadDays(), 30);
}

// Regression: a loaded config WITHOUT a `dashboard:` section must return
// defaults, not throw. Chained const Node access on a missing parent threw
// YAML EnsureNodeExists and crashed gateway startup for every existing config.
TEST(ConfigTest, DashboardPersistenceMissingSectionUsesDefaults) {
    Config config;
    ASSERT_TRUE(config.loadFromFile("config/aegisgate.yaml"));
    EXPECT_NO_THROW({
        EXPECT_TRUE(config.dashboardPersistenceEnabled());
        EXPECT_EQ(config.dashboardReloadDays(), 30);
    });
}

TEST(ConfigTest, StorageFromYaml) {
    Config config;
    ASSERT_TRUE(config.loadFromFile("config/aegisgate.yaml"));
    auto backend = config.persistentBackend();
    EXPECT_TRUE(backend == "sqlite" || backend == "memory");
}

TEST(ConfigTest, SqlitePathEnvOverride) {
    unsetenv("AEGISGATE_SQLITE_PATH");  // ensure clean state
    Config config;
    ASSERT_TRUE(config.loadFromFile("config/aegisgate.yaml"));
    std::string default_path = config.sqlitePath();

    ASSERT_EQ(0, setenv("AEGISGATE_SQLITE_PATH", "/tmp/override.db", 1));
    EXPECT_EQ(config.sqlitePath(), "/tmp/override.db");
    unsetenv("AEGISGATE_SQLITE_PATH");
    EXPECT_EQ(config.sqlitePath(), default_path);
}

TEST(ConfigTest, TelemetryConfigEnvOverride) {
    setenv("AEGISGATE_TELEMETRY_ENABLED", "1", 1);
    setenv("AEGISGATE_OTLP_ENDPOINT", "http://custom:4318", 1);
    setenv("AEGISGATE_TELEMETRY_SERVICE_NAME", "my-service", 1);
    setenv("AEGISGATE_TELEMETRY_SAMPLE_RATIO", "0.5", 1);

    Config config;
    ASSERT_TRUE(config.loadFromFile("config/aegisgate.yaml"));
    auto tc = config.telemetryConfig();
    EXPECT_TRUE(tc.enabled);
    EXPECT_EQ(tc.otlp_endpoint, "http://custom:4318");
    EXPECT_EQ(tc.service_name, "my-service");
    EXPECT_DOUBLE_EQ(tc.sample_ratio, 0.5);

    unsetenv("AEGISGATE_TELEMETRY_ENABLED");
    unsetenv("AEGISGATE_OTLP_ENDPOINT");
    unsetenv("AEGISGATE_TELEMETRY_SERVICE_NAME");
    unsetenv("AEGISGATE_TELEMETRY_SAMPLE_RATIO");
}

TEST(ConfigTest, SsoDefaults) {
    Config config;
    EXPECT_FALSE(config.ssoEnabled());
    EXPECT_TRUE(config.ssoDefaultProvider().empty());
}

TEST(ConfigTest, MfaDefaults) {
    Config config;
    EXPECT_EQ(config.mfaEnforcement(), "disabled");
    EXPECT_EQ(config.mfaTotpDigits(), 6);
    EXPECT_EQ(config.mfaTotpPeriod(), 30);
    EXPECT_EQ(config.mfaRecoveryCodeCount(), 8);
}

TEST(ConfigTest, SessionDefaults) {
    Config config;
    EXPECT_EQ(config.sessionAbsoluteTimeoutSeconds(), 28800);
    EXPECT_EQ(config.sessionIdleTimeoutSeconds(), 3600);
    EXPECT_EQ(config.sessionMaxConcurrent(), 5);
}

TEST(ConfigTest, AdminAllowedIpsDefaultEmpty) {
    Config config;
    EXPECT_TRUE(config.adminAllowedIps().empty());
}

TEST(ConfigTest, AdminAllowedIpsFromYaml) {
    Config config;
    ASSERT_TRUE(config.loadFromFile("config/aegisgate.yaml"));
    auto ips = config.adminAllowedIps();
    EXPECT_TRUE(ips.empty());
}

TEST(ConfigTest, AdminTrustedProxiesDefaultEmpty) {
    Config config;
    EXPECT_TRUE(config.adminTrustedProxies().empty());
}

TEST(ConfigTest, RedisPasswordAndPgUrlEnvExpansion) {
    ASSERT_EQ(0, setenv("REDIS_PASSWORD", "secret-redis", 1));
    ASSERT_EQ(0, setenv("POSTGRES_URL", "postgres://u:p@db:5432/aegisgate", 1));
    Config config;
    ASSERT_TRUE(config.loadFromFile("tests/fixtures/config_storage_env_test.yaml"));
    EXPECT_EQ(config.redisPassword(), "secret-redis");
    EXPECT_EQ(config.pgUrl(), "postgres://u:p@db:5432/aegisgate");
    unsetenv("REDIS_PASSWORD");
    unsetenv("POSTGRES_URL");
}

// --- Phase 11.5 Autonomy / BudgetGuard defaults (TASK-20260518-02 E4.1) ---
// Verify each new getter is SAFE-by-default before yaml load (so a fresh
// Config{} never accidentally runs an applier) and after loading the
// shipped aegisgate.yaml (which keeps every flag off but defines the
// values explicitly so operators have something to copy/paste).

TEST(ConfigTest, AutonomyDefaultsBeforeLoad) {
    Config config;
    EXPECT_FALSE(config.autonomyEnabled());
    EXPECT_EQ(config.autonomyAutoApplyMode(), "manual_only");
    EXPECT_EQ(config.autonomyProposalRetentionDays(), 90);
    EXPECT_FALSE(config.costAutonomyEnabled());
}

TEST(ConfigTest, BudgetGuardDefaultsBeforeLoad) {
    Config config;
    EXPECT_FALSE(config.budgetGuardEnabled());
    EXPECT_DOUBLE_EQ(config.budgetGuardPerTenant24hUsd(), 100.0);
    EXPECT_DOUBLE_EQ(config.budgetGuardPerRequestMaxUsd(), 1.0);
    EXPECT_TRUE(config.budgetGuardFailOpenOnError());
    EXPECT_EQ(config.budgetGuardDowngradeTier(), "economy");
    EXPECT_EQ(config.budgetGuardDowngradeHeaderName(),
              "X-AegisGate-Budget-Guard");
    EXPECT_EQ(config.budgetGuardDowngradeHeaderValue(), "triggered");
}

TEST(ConfigTest, AutonomyAndBudgetGuardLoadFromYaml) {
    Config config;
    ASSERT_TRUE(config.loadFromFile("config/aegisgate.yaml"));
    // Shipped yaml keeps everything OFF; the values must mirror the
    // documented defaults so operators have a copy/paste baseline.
    EXPECT_FALSE(config.autonomyEnabled());
    EXPECT_EQ(config.autonomyAutoApplyMode(), "manual_only");
    EXPECT_EQ(config.autonomyProposalRetentionDays(), 90);
    EXPECT_FALSE(config.costAutonomyEnabled());

    EXPECT_FALSE(config.budgetGuardEnabled());
    EXPECT_DOUBLE_EQ(config.budgetGuardPerTenant24hUsd(), 100.0);
    EXPECT_DOUBLE_EQ(config.budgetGuardPerRequestMaxUsd(), 1.0);
    EXPECT_TRUE(config.budgetGuardFailOpenOnError());
    EXPECT_EQ(config.budgetGuardDowngradeTier(), "economy");
}

TEST(ConfigTest, AutonomyOverridesFromYamlString) {
    Config config;
    const std::string yaml = R"(
autonomy:
  enabled: true
  auto_apply_mode: auto_low_risk
  proposal_retention_days: 30
  cost_optimizer:
    enabled: true
budget_guard:
  enabled: true
  per_tenant_24h_usd: 250.5
  per_request_max_usd: 2.5
  fail_open_on_error: false
  downgrade_tier: standard
  downgrade_header_name: X-Custom-Guard
  downgrade_header_value: capped
)";
    ASSERT_TRUE(config.loadFromString(yaml));

    EXPECT_TRUE(config.autonomyEnabled());
    EXPECT_EQ(config.autonomyAutoApplyMode(), "auto_low_risk");
    EXPECT_EQ(config.autonomyProposalRetentionDays(), 30);
    EXPECT_TRUE(config.costAutonomyEnabled());

    EXPECT_TRUE(config.budgetGuardEnabled());
    EXPECT_DOUBLE_EQ(config.budgetGuardPerTenant24hUsd(), 250.5);
    EXPECT_DOUBLE_EQ(config.budgetGuardPerRequestMaxUsd(), 2.5);
    EXPECT_FALSE(config.budgetGuardFailOpenOnError());
    EXPECT_EQ(config.budgetGuardDowngradeTier(), "standard");
    EXPECT_EQ(config.budgetGuardDowngradeHeaderName(), "X-Custom-Guard");
    EXPECT_EQ(config.budgetGuardDowngradeHeaderValue(), "capped");
}

TEST(ConfigTest, AutonomyAutoApplyModeUnknownFallsBackToManualOnly) {
    Config config;
    const std::string yaml = R"(
autonomy:
  enabled: true
  auto_apply_mode: bogus_value
)";
    ASSERT_TRUE(config.loadFromString(yaml));
    // Unknown mode must collapse to the conservative default rather
    // than letting a typo silently grant blanket auto-apply.
    EXPECT_EQ(config.autonomyAutoApplyMode(), "manual_only");
}

TEST(ConfigTest, BudgetGuardRejectsNonPositiveLimits) {
    Config config;
    const std::string yaml = R"(
budget_guard:
  enabled: true
  per_tenant_24h_usd: -10.0
  per_request_max_usd: 0.0
)";
    ASSERT_TRUE(config.loadFromString(yaml));
    // Negative / zero caps would defeat the guard (everything trips).
    // Getters must clamp back to the documented defaults to keep the
    // stage usable even with a misconfigured yaml.
    EXPECT_DOUBLE_EQ(config.budgetGuardPerTenant24hUsd(), 100.0);
    EXPECT_DOUBLE_EQ(config.budgetGuardPerRequestMaxUsd(), 1.0);
}

TEST(ConfigTest, ObservabilityConfigDefaults) {
    Config config;
    EXPECT_FALSE(config.costAttributionEnabled());
    EXPECT_FALSE(config.anomalyDetectionEnabled());
    EXPECT_DOUBLE_EQ(config.anomalyDetectionZScoreThreshold(), 3.0);
    EXPECT_EQ(config.anomalyDetectionWindowSize(), 100);
    EXPECT_FALSE(config.qualityMonitoringEnabled());
    EXPECT_DOUBLE_EQ(config.qualityMonitoringAlertThreshold(), 0.3);
    EXPECT_FALSE(config.costOptimizationEnabled());
}

TEST(ConfigTest, ObservabilityConfigMissingSectionUsesDefaults) {
    Config config;
    ASSERT_TRUE(config.loadFromString("server:\n  port: 8080\n"));
    EXPECT_FALSE(config.costAttributionEnabled());
    EXPECT_FALSE(config.anomalyDetectionEnabled());
    EXPECT_DOUBLE_EQ(config.anomalyDetectionZScoreThreshold(), 3.0);
    EXPECT_EQ(config.anomalyDetectionWindowSize(), 100);
    EXPECT_FALSE(config.qualityMonitoringEnabled());
    EXPECT_DOUBLE_EQ(config.qualityMonitoringAlertThreshold(), 0.3);
    EXPECT_FALSE(config.costOptimizationEnabled());
}

TEST(ConfigTest, ObservabilityConfigLoadsFromYaml) {
    Config config;
    ASSERT_TRUE(config.loadFromFile("config/aegisgate.yaml"));
    EXPECT_FALSE(config.costAttributionEnabled());
    EXPECT_FALSE(config.anomalyDetectionEnabled());
    EXPECT_DOUBLE_EQ(config.anomalyDetectionZScoreThreshold(), 3.0);
    EXPECT_EQ(config.anomalyDetectionWindowSize(), 100);
    EXPECT_FALSE(config.qualityMonitoringEnabled());
    EXPECT_DOUBLE_EQ(config.qualityMonitoringAlertThreshold(), 0.3);
    EXPECT_FALSE(config.costOptimizationEnabled());
}

TEST(ConfigTest, ObservabilityConfigOverridesFromYamlString) {
    Config config;
    const std::string yaml = R"(
observability:
  cost_attribution:
    enabled: true
  anomaly_detection:
    enabled: true
    z_score_threshold: 1.25
    window_size: 7
  quality_monitoring:
    enabled: true
    alert_threshold: 0.42
  cost_optimization:
    enabled: true
)";
    ASSERT_TRUE(config.loadFromString(yaml));
    EXPECT_TRUE(config.costAttributionEnabled());
    EXPECT_TRUE(config.anomalyDetectionEnabled());
    EXPECT_DOUBLE_EQ(config.anomalyDetectionZScoreThreshold(), 1.25);
    EXPECT_EQ(config.anomalyDetectionWindowSize(), 7);
    EXPECT_TRUE(config.qualityMonitoringEnabled());
    EXPECT_DOUBLE_EQ(config.qualityMonitoringAlertThreshold(), 0.42);
    EXPECT_TRUE(config.costOptimizationEnabled());
}

// TASK-20260708-02 / REV20260707-C1 — Redis rate limiter wiring.
// The user-facing `rate_limit.*` YAML namespace should be the preferred
// config space (per feature-list.md:59 "Config: rate_limit.*") while the
// legacy `rate_limiter.backend` key remains honored for back-compat.
// Precedence: rate_limit.backend > rate_limiter.backend > "memory".
TEST(ConfigTest, RateLimitBackendPrefersRateLimitNamespace) {
    Config config;
    const std::string yaml = R"(
rate_limit:
  backend: redis
)";
    ASSERT_TRUE(config.loadFromString(yaml));
    // New alias getter reads the user-facing namespace.
    EXPECT_EQ(config.rateLimitBackend(), "redis");
    // Legacy getter still returns its own key ("memory" default because
    // the legacy rate_limiter.backend was not set).
    EXPECT_EQ(config.rateLimiterBackend(), "memory");
}

TEST(ConfigTest, RateLimitBackendFallsBackToLegacyKey) {
    Config config;
    const std::string yaml = R"(
rate_limiter:
  backend: redis
)";
    ASSERT_TRUE(config.loadFromString(yaml));
    // Alias getter falls back to the legacy key when the new one is absent.
    EXPECT_EQ(config.rateLimitBackend(), "redis");
    // Legacy getter unchanged.
    EXPECT_EQ(config.rateLimiterBackend(), "redis");
}

TEST(ConfigTest, RateLimitBackendDefaultsToMemory) {
    Config config;
    ASSERT_TRUE(config.loadFromString("server:\n  port: 8080\n"));
    // Neither key set: default is "memory".
    EXPECT_EQ(config.rateLimitBackend(), "memory");
    EXPECT_EQ(config.rateLimiterBackend(), "memory");
}

TEST(ConfigTest, RateLimitBackendNewKeyWinsOverLegacy) {
    Config config;
    const std::string yaml = R"(
rate_limit:
  backend: redis
rate_limiter:
  backend: memory
)";
    ASSERT_TRUE(config.loadFromString(yaml));
    // Precedence: rate_limit.backend > rate_limiter.backend.
    EXPECT_EQ(config.rateLimitBackend(), "redis");
    EXPECT_EQ(config.rateLimiterBackend(), "memory");
}

TEST(ConfigTest, CircuitBreakerDefaultsBeforeLoad) {
    Config config;
    EXPECT_EQ(config.circuitFailureThreshold(), 3);
    EXPECT_EQ(config.circuitResetTimeoutSeconds(), 30);
    EXPECT_EQ(config.circuitHalfOpenMaxCalls(), 1);
    EXPECT_EQ(config.circuitMaxCircuits(), 512);
    EXPECT_EQ(config.circuitIdleTtlSeconds(), 0);
}

TEST(ConfigTest, CircuitBreakerLoadFromYamlString) {
    Config config;
    const std::string yaml = R"(
circuit_breaker:
  failure_threshold: 7
  reset_timeout_seconds: 45
  half_open_max_calls: 2
  max_circuits: 256
  circuit_idle_ttl_seconds: 120
)";
    ASSERT_TRUE(config.loadFromString(yaml));
    EXPECT_EQ(config.circuitFailureThreshold(), 7);
    EXPECT_EQ(config.circuitResetTimeoutSeconds(), 45);
    EXPECT_EQ(config.circuitHalfOpenMaxCalls(), 2);
    EXPECT_EQ(config.circuitMaxCircuits(), 256);
    EXPECT_EQ(config.circuitIdleTtlSeconds(), 120);
}
