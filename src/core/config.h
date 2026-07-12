#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <cstddef>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <yaml-cpp/yaml.h>
#include "core/rollout_config.h"
#include "observe/tracing.h"

namespace aegisgate {

enum class Edition { Community, Enterprise };

class Config {
public:
    bool loadFromFile(const std::string& path);
    // Loads the config from an in-memory YAML string without touching the
    // filesystem. Used by the Phase 9.3 control plane to validate a submitted
    // ConfigBundle without exposing a `path` side-channel (SR4).
    bool loadFromString(const std::string& yaml_content);
    bool isLoaded() const;
    bool reload();

    Edition edition() const;
    int serverPort() const;
    std::string serverHost() const;
    int serverThreads() const;
    int requestTimeoutSeconds() const;
    size_t maxRequestBodySize() const;
    std::string logLevel() const;
    std::string logFile() const;
    bool featureEnabled(const std::string& feature) const;

    std::string modelsConfigPath() const;

    bool authEnabled() const;
    std::vector<std::string> authApiKeys() const;
    std::string adminApiKey() const;

    bool tlsEnabled() const;
    /// TLS listen port from config; 0 means unset (caller should use HTTP port + 1).
    int tlsPort() const;
    std::string tlsCertPath() const;
    std::string tlsKeyPath() const;

    std::string auditLogPath() const;
    std::string licensePath() const;

    double rateLimitMaxTokens() const;
    double rateLimitRefillRate() const;

    // Circuit breaker (FallbackManager / CircuitBreaker)
    int circuitFailureThreshold() const;
    int circuitResetTimeoutSeconds() const;
    int circuitHalfOpenMaxCalls() const;
    int circuitMaxCircuits() const;
    int circuitIdleTtlSeconds() const;

    std::string embeddingModelPath() const;
    std::string embeddingVocabPath() const;

    int auditRetentionDays() const;

    std::string cacheBackend() const;
    std::string persistentBackend() const;
    std::string sqlitePath() const;
    bool sqliteWalMode() const;
    // TASK-20260622-01 E1 (G1): 严格后端校验。默认 true —— 当 YAML 请求
    // redis/postgres 但二进制未编入对应后端、或后端启动时不可达（静默回退 memory）
    // 时 fail-closed 拒绝启动，杜绝「以为上了生产」假阳性。设 false 可降级为仅告警
    // （保留「后端故障时仍降级可用」的运维逃生阀）。env AEGISGATE_STRICT_BACKENDS=0|1 覆盖。
    bool strictBackends() const;  // 默认 true

    // TASK-20260617-02: 仪表盘数据持久化。启用后进程启动时回放近 N 天的
    // cost_records + savings_events，使重启不再丢失仪表盘聚合视图。
    bool dashboardPersistenceEnabled() const;  // 默认 true
    int dashboardReloadDays() const;           // 默认 30

    std::string redisHost() const;
    int redisPort() const;
    std::string redisPassword() const;
    int redisDb() const;
    int redisPoolSize() const;
    int redisConnectTimeout() const;
    int redisCommandTimeout() const;

    std::string pgUrl() const;
    int pgPoolSize() const;
    int pgConnectTimeout() const;

    // Security — InputPreprocessor
    bool unicodeNormalizationEnabled() const;
    bool encodingDetectionEnabled() const;
    int encodingMinBase64Length() const;
    // TASK-20260707-03 / REV20260707-N19: cap for decoding data: URI text
    // payloads in the vision image-reference scan surface (SR-4 DoS guard).
    size_t imageScanMaxDecodeBytes() const;

    // Security — InjectionDetector (P0-2): degradation policy when the patterns
    // YAML fails to load. Default false = fail-closed (reject all).
    bool injectionFailOpen() const;

    // Security — GuardClassifier
    bool guardModelEnabled() const;
    std::string guardModelPath() const;
    std::string guardModelVocabPath() const;
    std::string guardModelSpmPath() const;       // DeBERTa-v3 SentencePiece model
    std::string guardModelFailPolicy() const;    // "open" (default) | "closed"
    double guardModelThreshold() const;

