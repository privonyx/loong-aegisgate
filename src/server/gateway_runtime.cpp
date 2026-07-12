#include "gateway_runtime.h"
#include "auth/authorization.h"
#include "auth/encryption.h"
#include "auth/scim_service.h"
#ifdef AEGISGATE_ENABLE_REDIS
#include "storage/redis_cache_store.h"
#endif
#include "core/crypto.h"
#include "gateway/connector/factory.h"
#include "observe/feedback_bus.h"
#include "observe/metrics.h"
#include "observe/metrics_feedback_subscriber.h"
#include "observe/savings_aggregator.h"
#include "observe/token_estimator.h"
#include "observe/tracing.h"
#include "multimodal/openai_modality_handlers.h"
#include "multimodal/openai_modality_upstream_adapter.h"
#include "server/modality_quota_enforcer.h"
#include "server/response_finalizer.h"
#include "server/ab_attribution.h"
#include "server/cache_savings.h"
#include "server/proxy_billing.h"
#include <aegisgate/error_codes.h>
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <algorithm>
#include <string_view>

namespace aegisgate {

namespace {

// Map the name of the rejecting inbound stage to a precise ErrorCode so the
// client gets an accurate reason (e.g. topic/PII/budget) instead of every
// block being reported as an injection attack. Unknown/security stages fall
// back to InjectionDetected to preserve historical behavior.
ErrorCode rejectStageToErrorCode(const std::string& stage) {
    if (stage == "PIIFilter")          return ErrorCode::PiiBlocked;
    if (stage == "TopicGuard")         return ErrorCode::TopicViolation;
    if (stage == "ExternalSafetyStage") return ErrorCode::ContentFiltered;
    if (stage == "BudgetGuardStage")   return ErrorCode::BudgetExceeded;
    // InjectionDetector / GuardClassifier / RuleEngine / anything else
    return ErrorCode::InjectionDetected;
}

// P1-C: translate a non-blocking AbuseDetector decision into a rate-limit cost
// multiplier so Throttle/Warn actually take effect (previously only Block did).
// Throttle inflates the request's token cost by 1/throttle_factor so an abusive
// key exhausts its budget proportionally faster; Warn is observable only.
double abuseCostMultiplier(const AbuseDetector* detector,
                           AbuseDetector::Action action) {
    using Action = AbuseDetector::Action;
    if (action == Action::Throttle) {
        double f = detector ? detector->throttleFactor() : 0.5;
        double mult = (f > 0.0 && f < 1.0) ? (1.0 / f) : 2.0;
        spdlog::warn("AbuseDetector: throttling key — rate budget x{:.2f}", mult);
        return mult;
    }
    if (action == Action::Warn) {
        spdlog::warn("AbuseDetector: warn — elevated rejection rate for key");
    }
    return 1.0;
}

// Last user message content, truncated (TASK-20260712-02 / creative extractAbuseText).
std::string extractAbuseText(const std::vector<Message>& messages, size_t max_bytes) {
    std::string out;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == "user") {
            out = it->content;
            break;
        }
    }
    if (out.size() > max_bytes) {
        out.resize(max_bytes);
    }
    return out;
}

std::string extractAbuseTextFromRaw(std::string_view raw, size_t max_bytes) {
    std::string out(raw.substr(0, std::min(raw.size(), max_bytes)));
    return out;
}

// Process-wide MetricsFeedbackSubscriber holder. Lazily allocated and attached
// to FeedbackBus::instance() the first time the runtime initializes the bus.
// Re-attach on subsequent initialize() calls is a no-op (idempotent — the
// underlying subscribe() already returns a fresh id, but for the metrics
// bridge a single registration is sufficient and matches actual production
// semantics).
std::unique_ptr<MetricsFeedbackSubscriber>& feedbackSubscriberSingleton() {
    static std::unique_ptr<MetricsFeedbackSubscriber> sub;
    return sub;
}

// P1-4: UTC ISO-8601 timestamp for proxy cost records (mirrors
// CostTracker::currentTimestamp formatting, which is private to that class).
std::string isoTimestampNow() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string resolveEnvVar(const std::string& value) {
    if (value.size() > 3 && value.substr(0, 2) == "${" && value.back() == '}') {
        auto var_name = value.substr(2, value.size() - 3);
        const char* env_val = std::getenv(var_name.c_str());
        return env_val ? std::string(env_val) : "";
    }
    return value;
}

} // namespace

// TASK-20260708-02 / REV20260707-C1 Epic 3 — SR-1 predicate.
// See gateway_runtime.h for full truth table. Free function so the unit
// test can exercise it without instantiating GatewayRuntime (Drogon).
bool shouldWireRedisRateLimiter(const Config& config, bool cluster_gate_enabled) {
    // New user-facing key takes precedence (`rate_limit.*` namespace, per
    // feature-list.md:59). Config::rateLimitBackend() already resolves the
    // three-tier precedence rate_limit.backend > rate_limiter.backend > "memory".
    if (config.rateLimitBackend() == "redis") return true;
    // Cluster mode gated by the enterprise FeatureGate::ClusterDeployment.
    if (cluster_gate_enabled && config.deploymentMode() == "cluster") return true;
    return false;
}

// TASK-20260711-01 / REV20260707-I13 Epic 1 — Edition gate predicate.
// See gateway_runtime.h for the truth table. Free function so unit tests
// exercise it without a full GatewayRuntime instance (Drogon).
// Nullable-safe: mirrors the pipeline_assembler.cpp:525
// `if (feature_gate && feature_gate->isEnabled(X))` idiom project-wide.
bool isAdvancedRoutingEnabled(const FeatureGate* gate) {
    return gate != nullptr && gate->isEnabled(Feature::AdvancedRouting);
}

GatewayRuntime& GatewayRuntime::instance() {
    static GatewayRuntime rt;
    return rt;
}

