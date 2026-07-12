#pragma once
#include "auth/auth_service.h"
#include "core/config.h"
#include "core/pipeline_assembler.h"
#include "gateway/connector/registry.h"
#include "gateway/router.h"
#include "gateway/ml_router.h"
#include "gateway/ab_test_router.h"
#include "gateway/geo_router.h"
#ifdef AEGISGATE_ENABLE_REDIS
#include "cluster/redis_state_store.h"
#endif
#include "gateway/fallback.h"
#include "gateway/rate_limiter.h"
#include "gateway/abuse_detector.h"
#include "multimodal/modality_rate_limiter.h"
#include "multimodal/modality_router.h"
#include "multimodal/modality_upstream.h"
#include "observe/autonomy/approval_queue.h"
#include "observe/autonomy/approval_workflow.h"
#include "observe/autonomy/cost_autonomy_applier.h"
#include "observe/metrics.h"
#include "guardrail/admin/guard_admin_controller.h"
#include "guardrail/autonomy/guard_model_applier.h"
#include "guardrail/feedback/guard_feedback_anomaly_detector.h"
#include "guardrail/feedback/guard_feedback_rate_limiter.h"
#include "guardrail/feedback/guard_feedback_sink.h"
#include "guardrail/model/memory_guard_model_registry.h"
#include "server/budget_guard_stage.h"
#include "workflow/memory_workflow_state_store.h"
#include "workflow/sqlite_workflow_state_store.h"
#include "workflow/workflow_approval_applier.h"
#include "workflow/workflow_engine.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <string_view>
#include <vector>
#include <functional>
#include <atomic>

namespace aegisgate {

struct RequestContext;
class ScimService;
class SavingsAggregator;

// TASK-20260708-02 / REV20260707-C1 Epic 3 — SR-1 predicate:
// Decides whether the assembler should wire a Redis-backed state store
// into the RateLimiter. Pure function so unit tests can drive the truth
// table without spinning up Drogon / a full runtime.
//
// Trigger conditions (OR semantics, per creative Option A):
//   1. rate_limit.backend == "redis"                     (new user-facing key)
//   2. rate_limiter.backend == "redis"                   (legacy alias)
//   3. deployment.mode == "cluster" AND cluster_gate_enabled
//
// `cluster_gate_enabled` corresponds to
// `FeatureGate::isEnabled(Feature::ClusterDeployment)` in production;
// tests pass a bool directly to exercise both edition branches.
bool shouldWireRedisRateLimiter(const Config& config, bool cluster_gate_enabled);

// TASK-20260711-01 / REV20260707-I13 Epic 1 — Edition gate predicate:
// Decides whether the assembler / admin controller may enable
// AdvancedRouting-tier features (MLRouter / ABTestRouter / GeoRouter
// wrapping / GET /admin/api/savings/summary). Pure function so unit
// tests can drive the truth table without spinning up Drogon / a full
// runtime.
//
// Contract:
//   - Nullable-safe: `gate == nullptr` returns false (fall-safe closed
//     for missing gates → Community-equivalent behaviour). This matches
//     the pipeline_assembler.cpp:525 `if (ap.feature_gate && ...)` idiom.
//   - Otherwise returns `gate->isEnabled(Feature::AdvancedRouting)`.
//
// The gate is checked at assembly time (gateway_runtime.cpp router
// branches) and at request-time in AdminController::getSavingsSummary.
// Hot-path Router::selectModel is NOT gated (assembly-time is the
// authoritative choke point).
class FeatureGate;
bool isAdvancedRoutingEnabled(const FeatureGate* gate);

struct ProcessResult {
    bool success = false;
    ChatResponse response;
    GatewayError error;
    int tokens_saved = 0;
    // P1-8/P1-9: client-facing headers accumulated by pipeline stages
    // (budget downgrade marker, A/B variant). Copied from ctx.response_headers.
    std::unordered_map<std::string, std::string> response_headers;
};

struct ProxyResult {
    bool success = false;
    ProxyResponse response;
    GatewayError error;
};

// TASK-20260703-04 Epic 1（D1）：/v1/agent 执行结果。success=false 时 error 携带
// 4xx/5xx 语义（认证失败/端点未启用/引擎不可用/编排失败），由 controller 映射。
struct AgentResult {
    bool success = false;
    OrchestrationResult run;
    GatewayError error;
};

// TASK-20260703-04 Epic 2（D2）：/v1/workflow 触发结果。
struct WorkflowResult {
    bool success = false;
    std::string run_id;
    workflow::WorkflowExecutionResult exec;
    GatewayError error;
};

class GatewayRuntime {
public:
    static GatewayRuntime& instance();