    // Security — External Safety APIs (L4)
    bool externalSafetyEnabled() const;
    std::string externalSafetyMode() const;
    std::string externalSafetyFailPolicy() const;
    bool externalSafetyParallel() const;
    // Phase 6.3: shadow-mode toggle for the External Safety stage (SR3+SR6).
    bool externalSafetyShadowMode() const;
    int externalSafetyShadowMaxInflight() const;
    int externalSafetyShadowAuditTtlSeconds() const;

    bool openaiModerationEnabled() const;
    std::string openaiModerationApiKey() const;
    std::string openaiModerationBaseUrl() const;
    std::string openaiModerationModel() const;
    int openaiModerationTimeout() const;

    bool perspectiveApiEnabled() const;
    std::string perspectiveApiKey() const;
    std::string perspectiveBaseUrl() const;
    double perspectiveThreshold() const;
    std::vector<std::string> perspectiveAttributes() const;
    int perspectiveTimeout() const;

    // Security — AbuseDetector
    bool abuseDetectionEnabled() const;
    int abuseWindowSeconds() const;
    int abuseWarnThreshold() const;
    int abuseThrottleThreshold() const;
    int abuseBlockThreshold() const;
    int abuseBlockDurationSeconds() const;
    double abuseThrottleFactor() const;
    int abuseMaxKeysPerShard() const;
    bool abuseSimilarityEnabled() const;
    int abuseSimilarityHammingThreshold() const;
    int abuseSimilarityMaxFingerprints() const;
    int abuseSimilarityMaxContentBytes() const;

    // Vector store backend
    std::string vectorStoreBackend() const;

    // Milvus
    std::string milvusHost() const;
    int milvusPort() const;
    std::string milvusCollectionPrefix() const;
    std::string milvusMetricType() const;
    std::string milvusToken() const;
    int milvusConnectTimeout() const;
    int milvusRequestTimeout() const;
    bool milvusAutoCreateCollection() const;

    // Qdrant
    std::string qdrantHost() const;
    int qdrantPort() const;
    std::string qdrantCollectionPrefix() const;
    std::string qdrantDistance() const;
    std::string qdrantApiKey() const;
    int qdrantConnectTimeout() const;
    int qdrantRequestTimeout() const;
    bool qdrantAutoCreateCollection() const;

    // Cache
    float cacheThreshold() const;
    int cacheTtlSeconds() const;
    int cacheMaxEntries() const;
    int cacheMaxPartitions() const;
    bool cacheContextAware() const;
    std::string cacheWarmupFile() const;

    // Cache — conversation hash
    std::string cacheConversationHashMode() const;
    int cacheConversationHashWindow() const;

    // Phase 6.4 — Multi-turn conversation cache (TASK-20260513-01).
    // All defaults are SAFE (disabled / rule_based) so legacy deployments
    // see no behaviour change until conversation_cache.enabled is true.
    bool conversationCacheEnabled() const;
    std::string conversationSummarizerType() const;          // "rule_based" | "onnx"
    std::string conversationSummarizerOnnxModelPath() const;
    int conversationSummarizerMaxSummaryMs() const;          // SR7 hard ceiling
    int conversationSummarizerMaxInputTokens() const;
    bool conversationIdResolverEnabled() const;

    // Cache — adaptive threshold
    bool cacheAdaptiveEnabled() const;
    float cacheAdaptiveMinThreshold() const;
    float cacheAdaptiveMaxThreshold() const;
    float cacheAdaptiveAdjustmentRate() const;
    int cacheAdaptiveWindowSize() const;

    // Cache — policy
    bool cachePolicyEnabled() const;
    std::vector<std::string> cachePolicySkipModels() const;
    double cachePolicyMaxTemperature() const;
    bool cachePolicySkipStreaming() const;

    TracingConfig telemetryConfig() const;

    // Admin panel
    std::string adminJwtSecret() const;
    int adminJwtExpireSeconds() const;
    std::vector<std::string> adminCorsOrigins() const;
    std::vector<std::string> adminAllowedIps() const;
    // TASK-20260702-02 P2-5（SR-5）：可信反向代理 CIDR 列表。仅当 TCP peer 属于
    // 该列表时才采信 X-Forwarded-For 解析真实客户端 IP（防 XFF 伪造）；为空=不信任
    // 任何 XFF，等价于直接用 peer（默认安全）。
    std::vector<std::string> adminTrustedProxies() const;
    std::string adminStaticDir() const;