void GatewayRuntime::initialize(const Config& config) {
    config_ = &config;

    // Phase 11.5 (TASK-20260518-02 E4.2) — Autonomy idempotency.
    //
    // initialize() may be called more than once in test setups (a previous
    // initialize() followed by shutdown() without going through
    // reinitializeForTesting()). Stale autonomy observer pointers from the
    // prior call would survive into the new pipeline_ otherwise, so we
    // proactively drop them here. Order mirrors reinitializeForTesting():
    // observer pointers first, owning shared_ptrs next, so we never leave
    // a dangling pointer-into-pipeline behind.
    budget_guard_stage_raw_ = nullptr;
    cost_autonomy_applier_.reset();
    // Phase 11.1 (TASK-20260523-01 R2.5) — drop adaptive-guard observers
    // BEFORE workflow/queue so the applier's weak reference to the workflow
    // is released first.
    guard_admin_controller_.reset();
    guard_model_applier_.reset();
    guard_anomaly_detector_.reset();
    guard_feedback_rate_limiter_.reset();
    // Phase 11.3 Workflow 2.0 — applier holds raw ptrs to engine + store,
    // so reset in dependency-reverse order before approval_workflow_ goes.
    workflow_applier_.reset();
    workflow_engine_.reset();
    workflow_state_store_.reset();
    guard_feedback_sink_.reset();
    guard_model_registry_.reset();
    approval_workflow_.reset();
    approval_queue_.reset();
    ml_router_raw_ = nullptr;

    pipeline_ = std::make_unique<AssembledPipeline>(
        PipelineAssembler::assemble(config));

    if (config.abuseDetectionEnabled()) {
        AbuseDetector::Config abuse_cfg;
        abuse_cfg.window_seconds = config.abuseWindowSeconds();
        abuse_cfg.warn_threshold = config.abuseWarnThreshold();
        abuse_cfg.throttle_threshold = config.abuseThrottleThreshold();
        abuse_cfg.block_threshold = config.abuseBlockThreshold();
        abuse_cfg.block_duration_seconds = config.abuseBlockDurationSeconds();
        abuse_cfg.throttle_factor = config.abuseThrottleFactor();
        abuse_cfg.max_keys_per_shard =
            static_cast<size_t>(std::max(1, config.abuseMaxKeysPerShard()));
        abuse_cfg.similarity_enabled = config.abuseSimilarityEnabled();
        abuse_cfg.similarity_hamming_threshold =
            config.abuseSimilarityHammingThreshold();
        abuse_cfg.similarity_max_fingerprints =
            static_cast<size_t>(std::max(1, config.abuseSimilarityMaxFingerprints()));
        abuse_cfg.similarity_max_content_bytes =
            static_cast<size_t>(std::max(1, config.abuseSimilarityMaxContentBytes()));
        abuse_detector_ = std::make_unique<AbuseDetector>(abuse_cfg);
        spdlog::info("AbuseDetector: enabled (window={}s, block_threshold={}, similarity={})",
                     abuse_cfg.window_seconds, abuse_cfg.block_threshold,
                     abuse_cfg.similarity_enabled);
    }

    // TASK-20260711-01 / REV20260707-I13 Epic 2 — AdvancedRouting license gate.
    // MLRouter / ABTestRouter / GeoRouter are Enterprise-tier per the
    // Editions matrix and feature-list.md; gate their assembly on
    // Feature::AdvancedRouting so Community deployments can't silently
    // bypass the license by config alone. Nullable-safe.
    const bool advanced_routing_enabled =
        isAdvancedRoutingEnabled(pipeline_->feature_gate.get());

    auto router_type = config.routerType();
    if (router_type == "ml") {
        if (advanced_routing_enabled) {
            MLRouter::Weights w{config.mlRouterCostWeight(),
                                config.mlRouterQualityWeight(),
                                config.mlRouterLatencyWeight()};
            auto ml = std::make_unique<MLRouter>(w);
            // Phase 11.5 (TASK-20260518-02 E4.2) — capture raw pointer before
            // wrapping; later autonomy wiring uses a non-owning aliasing
            // shared_ptr<MLRouter> so CostAutonomyApplier can override quality
            // tier on the same instance the router chain dispatches to.
            ml_router_raw_ = ml.get();
            router_ = std::move(ml);
            spdlog::info("Router: ML (cost={}, quality={}, latency={})",
                         w.cost, w.quality, w.latency);
        } else {
            // REV20260707-I13: Community edition cannot use MLRouter.
            // Fall back to CostAware (mirrors isOfflineOnlyRouterType
            // warn-and-fallback pattern). Service stays up; ops upgrades
            // license or switches routing.type per the warning.
            spdlog::warn(
                "Router: 'ml' requires the AdvancedRouting feature "
                "(Enterprise edition license); falling back to CostAware. "
                "Upgrade license or set routing.type = basic | cost_aware. "
                "See docs/known-issues.md#REV20260707-I13.");
            router_ = std::make_unique<CostAwareRouter>();
            spdlog::info("Router: CostAware (license-gated fallback)");
        }
    } else if (router_type == "basic") {
        router_ = std::make_unique<BasicRouter>();
        spdlog::info("Router: Basic");
    } else {
        // P2-#6: make the fall-through explicit instead of silently degrading.
        if (isOfflineOnlyRouterType(router_type)) {
            spdlog::warn(
                "Router: '{}' is an offline/shadow-only strategy (Phase 11.2 "
                "self-evolving router — available via the replay CLI) and is "
                "NOT wired to live routing; falling back to CostAware. Use "
                "routing.type = ml | basic | cost_aware for live routing.",
                router_type);
        } else if (router_type != "cost_aware") {
            spdlog::warn("Router: unknown routing.type '{}', falling back to "
                         "CostAware", router_type);
        }
        router_ = std::make_unique<CostAwareRouter>();
        spdlog::info("Router: CostAware");
    }

    auto ab_experiments = config.abTestExperiments();
    if (!ab_experiments.empty()) {
        if (advanced_routing_enabled) {
            std::vector<ABExperiment> exps;
            for (const auto& e : ab_experiments) {
                ABExperiment exp;
                exp.name = e.name;
                for (const auto& v : e.variants) {
                    exp.variants.push_back({v.model, v.weight});
                }
                exp.enabled = e.enabled;
                exp.tenant_id = e.tenant_id;
                exps.push_back(std::move(exp));
            }
            router_ = std::make_unique<ABTestRouter>(std::move(router_), std::move(exps));
            spdlog::info("Router: wrapped with ABTestRouter ({} experiments)",
                         ab_experiments.size());
        } else {
            // REV20260707-I13: skip ABTestRouter wrap without AdvancedRouting.
            spdlog::warn(
                "Router: ABTestRouter requested ({} experiment(s)) but the "
                "AdvancedRouting feature is not licensed; skipping A/B wrap. "
                "See docs/known-issues.md#REV20260707-I13.",
                ab_experiments.size());
        }
    }

    if (config.geoRoutingEnabled()) {
        if (advanced_routing_enabled) {
            GeoConfig geo;
            geo.enabled = true;
            geo.affinity = GeoConfig::parseAffinity(config.geoAffinity());
            geo.default_client_region = config.geoDefaultClientRegion();
            geo.header_names = config.geoHeaderNames();
            for (const auto& r : config.geoIpRegionMap()) {
                geo.ip_region_map.emplace_back(r.cidr, r.region);
            }
            geo.region_aliases = config.geoRegionAliases();
            router_ = std::make_unique<GeoRouter>(std::move(router_), std::move(geo));
            spdlog::info("Router: wrapped with GeoRouter (affinity={}, default_region={})",
                         config.geoAffinity(), config.geoDefaultClientRegion());
        } else {
            // REV20260707-I13: skip GeoRouter wrap without AdvancedRouting.
            spdlog::warn(
                "Router: GeoRouter requested (geo.enabled=true) but the "
                "AdvancedRouting feature is not licensed; skipping geo wrap. "
                "See docs/known-issues.md#REV20260707-I13.");
        }
    }

    // Phase 11.0 — Wire FeedbackBus into the runtime so autonomy modules can
    // publish/subscribe events. Disabled by default; enabling has zero impact
    // on the request hot path beyond the publish() O(1) enqueue cost when
    // the bus is fed.
    if (config.feedbackBusEnabled()) {
        FeedbackBusConfig fb_cfg;
        fb_cfg.enabled = true;
        fb_cfg.max_queue_size =
            static_cast<size_t>(config.feedbackBusMaxQueueSize());
        fb_cfg.drop_policy = config.feedbackBusDropPolicy();
        auto& bus = FeedbackBus::instance();
        bus.reconfigure(std::move(fb_cfg));
        bus.start();
        ensureFeedbackMetricsSubscriber();
        spdlog::info("FeedbackBus: enabled (max_queue={}, drop_policy={})",
                     config.feedbackBusMaxQueueSize(),
                     config.feedbackBusDropPolicy());
    }

    CircuitConfig cb_cfg;
    cb_cfg.failure_threshold = config.circuitFailureThreshold();
    cb_cfg.reset_timeout =
        std::chrono::seconds(config.circuitResetTimeoutSeconds());
    cb_cfg.half_open_max_calls = config.circuitHalfOpenMaxCalls();
    cb_cfg.max_circuits =
        static_cast<size_t>(std::max(1, config.circuitMaxCircuits()));
    cb_cfg.circuit_idle_ttl =
        std::chrono::seconds(config.circuitIdleTtlSeconds());
    fallback_ =
        std::make_unique<FallbackManager>(connector_registry_, cb_cfg);

    RateLimiter::Config rl_config{config.rateLimitMaxTokens(),
                                   config.rateLimitRefillRate()};
    std::atomic_store(&rate_limiter_, std::make_shared<RateLimiter>(rl_config));

    // NB: Redis wiring for the rate limiter (and, in cluster mode, three
    // sibling components) is deferred to the unified assembly block below
    // — after pipeline_/router_/abuse_detector_/fallback_ are constructed.
    // TASK-20260708-02 / REV20260707-C1 replaced the historical
    // no-op `rateLimiterBackend()=="redis"` info-log with real wiring.

    loadProviders(config.modelsConfigPath());

    valid_api_key_hashes_.clear();
    for (const auto& key : config.authApiKeys()) {
        valid_api_key_hashes_.push_back(crypto::sha256(key));
    }
    if (config.authEnabled() && valid_api_key_hashes_.empty()) {
        spdlog::warn("Auth is enabled but no valid API keys configured — "
                     "all authenticated endpoints will reject requests. "
                     "Set AEGISGATE_API_KEY or configure auth.api_keys.");
    }

    // Set up audit file sink if configured
    auto audit_path = config.auditLogPath();
    if (!audit_path.empty() && pipeline_->audit_logger) {
        pipeline_->audit_logger->setSink(
            [audit_path](const AuditEntry& entry) {
                std::ofstream ofs(audit_path, std::ios::app);
                if (!ofs) {
                    spdlog::error("Audit log write failed: cannot open {}", audit_path);
                    return;
                }
                ofs << entry.timestamp << "\t"
                    << entry.request_id << "\t"
                    << entry.tenant_id << "\t"
                    << entry.stage_name << "\t"
                    << entry.action << "\t"
                    << entry.detail << "\t"
                    << entry.input_hash << "\t"
                    << entry.chain_hash << "\n";
                if (ofs.fail()) {
                    spdlog::error("Audit log write failed: I/O error on {}", audit_path);
                }
            });
        spdlog::info("Audit log sink: {}", audit_path);
    }

    // Enable audit log encryption if encryption key is available
    if (pipeline_->audit_logger && Encryption::instance().isAvailable()) {
        pipeline_->audit_logger->setEncryption(&Encryption::instance());
    }

    // Load pricing into cost tracker
    if (pipeline_->cost_tracker) {
        pipeline_->cost_tracker->loadPricing(config.modelsConfigPath());
    }

    // 创建 SavingsAggregator：cost_tracker 装载 pricing 后才有效，
    // 否则所有节省事件都会标 fallback_pricing=true。
    if (pipeline_->cost_tracker) {
        savings_aggregator_ =
            std::make_unique<SavingsAggregator>(pipeline_->cost_tracker);
        // TASK-20260617-02: 注入持久化 store（启动 write-behind flush 线程）并
        // 回放历史节省事件，使重启后仪表盘 Hero 卡片恢复近 N 天视图。
        if (config.dashboardPersistenceEnabled() && pipeline_->persistent_store) {
            savings_aggregator_->setPersistentStore(
                pipeline_->persistent_store.get());
            savings_aggregator_->loadFromStore(config.dashboardReloadDays());
        }
    }

    // --- Create AuthService ---
    // Always create so admin panel login works in both community and enterprise.
    // In non-RBAC mode it falls back to config-based API key validation.
    if (pipeline_->persistent_store) {
        auth_service_ = std::make_unique<AuthService>(
            pipeline_->persistent_store.get(), config_,
            pipeline_->feature_gate ? pipeline_->feature_gate.get() : nullptr);
        if (pipeline_->feature_gate && pipeline_->feature_gate->isEnabled(Feature::RBAC)) {
            spdlog::info("AuthService: RBAC mode enabled");
        } else {
            spdlog::info("AuthService: legacy mode (config-based keys)");
        }
    }

    // P0-A (TASK-20260701-01): startup fail-open guard. When neither RBAC nor
    // legacy auth is active, every /v1 endpoint accepts unauthenticated
    // callers. Surface this loudly so an accidental open deployment is caught
    // at boot instead of a low-signal "auth=off" info line.
    {
        bool rbac_on = pipeline_->feature_gate &&
                       pipeline_->feature_gate->isEnabled(Feature::RBAC);
        if (!rbac_on && !config.authEnabled()) {
            spdlog::warn("SECURITY: authentication is DISABLED (RBAC off and "
                         "auth.enabled=false) — all /v1 endpoints are open to "
                         "unauthenticated callers. This fail-open posture is "
                         "unsafe for production; enable RBAC or set "
                         "auth.enabled=true with configured API keys.");
        }
    }

    if (pipeline_->persistent_store) {
        // TASK-20260604-01 P0-D：注入 audit_logger，使 SCIM 写操作可审计追溯。
        scim_service_ = std::make_unique<ScimService>(
            pipeline_->persistent_store.get(),
            pipeline_->audit_logger);
    }

#ifdef AEGISGATE_ENABLE_REDIS
    // TASK-20260708-02 / REV20260707-C1 Epic 3 — unified Redis rate limiter
    // wiring. Triggered by shouldWireRedisRateLimiter() which OR-combines
    // three sources of intent (rate_limit.backend / rate_limiter.backend
    // legacy alias / deployment.mode=cluster gated by the enterprise
    // FeatureGate::ClusterDeployment). Previously the two branches lived
    // apart: assemble-time `rateLimiterBackend()=="redis"` was a no-op
    // info-log, and only `deployment.mode=cluster` actually wired Redis
    // — leaving the feature-list.md:59 "Redis-backed cluster quotas via
    // rate_limit.*" claim structurally unmet.
    const bool cluster_gate =
        pipeline_->feature_gate &&
        pipeline_->feature_gate->isEnabled(Feature::ClusterDeployment);
    const bool cluster_mode_active =
        cluster_gate && (config.deploymentMode() == "cluster");
    if (shouldWireRedisRateLimiter(config, cluster_gate)) {
        auto* redis = dynamic_cast<RedisCacheStore*>(pipeline_->cache_store.get());
        if (redis && redis->isHealthy()) {
            redis_state_store_ = std::make_unique<RedisStateStore>(redis);
            if (redis_state_store_->initialize()) {
                auto rl = std::atomic_load(&rate_limiter_);
                if (rl) rl->setRedisStateStore(redis_state_store_.get());
                // The 4-component shared-store pattern (RateLimiter +
                // AbuseDetector + CircuitBreaker + MLRouter) applies to
                // cluster mode only. Standalone `rate_limit.backend=redis`
                // just externalizes the rate limiter — abuse/CB/router
                // remain node-local, matching prior single-node semantics.
                if (cluster_mode_active) {
                    if (abuse_detector_) {
                        abuse_detector_->setRedisStateStore(redis_state_store_.get());
                    }
                    fallback_->mutableCircuitBreaker()
                        .setRedisStateStore(redis_state_store_.get());
                    if (auto* ml = dynamic_cast<MLRouter*>(router_.get())) {
                        ml->setRedisStateStore(redis_state_store_.get());
                    }
                    spdlog::info(
                        "Cluster mode: Redis state store active, "
                        "4 components externalized");
                } else {
                    spdlog::info(
                        "RateLimiter backend: redis "
                        "(standalone mode, 1 component externalized)");
                }
            } else {
                spdlog::warn(
                    "Redis state store init failed, falling back to "
                    "in-memory rate limiter");
                MetricsRegistry::instance().rateLimiterDegradedTotal().inc();
                redis_state_store_.reset();
            }
        } else {
            spdlog::warn(
                "Redis rate limiter requested but Redis cache not "
                "available/healthy, falling back to in-memory");
            MetricsRegistry::instance().rateLimiterDegradedTotal().inc();
        }
    }
#endif

    if (!config.adminStaticDir().empty() && config.adminJwtSecret().empty()) {
        spdlog::warn("Admin panel is configured (static_dir={}) but "
                     "admin.jwt_secret is empty — login will fail. "
                     "Set AEGISGATE_ADMIN_JWT_SECRET or admin.jwt_secret in config.",
                     config.adminStaticDir());
    }

    // ----------------------------------------------------------------------
    // Phase 11.5 (TASK-20260518-02 E4.2) — Autonomy wiring.
    //
    // Construct the cross-cutting AutonomyApprovalWorkflow + per-source
    // appliers + BudgetGuardStage so every autonomy decision flows through
    // a single audited "propose → approve → apply" funnel (SR2).
    //
    // Three independent yaml flags drive this block:
    //   - autonomy.enabled                — master gate (SR2 + SR17 reuse)
    //   - autonomy.cost_optimizer.enabled — CostOptimizer v2 propose path
    //   - budget_guard.enabled            — direct inbound stage enforcement
    //
    // All three default to false so a legacy yaml gets zero behaviour change.
    // The block is intentionally placed AFTER audit_logger / cost_tracker /
    // persistent_store / router_ are wired so we can pass real dependencies
    // (vs nullptr) into the autonomy components.
    {
        const bool any_autonomy = config.autonomyEnabled() ||
                                  config.costAutonomyEnabled() ||
                                  config.budgetGuardEnabled();
        if (any_autonomy) {
            spdlog::info("Autonomy wiring requested "
                         "(autonomy={}, cost_optimizer={}, budget_guard={})",
                         config.autonomyEnabled(),
                         config.costAutonomyEnabled(),
                         config.budgetGuardEnabled());
        }

        // --- ApprovalQueue + AutonomyApprovalWorkflow ---------------------
        // The workflow itself only matters when either the master autonomy
        // gate is on OR the cost_optimizer propose path is on; BudgetGuard
        // is decoupled (it doesn't propose, it enforces).
        if (config.autonomyEnabled() || config.costAutonomyEnabled()) {
            approval_queue_ = std::make_shared<autonomy::ApprovalQueue>(
                pipeline_->persistent_store.get());
            if (!approval_queue_->initialize()) {
                spdlog::error(
                    "ApprovalQueue::initialize() failed — disabling autonomy"
                    " workflow (persistent_store rejected enumerate?)");
                approval_queue_.reset();
            } else if (!pipeline_->audit_logger) {
                spdlog::error(
                    "Autonomy workflow needs an audit_logger (T03 mitigation)"
                    " — disabling workflow");
                approval_queue_.reset();
            } else {
                // audit_logger is owned by pipeline_ (raw pointer). Wrap it
                // in a non-owning shared_ptr so the workflow signature is
                // satisfied without taking ownership.
                auto audit_alias = std::shared_ptr<AuditLogger>(
                    pipeline_->audit_logger, [](AuditLogger*) {});
                approval_workflow_ =
                    std::make_shared<autonomy::AutonomyApprovalWorkflow>(
                        approval_queue_, audit_alias, /*pii*/ nullptr);
                // Yaml flag is layered ON TOP of the env-derived kill switch
                // (SR17). The env var still wins via isAutonomyEnabled()
                // because the per-instance override only narrows, not widens.
                approval_workflow_->setAutonomyEnabledOverride(
                    config.autonomyEnabled() ? std::optional<bool>{true}
                                              : std::optional<bool>{false});

                // Register CostAutonomyApplier whenever the cost track is
                // active AND a real MLRouter exists; otherwise propose calls
                // will simply audit "apply_no_applier" and stay PROPOSED.
                if (config.costAutonomyEnabled() && ml_router_raw_ != nullptr) {
                    auto ml_alias = std::shared_ptr<MLRouter>(
                        std::shared_ptr<void>{}, ml_router_raw_);
                    cost_autonomy_applier_ =
                        std::make_shared<autonomy::CostAutonomyApplier>(
                            ml_alias);
                    approval_workflow_->registerApplier(
                        autonomy::AutonomySource::CostOptimizer,
                        cost_autonomy_applier_);
                    spdlog::info(
                        "CostAutonomyApplier registered "
                        "(auto_apply_mode={}, retention_days={})",
                        config.autonomyAutoApplyMode(),
                        config.autonomyProposalRetentionDays());
                } else if (config.costAutonomyEnabled()) {
                    spdlog::warn(
                        "cost_optimizer.enabled=true but router_type!=ml — "
                        "CostAutonomyApplier skipped (no MLRouter to drive)");
                }

                // Phase 11.1 (TASK-20260523-01 R2.5) — Adaptive Guard wiring.
                // Memory-backed registry (SQLite mirror is v2 / TASK-W19).
                // Sink, rate limiter and anomaly detector are constructed here
                // so the HTTP layer can forward requests without re-creating
                // dependencies per request.
                guard_model_registry_ =
                    std::make_shared<guard::MemoryGuardModelRegistry>();
                guard::GuardFeedbackSinkDeps sink_deps;
                if (pipeline_->pii_filter) {
                    sink_deps.pii = std::shared_ptr<PIIFilter>(
                        pipeline_->pii_filter, [](PIIFilter*) {});
                }
                sink_deps.audit = audit_alias;
                // FeedbackBus::instance() returns a process-wide singleton.
                // Wrap it as a non-owning shared_ptr so the sink signature is
                // satisfied without taking ownership.
                sink_deps.bus = std::shared_ptr<FeedbackBus>(
                    &FeedbackBus::instance(), [](FeedbackBus*) {});
                guard_feedback_sink_ =
                    std::make_shared<guard::GuardFeedbackSink>(sink_deps);
                guard_feedback_rate_limiter_ =
                    std::make_shared<guard::GuardFeedbackRateLimiter>(
                        guard::GuardFeedbackRateLimitConfig{});
                guard_anomaly_detector_ =
                    std::make_shared<guard::GuardFeedbackAnomalyDetector>(
                        guard::GuardFeedbackAnomalyConfig{});
                guard_model_applier_ =
                    std::make_shared<guard::GuardModelApplier>(
                        guard_model_registry_);
                approval_workflow_->registerApplier(
                    autonomy::AutonomySource::AdaptiveGuard,
                    guard_model_applier_);

                // Phase 11.3 TASK-20260523-02 — Workflow 2.0 wiring.
                //
                // The workflow uses the same approval_workflow_ (D4=C reuse)
                // and registers a WorkflowApprovalApplier so paused runs can
                // be resumed once a HumanApproval node is approved.
                //
                // v1 default uses MemoryWorkflowStateStore — durable SQLite
                // backend is opt-in via config (Workflow Marketplace v2).
                {
                    auto state_store =
                        std::make_unique<workflow::MemoryWorkflowStateStore>();
                    workflow_state_store_ = std::move(state_store);
                    workflow::WorkflowEngineConfig wf_cfg;
                    // TASK-20260703-04 D2：并发/超时/背压从 config 装配（替换全默认）。
                    wf_cfg.worker_count =
                        static_cast<std::size_t>(std::max(1, config.workflowWorkerCount()));
                    wf_cfg.default_node_timeout =
                        std::chrono::milliseconds(config.workflowNodeTimeoutMs());
                    wf_cfg.max_concurrent_runs = config.workflowMaxConcurrentRuns();
                    // C8 (TASK-20260703-02 / D2=A)：复用 pipeline_ 已装配的
                    // ToolSandbox（与 Orchestrator 同源工具白名单/资源边界），使
                    // NodeType::Tool 节点经 sandbox 真正执行而非全部进 DLQ。
                    // 生命周期（A2）：pipeline_（成员 203）先于 workflow_engine_
                    //（成员 268）声明 → 析构逆序 → engine 先析构；shutdown 亦
                    // workflow_engine_.reset() 早于 pipeline_.reset()。sandbox 缺失
                    //（未启用 AgentOrchestration）→ .get() 得 nullptr → 引擎安全降级
                    //（Tool 节点入 DLQ 不崩，见 workflow_engine.cpp:199）。
                    aegisgate::ToolSandbox* wf_sandbox =
                        pipeline_ ? pipeline_->tool_sandbox.get() : nullptr;
                    workflow_engine_ = std::make_unique<workflow::WorkflowEngine>(
                        wf_cfg, wf_sandbox,
                        workflow_state_store_.get());
                    workflow_applier_ =
                        std::make_shared<workflow::WorkflowApprovalApplier>(
                            workflow_engine_.get(),
                            workflow_state_store_.get());
                    approval_workflow_->registerApplier(
                        autonomy::AutonomySource::Workflow,
                        workflow_applier_);
                    spdlog::info(
                        "Workflow 2.0 wired (state_store=memory, applier="
                        "Workflow/v1, tool_sandbox={})",
                        wf_sandbox ? "active" : "absent(tool nodes DLQ)");
                }

                guard::GuardAdminDeps gad;
                gad.sink = guard_feedback_sink_;
                gad.rate_limiter = guard_feedback_rate_limiter_;
                gad.anomaly_detector = guard_anomaly_detector_;
                gad.registry = guard_model_registry_;
                gad.workflow = approval_workflow_;
                gad.audit = audit_alias;
                guard_admin_controller_ =
                    std::make_shared<guard::GuardAdminController>(std::move(gad));
                spdlog::info(
                    "Adaptive Guard wired (registry=memory, feedback bus={}, "
                    "applier=AdaptiveGuard/v1)",
                    config.feedbackBusEnabled() ? "enabled" : "disabled");

                // TASK-20260708-03 / REV20260707-C2: wire the Adaptive
                // Guard explanation recorder into the 4 inbound stages
                // that emit StageResult::Reject. Each stage constructs a
                // GuardExplanation via GuardExplanationBuilder::from* on
                // Reject and calls controller->recordExplanation(...) so
                // that GET /admin/api/guard/explanation/{id} returns
                // structured JSON. Nullable-safe (SR-3): stages ignore
                // the recording branch when no controller is wired.
                auto* ctrl = guard_admin_controller_.get();
                int wired = 0;
                if (pipeline_->injection_detector) {
                    pipeline_->injection_detector->setGuardAdminController(ctrl);
                    ++wired;
                }
                if (pipeline_->guard_classifier) {
                    pipeline_->guard_classifier->setGuardAdminController(ctrl);
                    ++wired;
                }
                if (pipeline_->rule_engine) {
                    pipeline_->rule_engine->setGuardAdminController(ctrl);
                    ++wired;
                }
                if (pipeline_->external_safety) {
                    pipeline_->external_safety->setGuardAdminController(ctrl);
                    ++wired;
                }
                spdlog::info(
                    "Adaptive Guard explanation recorder wired to {} "
                    "inbound stage(s)", wired);
            }
        }

        // --- BudgetGuardStage --------------------------------------------
        // Decoupled from the workflow: it's an inbound stage that enforces
        // 24h tenant caps and per-request maxima directly. Added LAST in the
        // inbound pipeline so it runs after audit/preprocess/PII/cache; that
        // way a cache HIT doesn't get billed against the budget (cache hits
        // short-circuit the pipeline before this stage).
        if (config.budgetGuardEnabled()) {
            if (!pipeline_->cost_tracker) {
                spdlog::warn(
                    "budget_guard.enabled=true but cost_tracker is null — "
                    "skipping stage install");
            } else {
                BudgetGuardConfig bg_cfg;
                bg_cfg.enabled = true;
                bg_cfg.per_tenant_24h_usd =
                    config.budgetGuardPerTenant24hUsd();
                bg_cfg.per_request_max_usd =
                    config.budgetGuardPerRequestMaxUsd();
                bg_cfg.fail_open_on_error =
                    config.budgetGuardFailOpenOnError();
                bg_cfg.downgrade_tier = config.budgetGuardDowngradeTier();
                bg_cfg.downgrade_header_name =
                    config.budgetGuardDowngradeHeaderName();
                bg_cfg.downgrade_header_value =
                    config.budgetGuardDowngradeHeaderValue();

                auto tracker_alias = std::shared_ptr<CostTracker>(
                    pipeline_->cost_tracker, [](CostTracker*) {});
                std::shared_ptr<MLRouter> router_alias;
                if (ml_router_raw_) {
                    router_alias = std::shared_ptr<MLRouter>(
                        std::shared_ptr<void>{}, ml_router_raw_);
                }
                auto stage = std::make_unique<BudgetGuardStage>(
                    tracker_alias, router_alias, bg_cfg);
                budget_guard_stage_raw_ = stage.get();
                pipeline_->inbound.addStage(std::move(stage));
                spdlog::info(
                    "BudgetGuardStage installed "
                    "(per_tenant_24h=${:.2f}, per_request_max=${:.2f}, "
                    "fail_open_on_error={})",
                    bg_cfg.per_tenant_24h_usd,
                    bg_cfg.per_request_max_usd,
                    bg_cfg.fail_open_on_error);
            }
        }
    }

    // Phase 6.1 wire (Epic 5.1b) — ModalityRouter + 5 OpenAI handlers.
    // Default-disabled: when multimodal.enabled is false the router stays
    // null and processProxyRequest falls through to the legacy
    // ConnectorRegistry path (zero behaviour change). Wiring the rate limiter
    // into processProxyRequest itself is intentionally deferred — the quotas
    // are read from yaml here and surfaced via getters, but the per-request
    // check belongs in a follow-up Epic 5.1c so this commit stays focused on
    // the router skeleton.
    if (config.multimodalEnabled()) {
        auto* openai = connector_registry_.findByProvider("openai");
        if (!openai) {
            spdlog::warn("multimodal.enabled=true but no provider named "
                         "'openai' is registered — skipping ModalityRouter "
                         "wire-up (legacy ConnectorRegistry path retained).");
        } else {
            openai_modality_upstream_ =
                std::make_unique<OpenAIModalityUpstreamAdapter>(openai);
            auto router = std::make_unique<ModalityRouter>();
            router->registerHandler(std::make_unique<OpenAIEmbeddingHandler>(
                *openai_modality_upstream_));
            router->registerHandler(std::make_unique<OpenAIImageGenHandler>(
                *openai_modality_upstream_));
            router->registerHandler(std::make_unique<OpenAIAudioTranscribeHandler>(
                *openai_modality_upstream_));
            router->registerHandler(std::make_unique<OpenAIAudioSpeechHandler>(
                *openai_modality_upstream_));
            router->registerHandler(std::make_unique<OpenAIModerationHandler>(
                *openai_modality_upstream_));

            const auto policy_str = config.multimodalRoutingPolicy();
            RoutingPolicy::Strategy strategy =
                RoutingPolicy::Strategy::Cheapest;
            if (policy_str == "round_robin") {
                strategy = RoutingPolicy::Strategy::RoundRobin;
            } else if (policy_str == "fastest_p99") {
                strategy = RoutingPolicy::Strategy::FastestP99;
            }
            for (auto m : {Modality::Embedding, Modality::ImageGen,
                            Modality::AudioTranscribe, Modality::AudioSpeech,
                            Modality::Moderation}) {
                router->setRoutingPolicy(m, RoutingPolicy{strategy});
            }
            modality_router_ = std::move(router);
            spdlog::info("ModalityRouter: wired with 5 OpenAI handlers "
                         "(policy={})", policy_str);
            if (config.multimodalRateLimitEnabled() && rate_limiter_) {
                // Phase 6.1 Epic 5.1c (B1, TASK-20260515-01): construct the
                // ModalityRateLimiter on top of the global RateLimiter and
                // load configured quotas. processProxyRequest enforces it
                // BEFORE upstream dispatch (fail-open if nullptr or no quota
                // for the request's modality — see SR-NEW4 in spec).
                auto mrl = std::make_unique<ModalityRateLimiter>(*rate_limiter_);
                size_t loaded = 0;
                for (const auto& q : config.multimodalRateLimitQuotas()) {
                    Modality m = modalityFromString(q.modality);
                    if (m == Modality::Unknown) continue;
                    mrl->setQuota(m, RateLimiter::Config{q.max_tokens,
                                                          q.refill_rate});
                    ++loaded;
                }
                modality_rate_limiter_ = std::move(mrl);
                spdlog::info("ModalityRateLimiter: {}/{} quotas loaded "
                             "(per-request enforcement ENABLED)",
                             loaded, config.multimodalRateLimitQuotas().size());
            } else if (config.multimodalRateLimitEnabled()) {
                spdlog::warn("ModalityRateLimiter: rate_limit.enabled=true "
                             "but no global RateLimiter configured "
                             "(fail-open: per-modality enforcement DISABLED)");
            }
        }
    }

    // TASK-20260703-04 D2：/v1/workflow 端点独立装配。若 autonomy 路径未建引擎
    // （autonomy 关但端点 opt-in），在此独立装配 MemoryWorkflowStateStore +
    // WorkflowEngine，使触发端点生产可达而不依赖 autonomy。
    if (config.workflowEndpointEnabled() && !workflow_engine_) {
        workflow_state_store_ =
            std::make_unique<workflow::MemoryWorkflowStateStore>();
        workflow::WorkflowEngineConfig wf_cfg;
        wf_cfg.worker_count =
            static_cast<std::size_t>(std::max(1, config.workflowWorkerCount()));
        wf_cfg.default_node_timeout =
            std::chrono::milliseconds(config.workflowNodeTimeoutMs());
        wf_cfg.max_concurrent_runs = config.workflowMaxConcurrentRuns();
        aegisgate::ToolSandbox* wf_sandbox =
            pipeline_ ? pipeline_->tool_sandbox.get() : nullptr;
        workflow_engine_ = std::make_unique<workflow::WorkflowEngine>(
            wf_cfg, wf_sandbox, workflow_state_store_.get());
        spdlog::info("Workflow endpoint engine wired (state_store=memory, "
                     "tool_sandbox={})",
                     wf_sandbox ? "active" : "absent(tool nodes DLQ)");
    }

    initialized_ = true;
    spdlog::info("GatewayRuntime initialized: {} providers, {} models, auth={}",
                 connector_registry_.all().size(),
                 registeredModels().size(),
                 config.authEnabled() ? "on" : "off");
}