    void initialize(const Config& config);
    bool isInitialized() const { return initialized_; }

    void beginShutdown();
    void shutdown();
    bool isShuttingDown() const;
    void resetShutdownForTesting() { shutting_down_.store(false); }
    void reinitializeForTesting();

    ProcessResult processRequest(
        ChatRequest request,
        const std::string& api_key,
        const std::unordered_map<std::string, std::string>& request_headers = {});
    ProxyResult processProxyRequest(ProxyRequest request, const std::string& api_key);

    // TASK-20260703-04 Epic 1（D1）：驱动 Orchestrator 做多步 ReAct。认证（SR-1）
    // → 端点 opt-in + 引擎可用性 → 以 pipeline_ 同源 registry/sandbox 构造 per-request
    // Orchestrator 并绑 fallback lambda 作 LlmFn（SR-3 继承熔断/多 key/成本）。
    AgentResult processAgentRequest(const std::string& model,
                                    const std::string& input,
                                    int max_steps,
                                    const std::string& api_key);

    // TASK-20260703-04 Epic 2（D2）：触发 WorkflowEngine 同步执行。认证（SR-4）
    // → 端点 opt-in + 引擎可用 → fromJson 校验（含 validateNoCycle/NoSandboxBypass
    // 由 execute 内部执行）→ 并发调度 + 节点超时 + 背压。
    WorkflowResult processWorkflowRequest(const nlohmann::json& workflow_json,
                                          const nlohmann::json& context,
                                          const std::string& api_key);

    workflow::WorkflowEngine* workflowEngine() const { return workflow_engine_.get(); }

    void processStreamingRequest(
        ChatRequest request,
        const std::string& api_key,
        std::function<void(const StreamDelta&)> onDelta,
        std::function<void(const TokenUsage&, int tokens_saved)> onDone,
        std::function<void(const GatewayError&)> onError,
        const std::unordered_map<std::string, std::string>& request_headers = {});

    bool validateApiKey(const std::string& key) const;

    // Authorize a standard (non-admin) API request. Unlike validateApiKey(),
    // this does NOT fail-open under RBAC: it performs the deferred resolve()
    // so read-only auxiliary endpoints (models list, metrics, cache stats)
    // enforce authentication instead of trusting a downstream resolve() that
    // is only executed on the chat / proxy hot paths.
    bool authorizeApiRequest(const std::string& key) const;

    bool validateAdminKey(const std::string& key) const;
    bool reloadConfig();
    void reloadSecurityRules();
    // TASK-20260702-02 P2-4（SR-4）：刷新单租户运行时规则集桶（admin activate 即时
    // 生效）。无 rule_engine / persistent_store 时 no-op。
    void refreshTenantRules(const std::string& tenant_id);

    std::shared_ptr<RateLimiter> rateLimiterSnapshot() const;

    void setAuthService(std::unique_ptr<AuthService> svc);
    AuthService* authService() const { return auth_service_.get(); }

    ConnectorRegistry& connectors() { return connector_registry_; }
    AssembledPipeline& pipeline() { return *pipeline_; }
    const Config& config() const { return *config_; }
    FallbackManager* fallbackManager() const { return fallback_.get(); }
    ScimService* scimService() const { return scim_service_.get(); }

    std::vector<ModelInfo> registeredModels() const;

    // Savings hook：在 cache hit / compression 节点后由热路径调用。
    // 由 GatewayRuntime::initialize 在 cost_tracker 加载 pricing 后创建；
    // 为 nullptr 时所有 hook 调用是 no-op（保证现有测试不受影响）。
    SavingsAggregator* savingsAggregator() const { return savings_aggregator_.get(); }