    // Plugins
    bool pluginEnabled() const;
    std::string pluginSearchPath() const;
    struct PluginStageConfig {
        std::string name;
        std::string path;
        std::string position;
        int order = 0;
    };
    std::vector<PluginStageConfig> pluginStages() const;

    // Deployment
    std::string deploymentMode() const;

    // Routing
    std::string routerType() const;
    double mlRouterCostWeight() const;
    double mlRouterQualityWeight() const;
    double mlRouterLatencyWeight() const;

    struct ABVariantConfig { std::string model; int weight = 1; };
    struct ABExperimentConfig {
        std::string name;
        std::vector<ABVariantConfig> variants;
        bool enabled = true;
        std::string tenant_id;
    };
    std::vector<ABExperimentConfig> abTestExperiments() const;

    // --- Geo-aware routing (Phase 9.1.1) ---
    // Reads `routing.geo.*` YAML. All getters are safe to call before load().
    bool geoRoutingEnabled() const;
    // Returns one of: "strict" | "prefer" | "any" (default: "prefer").
    std::string geoAffinity() const;
    std::string geoDefaultClientRegion() const;
    std::vector<std::string> geoHeaderNames() const;
    struct GeoIpRange { std::string cidr; std::string region; };
    std::vector<GeoIpRange> geoIpRegionMap() const;
    std::unordered_map<std::string, std::string> geoRegionAliases() const;

    // --- Phase 6.1 Multimodal routing (TASK-20260513-01 Epic 2 / 5.1) ---
    // Reads `multimodal.*` YAML. Defaults: disabled, policy=cheapest, no
    // quotas — turning the block on is required to construct a
    // ModalityRouter; otherwise the legacy ConnectorRegistry path is used.
    bool multimodalEnabled() const;
    std::string multimodalRoutingPolicy() const;       // cheapest | round_robin | fastest_p99
    bool multimodalCostAttributionEnabled() const;
    bool multimodalRateLimitEnabled() const;
    struct ModalityQuotaConfig {
        std::string modality;     // embedding | image_gen | audio_transcribe | audio_speech | moderation
        std::string identity;     // tenant_id, or "*" for global
        double max_tokens = 0.0;
        double refill_rate = 0.0;
    };
    std::vector<ModalityQuotaConfig> multimodalRateLimitQuotas() const;

    // --- Autonomy / FeedbackBus (Phase 11.0) ---
    // Reads `autonomy.feedback_bus.*` YAML.
    bool feedbackBusEnabled() const;
    int  feedbackBusMaxQueueSize() const;
    std::string feedbackBusDropPolicy() const;  // "oldest" | "newest"

    // --- Phase 11.5 Autonomy / CostAutonomy / BudgetGuard (TASK-20260518-02) ---
    // Reads `autonomy.*`, `autonomy.cost_optimizer.*`, `budget_guard.*` YAML.
    // All defaults are SAFE (disabled / manual_only / fail_open=true) so a
    // legacy data-plane config keeps current behaviour until operators opt in.
    // SR17 reuse: AEGISGATE_DISABLE_AUTONOMY env always wins over yaml.

    // Global autonomy switch. When false the AutonomyApprovalWorkflow
    // rejects every propose() so no IApprovalApplier ever runs.
    bool autonomyEnabled() const;

    // One of: "manual_only" | "auto_low_risk" | "auto_all".
    // Workflow's caller checks this to decide whether to auto-approve a
    // freshly minted proposal (see CostAutonomyApplier::isLowRisk for the
    // auto_low_risk rule set, plan §C2).
    std::string autonomyAutoApplyMode() const;

    // Days after which APPLIED / REJECTED / ROLLED_BACK proposals are
    // pruned by ApprovalQueue::prune. 0 keeps everything (NOT recommended).
    int autonomyProposalRetentionDays() const;