void GatewayRuntime::loadProviders(const std::string& models_path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(models_path);
    } catch (const YAML::Exception& e) {
        spdlog::error("Failed to load models config: {}", e.what());
        return;
    }

    if (auto default_model = root["default_model"]) {
        connector_registry_.setDefaultModel(default_model.as<std::string>());
    }

    auto providers = root["providers"];
    if (!providers || !providers.IsSequence()) return;

    for (const auto& p : providers) {
        ProviderConfig pc;
        pc.name = p["name"].as<std::string>("");
        pc.type = p["type"].as<std::string>("");
        pc.base_url = p["base_url"].as<std::string>("");
        pc.timeout_ms = p["timeout_ms"].as<int>(30000);
        pc.max_retries = p["max_retries"].as<int>(2);

        if (auto keys_node = p["api_keys"]) {
            for (const auto& k : keys_node) {
                std::string raw_key;
                int weight = 1;
                if (k.IsMap()) {
                    raw_key = resolveEnvVar(k["key"].as<std::string>(""));
                    weight = k["weight"].as<int>(1);
                } else {
                    raw_key = resolveEnvVar(k.as<std::string>(""));
                }
                if (!raw_key.empty()) {
                    pc.api_keys.emplace_back(raw_key, weight);
                }
            }
        }

        if (auto models = p["models"]) {
            for (const auto& m : models) {
                ModelInfo mi;
                mi.id = m["id"].as<std::string>("");
                mi.provider = pc.name;
                mi.cost_per_1k_input = m["cost_per_1k_input"].as<double>(0.0);
                mi.cost_per_1k_output = m["cost_per_1k_output"].as<double>(0.0);
                mi.max_context_tokens = m["max_tokens"].as<int>(4096);
                if (auto tags = m["tags"]) {
                    for (const auto& t : tags) {
                        mi.tags.push_back(t.as<std::string>());
                    }
                }
                pc.models.push_back(mi);
                connector_registry_.registerModelInfo(mi);
            }
        }

        auto connector = ConnectorFactory::defaults().create(pc);
        if (connector) {
            connector_registry_.registerConnector(std::move(connector));
        } else {
            spdlog::warn("Unknown provider type: {}", pc.type);
        }
    }

    // Set up fallback chains
    if (auto chains = root["fallback_chains"]) {
        for (auto it = chains.begin(); it != chains.end(); ++it) {
            auto primary = it->first.as<std::string>();
            std::vector<std::string> fallbacks;
            auto fb_node = it->second;
            for (const auto& f : fb_node) {
                fallbacks.push_back(f.as<std::string>());
            }
            fallback_->setChain(primary, fallbacks);
        }
    }
}