    // Phase 6.1 — ModalityRouter wiring (CR2 D4=C).
    // Default nullptr: processProxyRequest falls back to the legacy
    // ConnectorRegistry path. When the router is set AND has a handler
    // registered for the request's endpoint-derived modality, it takes
    // precedence over the legacy lookup. This keeps the 5 existing proxy
    // endpoints fully backward compatible while enabling N>1 backends in
    // the future without re-touching ApiController.
    void setModalityRouter(std::unique_ptr<ModalityRouter> r) {
        modality_router_ = std::move(r);
    }
    ModalityRouter* modalityRouter() const { return modality_router_.get(); }

    // Phase 6.1 Epic 5.1c (B1, TASK-20260515-01).
    // Per-modality rate limiter built from `multimodal.rate_limit.quotas`.
    // Hooked into processProxyRequest BEFORE upstream dispatch. nullptr =>
    // fail-open (no per-modality enforcement). Tests inject a custom
    // limiter via this setter; production GatewayRuntime::initialize wires
    // it from Config when multimodal.rate_limit.enabled = true.
    void setModalityRateLimiter(std::unique_ptr<ModalityRateLimiter> r) {
        modality_rate_limiter_ = std::move(r);
    }
    ModalityRateLimiter* modalityRateLimiter() const {
        return modality_rate_limiter_.get();
    }

    // Phase 11.5 (TASK-20260518-02 E4.2) — Autonomy wiring accessors.
    // All return nullptr when autonomy is disabled in config (or the
    // underlying dependencies — PersistentStore / AuditLogger / MLRouter —
    // are unavailable). Tests and the Admin API consume these to drive
    // the approval workflow.
    autonomy::AutonomyApprovalWorkflow* approvalWorkflow() const {
        return approval_workflow_.get();
    }
    autonomy::ApprovalQueue* approvalQueue() const {
        return approval_queue_.get();
    }
    autonomy::CostAutonomyApplier* costAutonomyApplier() const {
        return cost_autonomy_applier_.get();
    }
    BudgetGuardStage* budgetGuardStage() const {
        return budget_guard_stage_raw_;
    }

    // Phase 11.1 (TASK-20260523-01 R2.5) — Adaptive Guard admin wiring.
    // Returns nullptr until initialize() runs OR when the feedback bus is
    // disabled. The HTTP layer surfaces 503 in that case (same pattern as
    // approvalWorkflow()). Tests can pull the registry / sink / anomaly
    // detector individually for white-box assertions.
    guard::GuardAdminController* guardAdminController() const {
        return guard_admin_controller_.get();
    }
    guard::IGuardModelRegistry* guardModelRegistry() const {
        return guard_model_registry_.get();
    }
    guard::GuardFeedbackAnomalyDetector* guardAnomalyDetector() const {
        return guard_anomaly_detector_.get();
    }

private:
    GatewayRuntime() = default;

    struct TenantPolicy {
        std::optional<Tenant> tenant;
        std::shared_ptr<RateLimiter> rate_limiter;
    };

    TenantPolicy resolveTenantPolicy(const RequestContext& ctx) const;

    std::optional<GatewayError> checkTenantCostLimits(
        const RequestContext& ctx, const Tenant& tenant) const;

    static bool isModelAllowed(const Tenant& tenant, const std::string& model);

    void loadProviders(const std::string& models_path);
    std::string generateRequestId();

    // Phase 11.0 — attach the MetricsFeedbackSubscriber to FeedbackBus::instance()
    // on first call; subsequent calls re-attach (idempotent for the metrics bridge).
    void ensureFeedbackMetricsSubscriber();