    // CostOptimizer 2.0 propose-path switch. Separate from autonomyEnabled
    // so operators can stage the cost track without flipping every Phase
    // 11.x track.
    bool costAutonomyEnabled() const;

    // BudgetGuardStage inbound stage configuration. The composite
    // `BudgetGuardConfig` struct lives in src/server/budget_guard_stage.h
    // so we expose flat atomic getters here to avoid a server→core cycle;
    // GatewayRuntime assembles the struct from these values in E4.2.
    bool budgetGuardEnabled() const;
    double budgetGuardPerTenant24hUsd() const;
    double budgetGuardPerRequestMaxUsd() const;
    bool budgetGuardFailOpenOnError() const;
    std::string budgetGuardDowngradeTier() const;
    std::string budgetGuardDowngradeHeaderName() const;
    std::string budgetGuardDowngradeHeaderValue() const;

    // Token optimization — prompt compression
    bool promptCompressionEnabled() const;
    int promptCompressionMaxContextMessages() const;
    bool promptCompressionCompressWhitespace() const;
    bool promptCompressionDedupSystem() const;

    // Token optimization — smart max tokens
    bool smartMaxTokensEnabled() const;
    int smartMaxTokensDefaultOutput() const;
    double smartMaxTokensOutputRatio() const;
    int smartMaxTokensMinOutput() const;

    // SSO
    bool ssoEnabled() const;
    std::string ssoDefaultProvider() const;

    // MFA
    std::string mfaEnforcement() const;
    int mfaTotpDigits() const;
    int mfaTotpPeriod() const;
    int mfaRecoveryCodeCount() const;
    // TASK-20260702-02 P2-2（SR-2）：MFA 验证失败锁定（防在线爆破）。
    int mfaLockoutMaxFailures() const;
    int mfaLockoutWindowSeconds() const;

    // Session
    int sessionAbsoluteTimeoutSeconds() const;
    int sessionIdleTimeoutSeconds() const;
    int sessionMaxConcurrent() const;

    // Cluster / backends
    std::string rateLimiterBackend() const;
    // TASK-20260708-02 / REV20260707-C1: alias for `rate_limit.backend`
    // (the user-facing namespace per feature-list.md:59). Precedence:
    //   rate_limit.backend  >  rate_limiter.backend (legacy)  >  "memory"
    // Both getters remain callable so existing wiring stays valid.
    std::string rateLimitBackend() const;
    std::string sessionBackend() const;

    // Phase 8: Agent orchestration
    bool agentEnabled() const;
    int agentMaxSteps() const;
    int agentMaxTotalTimeoutMs() const;
    int agentToolDefaultTimeoutMs() const;
    int agentToolMaxOutputBytes() const;
    // TASK-20260703-04 D1：/v1/agent HTTP 端点显式 opt-in（默认关，与 agentEnabled
    // 特性装配解耦——装配不等于对外暴露执行面，需单独审计后开启）。
    bool agentEndpointEnabled() const;

    // TASK-20260703-04 D2：Workflow 引擎并发/触发端点。
    bool workflowEndpointEnabled() const;      // /v1/workflow 端点 opt-in（默认关）
    int workflowWorkerCount() const;           // ThreadPool worker 数（默认 4）
    int workflowMaxConcurrentRuns() const;     // 背压上限（默认 16）
    int workflowNodeTimeoutMs() const;         // 节点默认超时（默认 30000）

    // Phase 8: RAG
    bool ragEnabled() const;
    int ragTopK() const;
    float ragMinRelevance() const;
    int ragMaxContextTokens() const;
    int ragChunkSize() const;
    int ragChunkOverlap() const;
    std::string ragInjectionPosition() const;

    // Phase 8: Cache 2.0
    bool cacheFeedbackEnabled() const;
    bool cachePredictiveWarmupEnabled() const;
    int cachePredictiveWarmupIntervalSeconds() const;
    int cachePredictiveWarmupTopK() const;
    bool cacheCrossTenantEnabled() const;
    float cacheCrossTenantMinSimilarity() const;