bool GatewayRuntime::validateApiKey(const std::string& key) const {
    if (auth_service_ && auth_service_->isRbacEnabled()) {
        return true;  // RBAC-enabled: defer to auth_service_->resolve() in processRequest
    }
    if (!config_->authEnabled()) return true;
    if (valid_api_key_hashes_.empty()) return false;
    auto hash = crypto::sha256(key);
    for (const auto& stored : valid_api_key_hashes_) {
        if (crypto::constantTimeEquals(hash, stored)) return true;
    }
    return false;
}

bool GatewayRuntime::authorizeApiRequest(const std::string& key) const {
    // P0-A (TASK-20260701-01): close the RBAC fail-open on auxiliary read
    // endpoints. validateApiKey() returns true unconditionally under RBAC on
    // the assumption that a later resolve() enforces auth — true for the chat
    // and proxy paths, but listModels/metrics/cacheStats never resolve, so
    // RBAC-on turned them into unauthenticated info-leak surfaces. Resolve here
    // so the token is actually verified.
    if (auth_service_ && auth_service_->isRbacEnabled()) {
        return auth_service_->resolve(key).has_value();
    }
    return validateApiKey(key);
}

void GatewayRuntime::setAuthService(std::unique_ptr<AuthService> svc) {
    auth_service_ = std::move(svc);
}

bool GatewayRuntime::validateAdminKey(const std::string& key) const {
    if (!config_) return false;
    auto admin_key = config_->adminApiKey();
    if (admin_key.empty()) return false;
    return crypto::constantTimeEquals(
        crypto::sha256(key), crypto::sha256(admin_key));
}

void GatewayRuntime::ensureFeedbackMetricsSubscriber() {
    auto& sub = feedbackSubscriberSingleton();
    if (!sub) {
        sub = std::make_unique<MetricsFeedbackSubscriber>(
            MetricsRegistry::instance().feedbackEventsTotal());
        sub->attach(FeedbackBus::instance());
    }
}

void GatewayRuntime::beginShutdown() {
    shutting_down_.store(true, std::memory_order_release);
    spdlog::info("Shutdown initiated — rejecting new requests");
}

bool GatewayRuntime::isShuttingDown() const {
    return shutting_down_.load(std::memory_order_acquire);
}

void GatewayRuntime::shutdown() {
    spdlog::info("Shutting down components...");

    // Phase 11.0 — Drain the FeedbackBus before audit/persistent stores so any
    // subscriber-emitted audit / persistence write still has its dependencies.
    {
        auto& bus = FeedbackBus::instance();
        if (bus.stats().subscriber_count > 0) {
            spdlog::info("Flushing FeedbackBus...");
            if (!bus.flush(std::chrono::milliseconds{2000})) {
                spdlog::warn("FeedbackBus flush timed out after 2s; "
                             "proceeding to shutdown anyway");
            }
            bus.shutdown();
        }
    }

    if (pipeline_ && pipeline_->audit_logger) {
        spdlog::info("Flushing audit logger...");
        pipeline_->audit_logger->flush();
        pipeline_->audit_logger->shutdown();
    }

    if (pipeline_ && pipeline_->persistent_store) {
        spdlog::info("Closing persistent store...");
        try {
            pipeline_->persistent_store->close();
        } catch (const std::exception& e) {
            spdlog::error("Error closing persistent store: {}", e.what());
        }
    }

    if (pipeline_ && pipeline_->cache_store) {
        spdlog::info("Closing cache store...");
        try {
            pipeline_->cache_store->close();
        } catch (const std::exception& e) {
            spdlog::error("Error closing cache store: {}", e.what());
        }
    }

    spdlog::info("Shutdown complete");
}

void GatewayRuntime::reinitializeForTesting() {
    if (initialized_) {
        if (!isShuttingDown()) {
            beginShutdown();
        }
        shutdown();
    }
    shutting_down_.store(false);
    initialized_ = false;
    connector_registry_ = ConnectorRegistry{};

    // Phase 11.5 (TASK-20260518-02 E4.2) — Autonomy teardown.
    // Order matters: budget_guard_stage_raw_ points INTO pipeline_->inbound,
    // and cost_autonomy_applier_ holds a non-owning shared_ptr<MLRouter>
    // into router_. Drop these first so the subsequent pipeline_/router_
    // resets don't leave dangling observer pointers behind for the next
    // initialize() to trip over.
    budget_guard_stage_raw_ = nullptr;
    cost_autonomy_applier_.reset();
    guard_admin_controller_.reset();
    guard_model_applier_.reset();
    guard_anomaly_detector_.reset();
    guard_feedback_rate_limiter_.reset();
    workflow_applier_.reset();
    workflow_engine_.reset();
    workflow_state_store_.reset();
    guard_feedback_sink_.reset();
    guard_model_registry_.reset();
    approval_workflow_.reset();
    approval_queue_.reset();
    ml_router_raw_ = nullptr;

    pipeline_.reset();
    router_.reset();
    fallback_.reset();
    std::atomic_store(&rate_limiter_, std::shared_ptr<RateLimiter>{});
    valid_api_key_hashes_.clear();
    auth_service_.reset();
    scim_service_.reset();
#ifdef AEGISGATE_ENABLE_REDIS
    redis_state_store_.reset();
#endif
    request_counter_.store(0);

    if (config_) {
        initialize(*config_);
    }
}