    const Config* config_ = nullptr;
    std::unique_ptr<AssembledPipeline> pipeline_;
    ConnectorRegistry connector_registry_;
    std::unique_ptr<Router> router_;
    std::unique_ptr<FallbackManager> fallback_;
    std::shared_ptr<RateLimiter> rate_limiter_;
    std::unique_ptr<AbuseDetector> abuse_detector_;
    std::vector<std::string> valid_api_key_hashes_;
    std::unique_ptr<AuthService> auth_service_;
    std::unique_ptr<ScimService> scim_service_;
    std::unique_ptr<SavingsAggregator> savings_aggregator_;
    // Phase 6.1 wire (Epic 5.1b): adapter that exposes the OpenAI ModelConnector
    // as a ModalityUpstream. Handlers inside modality_router_ hold a
    // ModalityUpstream& to this adapter, so the adapter MUST outlive the
    // router. C++ destroys members in reverse declaration order, hence
    // adapter declared BEFORE the router.
    std::unique_ptr<ModalityUpstream> openai_modality_upstream_;
    std::unique_ptr<ModalityRouter> modality_router_;
    // Phase 6.1 Epic 5.1c (B1, TASK-20260515-01).
    // Holds a non-owning reference to rate_limiter_ via the ModalityRateLimiter
    // ctor (`backing_(backing)`). C++ destruction order is reverse-declaration,
    // so modality_rate_limiter_ (declared AFTER rate_limiter_) is destroyed
    // FIRST → rate_limiter_ outlives it → no use-after-free.
    // (A2 destruction-order checklist applied; see writing-plans.mdc.)
    std::unique_ptr<ModalityRateLimiter> modality_rate_limiter_;

    // Phase 11.5 (TASK-20260518-02 E4.2) — Autonomy wiring.
    //
    // Declaration / destruction order matters here (A2 checklist):
    //   - approval_queue_       owns no MLRouter; safe to destroy any time.
    //   - approval_workflow_    holds shared_ptr<ApprovalQueue> + non-owning
    //                           audit_logger; safe to destroy AFTER appliers.
    //   - cost_autonomy_applier_ wraps a NON-OWNING shared_ptr<MLRouter>
    //                           (aliasing-ctor). It must die BEFORE router_
    //                           because router_ holds the actual MLRouter.
    //   - budget_guard_stage_raw_ is a non-owning observer; the stage itself
    //                           lives inside pipeline_->inbound (owned by the
    //                           pipeline). Reset before pipeline_.reset().
    //
    // C++ destruction order is REVERSE-declaration, so we keep these AFTER
    // router_ / pipeline_ in the surrounding declaration: see below.
    std::shared_ptr<autonomy::ApprovalQueue> approval_queue_;
    std::shared_ptr<autonomy::AutonomyApprovalWorkflow> approval_workflow_;
    std::shared_ptr<autonomy::CostAutonomyApplier> cost_autonomy_applier_;
    BudgetGuardStage* budget_guard_stage_raw_ = nullptr;

    // Phase 11.1 (TASK-20260523-01 R2.5) — Adaptive Guard wiring.
    //
    // Declaration order = registry / sink dependencies first (since admin
    // controller holds shared_ptrs into them), admin controller LAST. C++
    // destroys in reverse order so the controller dies before any of its
    // upstream collaborators (A2 destruction-order checklist).
    std::shared_ptr<guard::IGuardModelRegistry> guard_model_registry_;
    std::shared_ptr<guard::GuardFeedbackSink> guard_feedback_sink_;
    std::shared_ptr<guard::GuardFeedbackRateLimiter> guard_feedback_rate_limiter_;
    std::shared_ptr<guard::GuardFeedbackAnomalyDetector> guard_anomaly_detector_;
    std::shared_ptr<guard::GuardModelApplier> guard_model_applier_;
    std::shared_ptr<guard::GuardAdminController> guard_admin_controller_;

    // Phase 11.3 TASK-20260523-02 — Workflow 2.0 wiring.
    //
    // Declaration order respects A2 destruction symmetry: the applier holds
    // a raw pointer to engine_ + state_store_, so it must die FIRST. State
    // store holds no references, so it can die last.
    std::unique_ptr<workflow::IWorkflowStateStore> workflow_state_store_;
    std::unique_ptr<workflow::WorkflowEngine>       workflow_engine_;
    std::shared_ptr<workflow::WorkflowApprovalApplier> workflow_applier_;
    // Captured at MLRouter construction in initialize() when router_type ==
    // "ml". Even after ABTestRouter/GeoRouter wrap, the underlying MLRouter
    // instance stays alive inside the wrapper chain — see initialize().
    MLRouter* ml_router_raw_ = nullptr;

    bool initialized_ = false;
    std::atomic<bool> shutting_down_{false};
    std::atomic<uint64_t> request_counter_{0};
#ifdef AEGISGATE_ENABLE_REDIS
    std::unique_ptr<RedisStateStore> redis_state_store_;
#endif
};

} // namespace aegisgate