    // Phase 9.3: Control-plane server options (all default-safe when key is
    // missing, so a data-plane config can be loaded by the control-plane
    // binary — it simply uses the defaults for anything outside its scope).
    std::string controlPlaneListen() const;             // "host:port"
    bool controlPlaneMutualTls() const;
    std::vector<std::string> controlPlaneAllowedClientFingerprints() const;
    int controlPlaneSubmitRateLimitPerUserPerMin() const;
    int controlPlaneMaxYamlBytes() const;
    std::string controlPlaneBootstrapYaml() const;

    // Phase 8: Advanced observability
    bool costAttributionEnabled() const;
    bool anomalyDetectionEnabled() const;
    double anomalyDetectionZScoreThreshold() const;
    int anomalyDetectionWindowSize() const;
    bool qualityMonitoringEnabled() const;
    double qualityMonitoringAlertThreshold() const;
    bool costOptimizationEnabled() const;

    const std::string& filePath() const;

    // Phase 9.3.4 — merged-yaml format support for data-plane rollouts.
    // "inline" when loaded from legacy format; version_id when loaded from
    // the control-plane merged-yaml format.
    std::string activeVersionId() const;
    std::optional<RolloutConfigView> rolloutConfig() const;
    // Returns a Config* parsed from configs[version_id], or nullptr.
    const Config* configForVersion(const std::string& version_id) const;

    struct AlertChannelConfig {
        std::string name;
        std::string type;
        std::string url;
        std::string secret;
    };
    std::vector<AlertChannelConfig> alertChannels() const;

    // Alert rules (alerting.rules[]). severity kept as string here so the
    // config layer stays independent of observe/alerting.h; the assembler
    // translates it to AlertSeverity via parseAlertSeverity().
    struct AlertRuleConfig {
        std::string id;
        std::string description;
        std::string metric_name;
        double threshold = 0.0;
        std::string severity = "warning";
        bool enabled = true;
        int cooldown_seconds = 0;
    };
    std::vector<AlertRuleConfig> alertRules() const;

    struct ValidationIssue {
        enum Severity { Error, Warning };
        Severity severity;
        std::string field;
        std::string message;
    };
    std::vector<ValidationIssue> validate() const;

private:
    YAML::Node safeNode(const std::string& k1, const std::string& k2) const {
        auto n1 = root_[k1];
        if (!n1 || !n1.IsMap()) return {};
        return n1[k2];
    }

    YAML::Node safeNode(const std::string& k1, const std::string& k2,
                        const std::string& k3) const {
        auto n2 = safeNode(k1, k2);
        if (!n2 || !n2.IsMap()) return {};
        return n2[k3];
    }

    YAML::Node safeNode(const std::string& k1, const std::string& k2,
                        const std::string& k3, const std::string& k4) const {
        auto n3 = safeNode(k1, k2, k3);
        if (!n3 || !n3.IsMap()) return {};
        return n3[k4];
    }

    template<typename T>
    T safeGet(const std::string& k1, const std::string& k2, const T& def) const {
        auto n2 = safeNode(k1, k2);
        if (!n2 || n2.IsNull()) return def;
        try { return n2.as<T>(); } catch (...) { return def; }
    }

    template<typename T>
    T safeGet(const std::string& k1, const std::string& k2,
              const std::string& k3, const T& def) const {
        auto n3 = safeNode(k1, k2, k3);
        if (!n3 || n3.IsNull()) return def;
        try { return n3.as<T>(); } catch (...) { return def; }
    }

    template<typename T>
    T safeGet(const std::string& k1, const std::string& k2,
              const std::string& k3, const std::string& k4,
              const T& def) const {
        auto n4 = safeNode(k1, k2, k3, k4);
        if (!n4 || n4.IsNull()) return def;
        try { return n4.as<T>(); } catch (...) { return def; }
    }

    YAML::Node root_;
    std::string file_path_;
    bool loaded_ = false;
    mutable std::shared_mutex rw_mutex_; // Lock Layer 0 — see docs/LOCK_ORDERING.md

    // Phase 9.3.4: merged-yaml fields.
    std::string active_version_id_ = "inline";
    std::optional<RolloutConfigView> rollout_config_;
    std::unordered_map<std::string, std::unique_ptr<Config>> configs_by_version_;
};

} // namespace aegisgate