std::shared_ptr<RateLimiter> GatewayRuntime::rateLimiterSnapshot() const {
    return std::atomic_load(&rate_limiter_);
}

bool GatewayRuntime::reloadConfig() {
    // HOT RELOAD SCOPE: This method refreshes a subset of runtime configuration
    // without process restart. Updated: rate limiter, API key hashes, security
    // rules (injection/pii/topic/custom). Custom rule_engine reload now prefers
    // the store's GLOBAL (empty-tenant) active rule set (admin activateRuleSet
    // takes effect), falling back to config/rules/custom_rules.yaml (P1-4).
    // NOT updated: providers, connectors, router type, fallback chains, pipeline
    // stages, ONNX models. Full provider/connector refresh requires restart.
    // KNOWN LIMITATION: RuleEngine is a single global stage; per-tenant admin
    // rule sets are staged/versioned but NOT enforced per-request yet (backlog).
    if (!config_) return false;
    bool ok = const_cast<Config*>(config_)->reload();
    if (!ok) {
        spdlog::error("Config reload failed — keeping previous configuration");
        return false;
    }

    auto new_rl = std::make_shared<RateLimiter>(
        RateLimiter::Config{config_->rateLimitMaxTokens(),
                            config_->rateLimitRefillRate()});
    std::atomic_store(&rate_limiter_, new_rl);

    valid_api_key_hashes_.clear();
    for (const auto& key : config_->authApiKeys()) {
        valid_api_key_hashes_.push_back(crypto::sha256(key));
    }

    reloadSecurityRules();

    spdlog::info("Config reloaded: rate limiter recreated, API key hashes refreshed, security rules reloaded");
    return true;
}

void GatewayRuntime::reloadSecurityRules() {
    if (!pipeline_) return;

    if (pipeline_->injection_detector) {
        pipeline_->injection_detector->reloadPatterns("config/rules/injection_patterns.yaml");
    }
    if (pipeline_->pii_filter) {
        pipeline_->pii_filter->reloadPatterns("config/rules/pii_patterns.yaml");
    }
    if (pipeline_->topic_guard) {
        pipeline_->topic_guard->reloadConfig("config/rules/topic_whitelist.yaml");
    }
    if (pipeline_->rule_engine) {
        // P1-4（TASK-20260702-01）：热重载时同样优先 store 全局作用域激活规则集
        // （后台 activateRuleSet 生效），无激活集时回退 YAML。与装配路径一致。
        if (pipeline_->persistent_store) {
            pipeline_->rule_engine->reloadFromStoreOrYaml(
                *pipeline_->persistent_store, "", "config/rules/custom_rules.yaml");
            // P2-4（SR-4）：热重载时同步刷新各租户激活集桶。
            pipeline_->rule_engine->loadAllTenants(*pipeline_->persistent_store);
        } else {
            pipeline_->rule_engine->reloadRules("config/rules/custom_rules.yaml");
        }
    }
}

void GatewayRuntime::refreshTenantRules(const std::string& tenant_id) {
    if (!pipeline_ || !pipeline_->rule_engine || !pipeline_->persistent_store) {
        return;
    }
    pipeline_->rule_engine->reloadTenant(*pipeline_->persistent_store, tenant_id);
}

std::string GatewayRuntime::generateRequestId() {
    auto ts = std::chrono::system_clock::now().time_since_epoch().count();
    auto seq = request_counter_.fetch_add(1);
    return "chatcmpl-" + std::to_string(ts) + "-" + std::to_string(seq);
}

GatewayRuntime::TenantPolicy GatewayRuntime::resolveTenantPolicy(
    const RequestContext& ctx) const {
    TenantPolicy policy;
    if (!ctx.auth_context || !pipeline_ || !pipeline_->persistent_store)
        return policy;

    policy.tenant = pipeline_->persistent_store->getTenant(
        ctx.auth_context->tenant_id);

    if (policy.tenant && policy.tenant->rate_limit_tokens > 0) {
        policy.rate_limiter = std::make_shared<RateLimiter>(RateLimiter::Config{
            static_cast<double>(policy.tenant->rate_limit_tokens),
            policy.tenant->rate_limit_refill > 0
                ? policy.tenant->rate_limit_refill : 1.0});
    }
    return policy;
}

std::optional<GatewayError> GatewayRuntime::checkTenantCostLimits(
    const RequestContext& ctx, const Tenant& tenant) const {
    if (!pipeline_ || !pipeline_->persistent_store) return std::nullopt;

    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm buf{};
    gmtime_r(&tt, &buf);

    if (tenant.daily_cost_limit > 0) {
        char day_start[32], day_end[32];
        strftime(day_start, sizeof(day_start), "%Y-%m-%dT00:00:00Z", &buf);
        strftime(day_end, sizeof(day_end), "%Y-%m-%dT23:59:59Z", &buf);
        double daily = pipeline_->persistent_store->getTenantCostInPeriod(
            ctx.auth_context->tenant_id, day_start, day_end);
        if (daily >= tenant.daily_cost_limit) {
            return GatewayError{429, toAegisCode(ErrorCode::CostLimitExceeded),
                toErrorType(ErrorCode::CostLimitExceeded),
                "Daily cost limit exceeded", ""};
        }
    }

    if (tenant.monthly_cost_limit > 0) {
        char month_start[32], month_end[32];
        strftime(month_start, sizeof(month_start), "%Y-%m-01T00:00:00Z", &buf);
        struct tm next_month = buf;
        next_month.tm_mon++;
        if (next_month.tm_mon > 11) { next_month.tm_mon = 0; next_month.tm_year++; }
        strftime(month_end, sizeof(month_end), "%Y-%m-01T00:00:00Z", &next_month);
        double monthly = pipeline_->persistent_store->getTenantCostInPeriod(
            ctx.auth_context->tenant_id, month_start, month_end);
        if (monthly >= tenant.monthly_cost_limit) {
            return GatewayError{429, toAegisCode(ErrorCode::CostLimitExceeded),
                toErrorType(ErrorCode::CostLimitExceeded),
                "Monthly cost limit exceeded", ""};
        }
    }
    return std::nullopt;
}

bool GatewayRuntime::isModelAllowed(const Tenant& tenant,
                                     const std::string& model) {
    if (tenant.model_whitelist.empty()) return true;
    for (const auto& m : tenant.model_whitelist) {
        if (m == model) return true;
    }
    return false;
}

void GatewayRuntime::processStreamingRequest(
    ChatRequest request,
    const std::string& api_key,
    std::function<void(const StreamDelta&)> onDelta,
    std::function<void(const TokenUsage&, int tokens_saved)> onDone,
    std::function<void(const GatewayError&)> onError,
    const std::unordered_map<std::string, std::string>& request_headers) {

    auto& metrics = MetricsRegistry::instance();
    metrics.activeConnections().inc();
    auto start = std::chrono::steady_clock::now();

    auto ctx = std::make_shared<RequestContext>();
    ctx->request_id = generateRequestId();
    ctx->api_key = api_key;
    ctx->chat_request = std::move(request);
    ctx->request_headers = request_headers;
    ctx->start_time = start;
    ctx->is_streaming = true;
    ctx->has_tools = hasToolsRequest(ctx->chat_request);

    // --- RBAC: resolve AuthContext (streaming) ---
    if (auth_service_) {
        auto auth_ctx = auth_service_->resolve(api_key);
        if (auth_ctx) {
            ctx->auth_context = *auth_ctx;
            ctx->tenant_id = auth_ctx->tenant_id;
        } else if (auth_service_->isRbacEnabled()) {
            metrics.activeConnections().dec();
            onError({401, toAegisCode(ErrorCode::InvalidApiKey),
                     toErrorType(ErrorCode::InvalidApiKey),
                     toDefaultMessage(ErrorCode::InvalidApiKey), ""});
            return;
        }
    }

    // --- Abuse detection (pre-pipeline, parity with non-streaming) ---
    double abuse_cost_multiplier = 1.0;  // P1-C: Throttle tightens rate budget
    if (abuse_detector_) {
        auto text = extractAbuseText(ctx->chat_request.messages,
                                     abuse_detector_->similarityMaxContentBytes());
        auto action = abuse_detector_->observe(api_key, text);
        if (action == AbuseDetector::Action::Block) {
            metrics.activeConnections().dec();
            onError({429, toAegisCode(ErrorCode::AbuseDetected),
                     toErrorType(ErrorCode::AbuseDetected),
                     toDefaultMessage(ErrorCode::AbuseDetected), ""});
            return;
        }
        abuse_cost_multiplier = abuseCostMultiplier(abuse_detector_.get(), action);
    }

#ifdef AEGISGATE_ENABLE_OTEL
    if (Tracing::instance().isEnabled()) {
        auto current_span = otel_trace::Tracer::GetCurrentSpan();
        if (current_span && current_span->GetContext().IsValid()) {
            ctx->root_span = current_span;
            ctx->trace_ctx = opentelemetry::context::RuntimeContext::GetCurrent();
        }
    }
#endif

    auto inbound_result = pipeline_->inbound.execute(*ctx);
    if (inbound_result == PipelineResult::Rejected) {
        if (abuse_detector_) {
            abuse_detector_->recordRejection(api_key);
        }
        metrics.guardrailBlocksTotal().inc();
        metrics.requestsTotal().inc(
            {{{{"model", ctx->chat_request.model}, {"status", "rejected"}}}});
        metrics.activeConnections().dec();
        ErrorCode reject_code = rejectStageToErrorCode(ctx->reject_stage);
        onError({403, toAegisCode(reject_code),
                 toErrorType(reject_code),
                 toDefaultMessage(reject_code), ""});
        return;
    }
    if (inbound_result == PipelineResult::Error) {
        metrics.activeConnections().dec();
        onError({500, toAegisCode(ErrorCode::InternalError),
                 toErrorType(ErrorCode::InternalError),
                 "Internal pipeline error", ""});
        return;
    }
    if (ctx->tokens_saved_compression > 0) {
        metrics.tokensSavedTotal().inc(
            {{{{"method", "compression"}}}},
            static_cast<double>(ctx->tokens_saved_compression));
        // SavingsAggregator hook（noexcept，nullptr-safe），见 SR-NEW4
        if (savings_aggregator_) {
            const std::string tenant =
                ctx->auth_context ? ctx->auth_context->tenant_id : std::string();
            const std::string& model =
                !ctx->target_model.empty() ? ctx->target_model : ctx->chat_request.model;
            savings_aggregator_->recordCompression(
                model, ctx->tokens_saved_compression, tenant);
        }
    }

    if (inbound_result == PipelineResult::ShortCircuited && ctx->cache_hit) {
        metrics.cacheHitsTotal().inc();
        // P1-10: derive cache savings from the prompt directly so they are not
        // gated on the default-off compressor's tokens_estimated.
        int saved_tokens = cacheSavedPromptTokens(*ctx);
        int response_tokens = TokenEstimator::estimateTokens(ctx->cached_response);
        if (saved_tokens > 0) {
            metrics.tokensSavedTotal().inc(
                {{{{"method", "cache"}}}},
                static_cast<double>(saved_tokens));
        }
        // SavingsAggregator hook（noexcept）
        if (savings_aggregator_) {
            const std::string tenant =
                ctx->auth_context ? ctx->auth_context->tenant_id : std::string();
            const std::string& model =
                !ctx->target_model.empty() ? ctx->target_model : ctx->chat_request.model;
            savings_aggregator_->recordCacheHit(
                model, saved_tokens, response_tokens, tenant);
        }
        auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        metrics.requestDuration().observe(elapsed);
        metrics.requestsTotal().inc(
            {{{{"model", ctx->target_model}, {"status", "cache_hit"}}}});
        metrics.activeConnections().dec();
        StreamDelta cache_delta;
        cache_delta.content = ctx->cached_response;
        onDelta(cache_delta);
        // Report cache savings in the SSE metadata event (consistent with the
        // non-stream X-AegisGate-Tokens-Saved header and the cache metric).
        onDone({0, 0, 0}, saved_tokens);
        return;
    }

    auto rl_key = api_key.empty() ? "anonymous" : api_key;
    if (ctx->auth_context && !ctx->auth_context->tenant_id.empty()) {
        rl_key = ctx->auth_context->tenant_id;
    }
    double estimated_tokens = 1.0;
    for (const auto& msg : ctx->chat_request.messages) {
        estimated_tokens += static_cast<double>(msg.content.size()) / 3.0;
    }
    estimated_tokens *= abuse_cost_multiplier;  // P1-C: Throttle tightens budget

    auto policy = resolveTenantPolicy(*ctx);
    auto rl = policy.rate_limiter ? policy.rate_limiter
                                  : std::atomic_load(&rate_limiter_);

    if (!rl->allow(rl_key, estimated_tokens)) {
        metrics.rateLimitedTotal().inc();
        metrics.requestsTotal().inc(
            {{{{"model", ctx->chat_request.model}, {"status", "rate_limited"}}}});
        metrics.activeConnections().dec();
        onError({429, toAegisCode(ErrorCode::RateLimitExceeded),
                 toErrorType(ErrorCode::RateLimitExceeded),
                 toDefaultMessage(ErrorCode::RateLimitExceeded), ""});
        return;
    }

    if (policy.tenant) {
        if (auto err = checkTenantCostLimits(*ctx, *policy.tenant)) {
            metrics.activeConnections().dec();
            onError(*err);
            return;
        }
    }

    ScopedSpan route_span;
#ifdef AEGISGATE_ENABLE_OTEL
    if (Tracing::instance().isEnabled() && ctx->root_span)
        route_span = ScopedSpan("aegisgate.route", ctx->trace_ctx);
#endif
    auto model = router_->selectModel(*ctx, connector_registry_);
    route_span.setAttribute("aegisgate.selected_model", model);
    route_span.end();

    if (model.empty()) {
        metrics.activeConnections().dec();
        onError({503, toAegisCode(ErrorCode::NoModelAvailable),
                 toErrorType(ErrorCode::NoModelAvailable),
                 toDefaultMessage(ErrorCode::NoModelAvailable), ""});
        return;
    }
    ctx->target_model = model;
    ctx->stream_model = model;

    // P1-9: attribute A/B experiment assignment (labeled metric). The variant
    // header is not surfaced on the SSE path (headers are flushed before the
    // body), but the assignment is still counted for analysis.
    recordAbAttribution(*ctx, metrics);

    if (policy.tenant && !isModelAllowed(*policy.tenant, model)) {
        metrics.activeConnections().dec();
        onError({403, toAegisCode(ErrorCode::ModelNotAllowed),
                 toErrorType(ErrorCode::ModelNotAllowed),
                 toDefaultMessage(ErrorCode::ModelNotAllowed), ""});
        return;
    }

    // --- Gateway-level request timeout check ---
    if (config_->requestTimeoutSeconds() > 0) {
        auto elapsed_so_far = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed_so_far >= config_->requestTimeoutSeconds()) {
            metrics.activeConnections().dec();
            onError({504, toAegisCode(ErrorCode::UpstreamTimeout),
                     toErrorType(ErrorCode::UpstreamTimeout),
                     "Gateway request timeout exceeded before upstream call", ""});
            return;
        }
    }

    auto pipeline_ptr = pipeline_.get();
    auto semantic_cache = pipeline_->semantic_cache;
    auto* router_raw = router_.get();

    auto upstream_stream_span = std::make_shared<ScopedSpan>();
#ifdef AEGISGATE_ENABLE_OTEL
    if (Tracing::instance().isEnabled() && ctx->root_span)
        *upstream_stream_span = ScopedSpan("aegisgate.upstream.stream",
            ctx->trace_ctx,
            {{"aegisgate.model", std::string(ctx->target_model)}});
#endif
    auto chunk_count = std::make_shared<int>(0);
    auto upstream_start = std::chrono::steady_clock::now();

    fallback_->streamWithFallback(
        ctx->chat_request, ctx->target_model,
        [ctx, pipeline_ptr, onDelta, upstream_stream_span, chunk_count
        ](const StreamDelta& delta) {
            ctx->accumulated_response += delta.content;
            if (!delta.tool_calls_delta.is_null()) {
                if (ctx->accumulated_tool_calls.is_null())
                    ctx->accumulated_tool_calls = nlohmann::json::array();
                for (const auto& tc : delta.tool_calls_delta)
                    ctx->accumulated_tool_calls.push_back(tc);
            }
            if (*upstream_stream_span) {
                ++(*chunk_count);
                if (*chunk_count == 1) {
                    upstream_stream_span->addEvent("first_chunk");
                }
            }
            if (ctx->stream_rejected) return;
            if (!delta.content.empty()) {
                auto [result, filtered] = pipeline_ptr->outbound.executeChunk(
                    *ctx, delta.content);
                if (result == PipelineResult::Rejected) {
                    ctx->stream_rejected = true;
                    return;
                }
                if (!filtered.empty()) {
                    StreamDelta filtered_delta = delta;
                    filtered_delta.content = filtered;
                    onDelta(filtered_delta);
                } else {
                    onDelta(delta);
                }
            } else {
                onDelta(delta);
            }
        },
        [ctx, pipeline_ptr, semantic_cache, router_raw, onDone, onError,
         &metrics, start, upstream_start, upstream_stream_span, chunk_count
        ](const TokenUsage& usage) {
            ctx->token_usage = usage;
            {
                auto ue = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - upstream_start).count();
                metrics.upstreamDuration().observe(ue,
                    {{{{"model", ctx->target_model}}}});
            }
            auto outbound_result = pipeline_ptr->outbound.execute(*ctx);
            if (ctx->stream_rejected
                || outbound_result == PipelineResult::Rejected) {
                metrics.guardrailBlocksTotal().inc();
                metrics.requestsTotal().inc(
                    {{{{"model", ctx->target_model}, {"status", "rejected"}}}});
                metrics.activeConnections().dec();
                onError({403, toAegisCode(ErrorCode::ContentFiltered),
                         toErrorType(ErrorCode::ContentFiltered),
                         toDefaultMessage(ErrorCode::ContentFiltered), ""});
                return;
            }

            if (*upstream_stream_span) {
                upstream_stream_span->addEvent("stream_complete");
                upstream_stream_span->setAttribute("aegisgate.chunk_count",
                    *chunk_count);
                upstream_stream_span->setAttribute("aegisgate.tokens.total",
                    static_cast<int>(usage.total_tokens));
                upstream_stream_span->end();
            }

            if (semantic_cache && !ctx->chat_request.messages.empty()
                && !ctx->has_tools) {
                // P0-1 / SR-1: tenant-scope the streaming cache write so the
                // V2 read path (which mixes in tenant_id) can match it.
                std::string conv_id = semantic_cache->conversationIdResolver()
                    ? semantic_cache->conversationIdResolver()->resolve(
                          ctx->chat_request)
                    : "";
                semantic_cache->putFromContext(
                    ctx->chat_request.messages,
                    ctx->accumulated_response,
                    ctx->target_model,
                    ctx->tenant_id,
                    conv_id);
            }

            if (router_raw) {
                auto elapsed_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - upstream_start).count();
                router_raw->reportOutcome(ctx->target_model, elapsed_ms, true);
            }

            auto elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
            metrics.requestDuration().observe(elapsed);
            metrics.requestsTotal().inc(
                {{{{"model", ctx->target_model}, {"status", "ok"}}}});
            metrics.tokensTotal().inc({}, static_cast<double>(usage.total_tokens));
            metrics.activeConnections().dec();
            onDone(usage, ctx->tokens_saved_compression);
        },
        [ctx, router_raw, onError, &metrics, upstream_start,
         upstream_stream_span
        ](const GatewayError& err) {
            {
                auto ue = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - upstream_start).count();
                metrics.upstreamDuration().observe(ue);
            }
            if (*upstream_stream_span) {
                upstream_stream_span->setError(err.message);
                upstream_stream_span->end();
            }
            if (router_raw) {
                auto elapsed_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - upstream_start).count();
                router_raw->reportOutcome(ctx->target_model, elapsed_ms, false);
            }
            metrics.fallbackTotal().inc();
            metrics.requestsTotal().inc(
                {{{{"model", ctx->target_model}, {"status", "error"}}}});
            metrics.activeConnections().dec();
            onError(err);
        });
}

AgentResult GatewayRuntime::processAgentRequest(const std::string& model,
                                                const std::string& input,
                                                int max_steps,
                                                const std::string& api_key) {
    AgentResult result;

    // SR-1：认证优先（与 /v1/chat/completions 同源 validateApiKey）。
    if (!validateApiKey(api_key)) {
        result.error = {401, toAegisCode(ErrorCode::InvalidApiKey),
                        toErrorType(ErrorCode::InvalidApiKey),
                        toDefaultMessage(ErrorCode::InvalidApiKey), ""};
        return result;
    }

    // 端点 opt-in（默认关，未审计不暴露执行面）。
    if (!config_ || !config_->agentEndpointEnabled()) {
        result.error = {501, "agent_endpoint_disabled", "not_implemented",
                        "Agent endpoint is disabled", ""};
        return result;
    }

    // 引擎可用性：Orchestrator 仅在 AgentOrchestration 特性装配时存在。
    if (!pipeline_ || !pipeline_->orchestrator || !pipeline_->tool_registry ||
        !pipeline_->tool_sandbox) {
        result.error = {503, "agent_unavailable", "server_error",
                        "Agent orchestration is not available", ""};
        return result;
    }
    if (!fallback_) {
        result.error = {503, "agent_unavailable", "server_error",
                        "Model backend is not available", ""};
        return result;
    }

    // per-request Orchestrator：与共享 pipeline_ 同源 registry/sandbox（SR-1 同源
    // 白名单），但配置本地化以支持 per-request max_steps 且天然线程安全（不改共享
    // 实例状态）。
    OrchestrationConfig cfg;
    cfg.max_steps = config_->agentMaxSteps();
    cfg.max_total_timeout_ms = config_->agentMaxTotalTimeoutMs();
    if (max_steps > 0 && max_steps < cfg.max_steps) {
        cfg.max_steps = max_steps;  // 只允许收紧，不允许放宽（SR-3 预算硬约束）
    }
    Orchestrator orch(cfg);
    orch.setToolRegistry(pipeline_->tool_registry.get());
    orch.setToolSandbox(pipeline_->tool_sandbox.get());
    // SR-3：LlmFn = fallback lambda，继承熔断/多 key/限流/成本核算。
    FallbackManager* fb = fallback_.get();
    orch.setLlmFn([fb](const ChatRequest& r) -> ChatResponse {
        return fb->executeWithFallback(r, r.model);
    });

    result.run = orch.run(input, model);
    result.success = result.run.success;
    if (!result.success && result.run.error.empty()) {
        result.error = {500, "agent_error", "server_error",
                        "Agent orchestration failed", ""};
    }
    return result;
}

WorkflowResult GatewayRuntime::processWorkflowRequest(
    const nlohmann::json& workflow_json,
    const nlohmann::json& context,
    const std::string& api_key) {
    WorkflowResult result;

    // SR-4：认证优先。
    if (!validateApiKey(api_key)) {
        result.error = {401, toAegisCode(ErrorCode::InvalidApiKey),
                        toErrorType(ErrorCode::InvalidApiKey),
                        toDefaultMessage(ErrorCode::InvalidApiKey), ""};
        return result;
    }
    if (!config_ || !config_->workflowEndpointEnabled()) {
        result.error = {501, "workflow_endpoint_disabled", "not_implemented",
                        "Workflow endpoint is disabled", ""};
        return result;
    }
    if (!workflow_engine_) {
        result.error = {503, "workflow_unavailable", "server_error",
                        "Workflow engine is not available", ""};
        return result;
    }

    // DSL 校验（结构合法性；环/沙箱绕过不变式由 execute() 内部强制）。
    auto dsl_opt = workflow::fromJson(workflow_json);
    if (!dsl_opt) {
        result.error = {400, "invalid_workflow_dsl", "invalid_request_error",
                        "Malformed workflow DSL", ""};
        return result;
    }

    result.run_id = generateRequestId();
    result.exec = workflow_engine_->execute(*dsl_opt, result.run_id,
                                            context.is_null() ? nlohmann::json::object()
                                                              : context);
    result.success = result.exec.ok;
    if (!result.success) {
        // 背压拒绝 → 429；其余执行失败以 200 携带 final_status 由 controller 决定。
        if (result.exec.error_message == "backpressure_rejected") {
            result.error = {429, "workflow_backpressure", "rate_limit_error",
                            "Too many concurrent workflow runs", ""};
        }
    }
    return result;
}

ProcessResult GatewayRuntime::processRequest(
    ChatRequest request,
    const std::string& api_key,
    const std::unordered_map<std::string, std::string>& request_headers) {
    auto& metrics = MetricsRegistry::instance();
    metrics.activeConnections().inc();
    auto start = std::chrono::steady_clock::now();

    RequestContext ctx;
    ctx.request_id = generateRequestId();
    ctx.api_key = api_key;
    ctx.chat_request = std::move(request);
    ctx.request_headers = request_headers;
    ctx.start_time = start;
    ctx.has_tools = hasToolsRequest(ctx.chat_request);

    // --- RBAC: resolve AuthContext ---
    if (auth_service_) {
        auto auth_ctx = auth_service_->resolve(api_key);
        if (auth_ctx) {
            ctx.auth_context = *auth_ctx;
            ctx.tenant_id = auth_ctx->tenant_id;
        } else if (auth_service_->isRbacEnabled()) {
            metrics.activeConnections().dec();
            return {false, {}, {401, toAegisCode(ErrorCode::InvalidApiKey),
                    toErrorType(ErrorCode::InvalidApiKey),
                    toDefaultMessage(ErrorCode::InvalidApiKey), ""}};
        }
    }

    // --- Abuse detection (pre-pipeline) ---
    double abuse_cost_multiplier = 1.0;  // P1-C: Throttle tightens rate budget
    if (abuse_detector_) {
        auto text = extractAbuseText(ctx.chat_request.messages,
                                     abuse_detector_->similarityMaxContentBytes());
        auto action = abuse_detector_->observe(api_key, text);
        if (action == AbuseDetector::Action::Block) {
            metrics.activeConnections().dec();
            return {false, {}, {429, toAegisCode(ErrorCode::AbuseDetected),
                    toErrorType(ErrorCode::AbuseDetected),
                    toDefaultMessage(ErrorCode::AbuseDetected), ""}};
        }
        abuse_cost_multiplier = abuseCostMultiplier(abuse_detector_.get(), action);
    }

#ifdef AEGISGATE_ENABLE_OTEL
    if (Tracing::instance().isEnabled()) {
        auto current_span = otel_trace::Tracer::GetCurrentSpan();
        if (current_span && current_span->GetContext().IsValid()) {
            ctx.root_span = current_span;
            ctx.trace_ctx = opentelemetry::context::RuntimeContext::GetCurrent();
        }
    }
#endif

    // --- Inbound pipeline (guardrails + cache) ---
    auto inbound_result = pipeline_->inbound.execute(ctx);

    if (inbound_result == PipelineResult::Rejected) {
        if (abuse_detector_) {
            abuse_detector_->recordRejection(api_key);
        }
        metrics.guardrailBlocksTotal().inc();
        metrics.requestsTotal().inc(
            {{{{"model", ctx.chat_request.model}, {"status", "rejected"}}}});
        metrics.activeConnections().dec();
        ErrorCode reject_code = rejectStageToErrorCode(ctx.reject_stage);
        return {false, {}, {403, toAegisCode(reject_code),
                toErrorType(reject_code),
                toDefaultMessage(reject_code), ""}};
    }

    if (inbound_result == PipelineResult::Error) {
        metrics.activeConnections().dec();
        return {false, {}, {500, toAegisCode(ErrorCode::InternalError),
                toErrorType(ErrorCode::InternalError),
                "Internal pipeline error", ""}};
    }

    // Cache hit — return cached response directly
    if (ctx.tokens_saved_compression > 0) {
        metrics.tokensSavedTotal().inc(
            {{{{"method", "compression"}}}},
            static_cast<double>(ctx.tokens_saved_compression));
        // SavingsAggregator hook（noexcept），见 SR-NEW4
        if (savings_aggregator_) {
            const std::string tenant =
                ctx.auth_context ? ctx.auth_context->tenant_id : std::string();
            const std::string& model =
                !ctx.target_model.empty() ? ctx.target_model : ctx.chat_request.model;
            savings_aggregator_->recordCompression(
                model, ctx.tokens_saved_compression, tenant);
        }
    }

    if (inbound_result == PipelineResult::ShortCircuited && ctx.cache_hit) {
        metrics.cacheHitsTotal().inc();
        // P1-10: derive cache savings from the prompt directly so they are not
        // gated on the default-off compressor's tokens_estimated.
        int saved_tokens = cacheSavedPromptTokens(ctx);
        int response_tokens = TokenEstimator::estimateTokens(ctx.cached_response);
        if (saved_tokens > 0) {
            metrics.tokensSavedTotal().inc(
                {{{{"method", "cache"}}}},
                static_cast<double>(saved_tokens));
        }
        // SavingsAggregator hook（noexcept）
        if (savings_aggregator_) {
            const std::string tenant =
                ctx.auth_context ? ctx.auth_context->tenant_id : std::string();
            const std::string& model =
                !ctx.target_model.empty() ? ctx.target_model : ctx.chat_request.model;
            savings_aggregator_->recordCacheHit(
                model, saved_tokens, response_tokens, tenant);
        }
        auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        metrics.requestDuration().observe(elapsed);
        metrics.requestsTotal().inc(
            {{{{"model", ctx.chat_request.model}, {"status", "cache_hit"}}}});
        metrics.activeConnections().dec();

        ChatResponse cached_resp;
        cached_resp.id = ctx.request_id;
        cached_resp.model = ctx.chat_request.model;
        cached_resp.content = ctx.cached_response;
        cached_resp.finish_reason = "stop";
        // Surface the cache savings to the client so api_controller emits the
        // X-AegisGate-Tokens-Saved header (kept consistent with the
        // tokens_saved_total{method=cache} metric above, i.e. prompt tokens).
        ProcessResult cache_ok{true, cached_resp, {}, saved_tokens};
        cache_ok.response_headers = std::move(ctx.response_headers);
        return cache_ok;
    }

    // --- Tenant rate limiting (RBAC: use tenant_id as key) ---
    auto rl_key = api_key.empty() ? "anonymous" : api_key;
    if (ctx.auth_context && !ctx.auth_context->tenant_id.empty()) {
        rl_key = ctx.auth_context->tenant_id;
    }
    double estimated_tokens = 1.0;
    for (const auto& msg : ctx.chat_request.messages) {
        estimated_tokens += static_cast<double>(msg.content.size()) / 3.0;
    }
    estimated_tokens *= abuse_cost_multiplier;  // P1-C: Throttle tightens budget

    auto policy = resolveTenantPolicy(ctx);
    auto rl = policy.rate_limiter ? policy.rate_limiter
                                  : std::atomic_load(&rate_limiter_);

    if (!rl->allow(rl_key, estimated_tokens)) {
        metrics.rateLimitedTotal().inc();
        metrics.requestsTotal().inc(
            {{{{"model", ctx.chat_request.model}, {"status", "rate_limited"}}}});
        metrics.activeConnections().dec();
        return {false, {}, {429, toAegisCode(ErrorCode::RateLimitExceeded),
                toErrorType(ErrorCode::RateLimitExceeded),
                toDefaultMessage(ErrorCode::RateLimitExceeded), ""}};
    }

    if (policy.tenant) {
        if (auto err = checkTenantCostLimits(ctx, *policy.tenant)) {
            metrics.activeConnections().dec();
            return {false, {}, *err};
        }
    }

    // --- Gateway: routing ---
    ScopedSpan route_span;
#ifdef AEGISGATE_ENABLE_OTEL
    if (Tracing::instance().isEnabled() && ctx.root_span)
        route_span = ScopedSpan("aegisgate.route", ctx.trace_ctx);
#endif
    auto model = router_->selectModel(ctx, connector_registry_);
    route_span.setAttribute("aegisgate.selected_model", std::string(model));
    route_span.end();
    if (model.empty()) {
        metrics.activeConnections().dec();
        return {false, {}, {503, toAegisCode(ErrorCode::NoModelAvailable),
                toErrorType(ErrorCode::NoModelAvailable),
                toDefaultMessage(ErrorCode::NoModelAvailable), ""}};
    }
    ctx.target_model = model;

    // P1-9: attribute A/B experiment assignment (labeled metric + variant
    // header). No-op unless the router assigned a variant.
    recordAbAttribution(ctx, metrics);

    if (policy.tenant && !isModelAllowed(*policy.tenant, model)) {
        metrics.activeConnections().dec();
        return {false, {}, {403, toAegisCode(ErrorCode::ModelNotAllowed),
                toErrorType(ErrorCode::ModelNotAllowed),
                toDefaultMessage(ErrorCode::ModelNotAllowed), ""}};
    }

    // --- Gateway-level request timeout check ---
    if (config_->requestTimeoutSeconds() > 0) {
        auto elapsed_so_far = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed_so_far >= config_->requestTimeoutSeconds()) {
            metrics.activeConnections().dec();
            return {false, {}, {504, toAegisCode(ErrorCode::UpstreamTimeout),
                    toErrorType(ErrorCode::UpstreamTimeout),
                    "Gateway request timeout exceeded before upstream call", ""}};
        }
    }

    // --- Gateway: model invocation with fallback ---
    ScopedSpan upstream_span;
#ifdef AEGISGATE_ENABLE_OTEL
    if (Tracing::instance().isEnabled() && ctx.root_span)
        upstream_span = ScopedSpan("aegisgate.upstream", ctx.trace_ctx,
            {{"aegisgate.model", std::string(ctx.target_model)}});
#endif
    ChatResponse model_response;
    auto upstream_start = std::chrono::steady_clock::now();
    try {
        model_response = fallback_->executeWithFallback(
            ctx.chat_request, ctx.target_model);
        ctx.token_usage = model_response.usage;
        ctx.accumulated_response = model_response.content;
        model_response.id = ctx.request_id;
    } catch (const std::exception& e) {
        auto upstream_elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - upstream_start).count();
        metrics.upstreamDuration().observe(upstream_elapsed,
            {{{{"model", ctx.target_model}}}});
        upstream_span.setError(e.what());
        upstream_span.end();
        if (router_) {
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - upstream_start).count();
            router_->reportOutcome(ctx.target_model, static_cast<double>(elapsed_ms), false);
        }
        metrics.fallbackTotal().inc();
        metrics.requestsTotal().inc(
            {{{{"model", ctx.target_model}, {"status", "error"}}}});
        metrics.activeConnections().dec();
        // P0-3: distinguish an exhausted API-key pool (capacity/config issue,
        // AEGIS-4007/503) from a generic upstream failure (AEGIS-4004/502).
        if (dynamic_cast<const NoHealthyKeysError*>(&e)) {
            return {false, {}, {toHttpStatus(ErrorCode::NoHealthyKeys),
                    toAegisCode(ErrorCode::NoHealthyKeys),
                    toErrorType(ErrorCode::NoHealthyKeys),
                    toDefaultMessage(ErrorCode::NoHealthyKeys), e.what()}};
        }
        // P1-A: every candidate skipped because its circuit is open → 503.
        if (dynamic_cast<const CircuitBreakerOpenError*>(&e)) {
            return {false, {}, {toHttpStatus(ErrorCode::CircuitBreakerOpen),
                    toAegisCode(ErrorCode::CircuitBreakerOpen),
                    toErrorType(ErrorCode::CircuitBreakerOpen),
                    toDefaultMessage(ErrorCode::CircuitBreakerOpen), e.what()}};
        }
        // P1-A: pass the real upstream status through (429 back-off, 5xx
        // unavailable, 4xx client error, 408/504 timeout) instead of 502.
        if (auto* use = dynamic_cast<const UpstreamStatusError*>(&e)) {
            auto m = mapUpstreamStatus(use->upstreamStatus());
            return {false, {}, {m.http_status, toAegisCode(m.code),
                    toErrorType(m.code),
                    "Upstream returned " + std::to_string(use->upstreamStatus()),
                    e.what()}};
        }
        return {false, {}, {502, toAegisCode(ErrorCode::UpstreamError),
                toErrorType(ErrorCode::UpstreamError),
                toDefaultMessage(ErrorCode::UpstreamError), e.what()}};
    }
    {
        auto upstream_elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - upstream_start).count();
        metrics.upstreamDuration().observe(upstream_elapsed,
            {{{{"model", ctx.target_model}}}});
    }
    upstream_span.setAttribute("aegisgate.tokens.total",
        static_cast<int>(model_response.usage.total_tokens));
    upstream_span.end();

    // --- Router outcome reporting (P1-E: polymorphic — works for ML /
    // MultiObjective / Bandit-wrapped routers, not just a raw MLRouter) ---
    if (router_) {
        auto elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - upstream_start).count();
        router_->reportOutcome(ctx.target_model, elapsed_ms, true);
    }

    // --- Outbound pipeline (content filter, hallucination, cost, logger) ---
    // P0-3 / SR-3 (D3=B2): cache write moved to AFTER outbound so we cache the
    // filtered (redacted) content, not the raw upstream content.
    auto outbound_result = pipeline_->outbound.execute(ctx);
    if (outbound_result == PipelineResult::Rejected) {
        metrics.guardrailBlocksTotal().inc();
        metrics.requestsTotal().inc(
            {{{{"model", ctx.target_model}, {"status", "rejected"}}}});
        metrics.activeConnections().dec();
        return {false, {}, {403, toAegisCode(ErrorCode::ContentFiltered),
                toErrorType(ErrorCode::ContentFiltered),
                toDefaultMessage(ErrorCode::ContentFiltered), ""}};
    }

    // P0-3 / SR-3: reflect outbound redaction into the response body AND cache
    // the filtered content so cache hits also stay redacted. Same key
    // derivation (tenant + resolved conversation id) as the read path.
    finalizeNonStreamingResponse(model_response, pipeline_->semantic_cache, ctx);

    // --- Metrics ---
    auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    metrics.requestDuration().observe(elapsed);
    metrics.requestsTotal().inc(
        {{{{"model", ctx.target_model}, {"status", "ok"}}}});
    metrics.tokensTotal().inc({}, static_cast<double>(ctx.token_usage.total_tokens));
    metrics.activeConnections().dec();

    ProcessResult ok{true, model_response, {}, ctx.tokens_saved_compression};
    ok.response_headers = std::move(ctx.response_headers);
    return ok;
}

ProxyResult GatewayRuntime::processProxyRequest(ProxyRequest request,
                                                 const std::string& api_key) {
    // DESIGN DECISION: Proxy endpoints (/v1/embeddings, /v1/images/*, /v1/audio/*)
    // do not pass through the inbound content guardrail pipeline. Rationale:
    // 1. No LLM system prompt context → no prompt injection risk
    // 2. Audio endpoints carry binary data incompatible with text guardrails
    // 3. Auth + rate limiting + abuse detection provide sufficient protection
    // See docs/scenario-analysis-aegisgate.md GAP-003 for full analysis.

    auto& metrics = MetricsRegistry::instance();
    metrics.activeConnections().inc();
    auto start = std::chrono::steady_clock::now();

    // --- RBAC: enforce authentication before upstream dispatch (P0-2, SR-2) ---
    // Mirror of the chat path (processRequest): when RBAC is enabled an
    // unresolvable token is rejected with 401. In legacy (non-RBAC) mode
    // resolve() returning nullopt does not block (behavior unchanged).
    std::string proxy_tenant_id;  // P1-4: tenant attribution for proxy billing
    if (auth_service_) {
        auto auth_ctx = auth_service_->resolve(api_key);
        if (!auth_ctx && auth_service_->isRbacEnabled()) {
            metrics.activeConnections().dec();
            return {false, {}, {401, toAegisCode(ErrorCode::InvalidApiKey),
                    toErrorType(ErrorCode::InvalidApiKey),
                    toDefaultMessage(ErrorCode::InvalidApiKey), ""}};
        }
        if (auth_ctx) proxy_tenant_id = auth_ctx->tenant_id;
    }

    // --- Abuse detection ---
    double abuse_cost_multiplier = 1.0;  // P1-C: Throttle tightens rate budget
    if (abuse_detector_) {
        auto text = extractAbuseTextFromRaw(
            request.raw_body, abuse_detector_->similarityMaxContentBytes());
        auto action = abuse_detector_->observe(api_key, text);
        if (action == AbuseDetector::Action::Block) {
            metrics.activeConnections().dec();
            return {false, {}, {429, toAegisCode(ErrorCode::AbuseDetected),
                    toErrorType(ErrorCode::AbuseDetected),
                    toDefaultMessage(ErrorCode::AbuseDetected), ""}};
        }
        abuse_cost_multiplier = abuseCostMultiplier(abuse_detector_.get(), action);
    }

    // --- Rate limiting ---
    auto rl_key = api_key.empty() ? "anonymous" : api_key;
    auto rl = std::atomic_load(&rate_limiter_);
    if (!rl->allow(rl_key, abuse_cost_multiplier)) {
        metrics.rateLimitedTotal().inc();
        metrics.requestsTotal().inc(
            {{{{"model", request.model.empty() ? "proxy" : request.model},
               {"status", "rate_limited"}}}});
        metrics.activeConnections().dec();
        return {false, {}, {429, toAegisCode(ErrorCode::RateLimitExceeded),
                toErrorType(ErrorCode::RateLimitExceeded),
                toDefaultMessage(ErrorCode::RateLimitExceeded), ""}};
    }

    // --- Phase 6.1 ModalityRouter fast path (CR2 D4=C, Epic 2.4) ---
    // When a router has been wired AND has a handler for the endpoint's
    // modality, it takes precedence over the legacy connector lookup.
    // Otherwise we fall through to the existing ConnectorRegistry path.
    ModalityHandler* mod_handler = nullptr;
    Modality endpoint_modality = Modality::Unknown;
    if (modality_router_) {
        endpoint_modality = modalityFromEndpoint(request.endpoint);
        if (endpoint_modality != Modality::Unknown &&
            modality_router_->handlerCount(endpoint_modality) > 0) {
            mod_handler = modality_router_->selectHandler(endpoint_modality, request);
        }
    }

    // --- Phase 6.1 Epic 5.1c B1 (TASK-20260515-01): per-modality enforcement ---
    // Runs AFTER the global rate limiter (rate_limiter_) and AFTER the modality
    // router resolved the endpoint's modality, but BEFORE upstream dispatch.
    // Fail-open semantics (SR-NEW4) live in enforceModalityQuota itself.
    {
        auto modality_err = enforceModalityQuota(
            endpoint_modality, rl_key,
            modality_rate_limiter_.get(), metrics);
        if (modality_err) {
            metrics.activeConnections().dec();
            // Audit log: identity is hashed to avoid leaking the API key,
            // even into structured logs (defense-in-depth on top of the
            // existing redaction layer).
            spdlog::warn("modality_quota_block modality={} identity_hash={:x}",
                         modalityToString(endpoint_modality),
                         std::hash<std::string>{}(rl_key));
            return {false, {}, *modality_err};
        }
    }

    // --- Find connector by model (legacy path, also used when router declines) ---
    ModelConnector* connector = nullptr;
    if (!mod_handler) {
        if (!request.model.empty()) {
            connector = connector_registry_.findByModel(request.model);
        }
        if (!connector) {
            auto all = connector_registry_.all();
            for (auto* c : all) {
                if (c->supportsEndpoint(request.endpoint)) {
                    connector = c;
                    break;
                }
            }
        }

        if (!connector) {
            metrics.activeConnections().dec();
            return {false, {}, {404, toAegisCode(ErrorCode::UnsupportedEndpoint),
                    toErrorType(ErrorCode::UnsupportedEndpoint),
                    toDefaultMessage(ErrorCode::UnsupportedEndpoint), ""}};
        }

        if (!connector->supportsEndpoint(request.endpoint)) {
            metrics.activeConnections().dec();
            return {false, {}, {404, toAegisCode(ErrorCode::UnsupportedEndpoint),
                    toErrorType(ErrorCode::UnsupportedEndpoint),
                    "Endpoint " + request.endpoint +
                        " not supported by provider " + connector->provider(), ""}};
        }
    }

    // --- Proxy to upstream (router path or legacy connector path) ---
    auto upstream_start = std::chrono::steady_clock::now();
    try {
        auto proxy_resp = mod_handler
            ? mod_handler->handle(request, api_key)
            : connector->proxyRequest(request);
        {
            auto upstream_elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - upstream_start).count();
            metrics.upstreamDuration().observe(upstream_elapsed);
        }

        auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        metrics.requestDuration().observe(elapsed);
        metrics.requestsTotal().inc(
            {{{{"model", request.model.empty() ? "proxy" : request.model},
               {"status", proxy_resp.http_status >= 200 && proxy_resp.http_status < 300 ? "ok" : "error"}}}});
        metrics.activeConnections().dec();

        const std::string proxy_request_id =
            "proxy-" + std::to_string(start.time_since_epoch().count());

        if (pipeline_ && pipeline_->audit_logger) {
            pipeline_->audit_logger->logAction(
                proxy_request_id,
                "", "ProxyPass", "forward",
                "endpoint=" + request.endpoint + " model=" + request.model);
        }

        // P1-4: bill proxy/multimodal requests with the endpoint's modality so
        // /v1/embeddings, /v1/images/*, /v1/audio/* stop being zero-cost and
        // CostTracker::summaryByModality() stops being dead code. Best-effort:
        // tokens come from the upstream usage block when present (embeddings),
        // otherwise the record carries 0 tokens but the correct modality.
        if (pipeline_ && pipeline_->cost_tracker &&
            proxy_resp.http_status >= 200 && proxy_resp.http_status < 300) {
            auto rec = buildProxyCostRecord(
                *pipeline_->cost_tracker, request.endpoint, request.model,
                proxy_tenant_id, proxy_request_id, isoTimestampNow(), proxy_resp);
            pipeline_->cost_tracker->record(rec);
        }

        if (proxy_resp.http_status >= 200 && proxy_resp.http_status < 300) {
            return {true, std::move(proxy_resp), {}};
        }

        return {false, std::move(proxy_resp),
                {proxy_resp.http_status, toAegisCode(ErrorCode::UpstreamError),
                 toErrorType(ErrorCode::UpstreamError),
                 "Upstream returned " + std::to_string(proxy_resp.http_status), ""}};
    } catch (const std::exception& e) {
        auto upstream_elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - upstream_start).count();
        metrics.upstreamDuration().observe(upstream_elapsed);
        metrics.requestsTotal().inc(
            {{{{"model", request.model.empty() ? "proxy" : request.model},
               {"status", "error"}}}});
        metrics.activeConnections().dec();
        return {false, {}, {502, toAegisCode(ErrorCode::UpstreamError),
                toErrorType(ErrorCode::UpstreamError),
                toDefaultMessage(ErrorCode::UpstreamError), e.what()}};
    }
}

std::vector<ModelInfo> GatewayRuntime::registeredModels() const {
    return connector_registry_.allModelInfos();
}

} // namespace aegisgate
