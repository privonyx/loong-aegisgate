#pragma once
#include "server/admin_controller_base.h"
#include "server/admin_iam_controller.h"
#include "server/admin_governance_controller.h"
#include "auth/auth_models.h"
#include "auth/auth_service.h"
#include "auth/authorization.h"
#include "auth/crypto_utils.h"
#include "auth/oidc_client.h"
#include "auth/identity_mapper.h"
#include "auth/encryption.h"
#include "auth/totp_service.h"
#include "storage/persistent_store.h"
#include "guardrail/audit.h"
#include <aegisgate/error_codes.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace aegisgate {

class FeatureGate;  // TASK-20260711-01 / REV20260707-I13 — AdvancedRouting gate
class SavingsAggregator;
class SemanticCache;
class CostTracker;       // TASK-20260527-02
class QualityMonitor;    // TASK-20260527-02
namespace autonomy {
class AutonomyApprovalWorkflow;
class ApprovalQueue;
} // namespace autonomy

// AdminResult 已上移 admin_controller_base.h（TASK-20260605-05 Epic 2）。

struct SsoLoginResult {
    std::string redirect_url;
    std::string state;
    std::string nonce;
    std::string code_verifier;
    std::string error;
};

struct SsoCallbackResult {
    bool success = false;
    Session session;
    std::string error;
    std::string redirect_url;
};

// TASK-20260605-05 — 过渡期 Facade（creative D1=C）：保留全部 50 public 方法
// 签名供 124+ 单元测试零改动直调；IAM / 治理域方法体迁至域子 controller，本类
// 改薄委托。继承 AdminControllerBase 复用 effectiveTenantId / audit / id-helper。
class AdminController : public AdminControllerBase {
public:
    AdminController(PersistentStore* store, AuthService* auth_service,
                    AuditLogger* audit = nullptr,
                    OidcClient* oidc_client = nullptr,
                    IdentityMapper* identity_mapper = nullptr,
                    SavingsAggregator* savings_aggregator = nullptr,
                    SemanticCache* semantic_cache = nullptr,
                    // TASK-20260711-01 / REV20260707-I13: license gate for
                    // AdvancedRouting-tier admin endpoints (savings summary).
                    // Nullable → fall-open (legacy tests unaffected).
                    const FeatureGate* feature_gate = nullptr);

    // 测试 / runtime 注入：允许在构造后绑定 savings + cache 数据源。
    void setSavingsAggregator(SavingsAggregator* agg) { savings_aggregator_ = agg; }
    void setSemanticCache(SemanticCache* cache) { semantic_cache_ = cache; }
    // TASK-20260711-01 / REV20260707-I13: post-construction FeatureGate
    // binding (mirrors savings/cache setter family; used by buildAdmin()).
    void setFeatureGate(const FeatureGate* gate) { feature_gate_ = gate; }
    // TASK-20260702-02 P2-2：MFA 恢复码数量 + 失败锁定策略（buildAdmin 从 config 注入）。
    void setMfaPolicy(int recovery_code_count, int lockout_max_failures,
                      int lockout_window_sec) {
        if (recovery_code_count > 0) mfa_recovery_code_count_ = recovery_code_count;
        if (lockout_max_failures > 0) mfa_lockout_max_failures_ = lockout_max_failures;
        if (lockout_window_sec > 0) mfa_lockout_window_sec_ = lockout_window_sec;
    }
    // TASK-20260702-02 P2-4（SR-4）：规则集激活刷新钩子（委托治理域子 controller）。
    void setRuleSetActivationHook(std::function<void(const std::string&)> hook) {
        governance_.setRuleSetActivationHook(std::move(hook));
    }

    // TASK-20260527-02 — case-study Headline (D1=A 3 块 admin endpoint).
    // 注入 CostTracker（saved vs baseline 数据源）+ QualityMonitor
    // （quality_reason taxonomy 数据源）。两者都是非拥有指针，缺失时
    // caseStudyHeadline 用零值占位（保证 endpoint 始终可用 / 即便后端
    // 未装配也不 500）。
    void setCostTracker(CostTracker* tracker) { cost_tracker_ = tracker; }
    void setQualityMonitor(QualityMonitor* monitor) { quality_monitor_ = monitor; }

    // Phase 11.5 (TASK-20260518-02 E5.1) — Autonomy workflow injection.
    // Both setters take non-owning pointers (workflow + queue live on the
    // GatewayRuntime side).  Either pointer may be null when the autonomy
    // feature is disabled; the API handlers will return AEGIS-6002
    // (AutonomyDisabled) in that case.
    void setAutonomyWorkflow(autonomy::AutonomyApprovalWorkflow* w) {
        autonomy_workflow_ = w;
    }
    void setAutonomyQueue(autonomy::ApprovalQueue* q) {
        autonomy_queue_ = q;
    }

    // --- Tenant management ---
    AdminResult createTenant(const AuthContext& ctx, const nlohmann::json& body);
    AdminResult listTenants(const AuthContext& ctx, int limit, int offset);
    AdminResult getTenant(const AuthContext& ctx, const std::string& id);
    AdminResult updateTenant(const AuthContext& ctx, const std::string& id, const nlohmann::json& body);
    AdminResult deleteTenant(const AuthContext& ctx, const std::string& id);

    // --- User management ---
    AdminResult createUser(const AuthContext& ctx, const nlohmann::json& body);
    AdminResult listUsers(const AuthContext& ctx, const std::string& tenant_id, int limit, int offset);
    AdminResult getUser(const AuthContext& ctx, const std::string& id);
    AdminResult updateUser(const AuthContext& ctx, const std::string& id, const nlohmann::json& body);
    AdminResult deleteUser(const AuthContext& ctx, const std::string& id);

    // --- API Key management ---
    AdminResult createApiKey(const AuthContext& ctx, const nlohmann::json& body);
    AdminResult listApiKeys(const AuthContext& ctx, const std::string& tenant_id, int limit, int offset);
    AdminResult revokeApiKey(const AuthContext& ctx, const std::string& id);
    AdminResult rotateApiKey(const AuthContext& ctx, const std::string& id);

    // --- Audit & Cost queries ---
    AdminResult queryAudits(const AuthContext& ctx, const std::string& tenant_id, int limit, int offset);
    AdminResult queryCosts(const AuthContext& ctx, const std::string& tenant_id,
                           const std::string& model, int limit, int offset);

    // --- Dashboard ---
    AdminResult dashboardSummary(const AuthContext& ctx);

    // --- Case Study Numbers (TASK-20260527-02 / MVP-5 prep) ---
    // 返回 3 头条数字（spec §3.5 Row 4）：
    //   saved_vs_baseline { cost_saved, baseline_cost, actual_cost, savings_percent }
    //   cache_hit_by_type { total_hit_rate, hit_exact, hit_semantic, hit_conversation, miss }
    //   quality_reason    { current_ema, slope, reason_factuality, reason_refusal, reason_latency_degraded }
    // SR1 RBAC：SuperAdmin → 全局聚合；其他角色 → 限本租户。
    AdminResult caseStudyHeadline(const AuthContext& ctx);

    // --- Savings Dashboard (TASK-20260510-01) ---
    // 返回多维节省聚合：total / by_type / by_model / time_series / top_tenants（仅 SuperAdmin）。
    // from_iso / to_iso 缺省时取近 7 天；时间窗口 ≤ 365 天（SR-NEW3）。
    // tenant_id 非空时仅 SuperAdmin 可跨租户查询；其它角色限定为本租户（SR1）。
    AdminResult getSavingsSummary(const AuthContext& ctx,
                                  const std::string& from_iso,
                                  const std::string& to_iso,
                                  const std::string& tenant_id);

    // --- Security events summary (Dashboard mock 替换) ---
    // 返回 24h 内 guardrail 拦截 / 输入归一化 / 速率限制 等安全事件的真实计数。
    AdminResult getSecurityEvents(const AuthContext& ctx);

    // --- SSO flow ---
    SsoLoginResult initiateSsoLogin(const std::string& tenant_id);
    SsoCallbackResult handleSsoCallback(const std::string& code,
                                         const std::string& code_verifier,
                                         const std::string& nonce,
                                         const std::string& tenant_id,
                                         const std::string& ip_address,
                                         const std::string& user_agent);
    std::string handleSsoLogout(const std::string& session_id,
                                 const std::string& tenant_id);

    // --- SSO Provider CRUD ---
    AdminResult createSsoProvider(const AuthContext& ctx, const nlohmann::json& body);
    AdminResult listSsoProviders(const AuthContext& ctx, int limit, int offset);
    AdminResult getSsoProvider(const AuthContext& ctx, const std::string& id);
    AdminResult updateSsoProvider(const AuthContext& ctx, const std::string& id,
                                   const nlohmann::json& body);
    AdminResult deleteSsoProvider(const AuthContext& ctx, const std::string& id);

    // --- Prompt Template management ---
    AdminResult createPromptTemplate(const AuthContext& ctx, const nlohmann::json& body);
    AdminResult listPromptTemplates(const AuthContext& ctx, const std::string& tenant_id,
                                     int limit, int offset);
    AdminResult getPromptTemplate(const AuthContext& ctx, const std::string& id);
    AdminResult updatePromptTemplate(const AuthContext& ctx, const std::string& id,
                                      const nlohmann::json& body);
    AdminResult deletePromptTemplate(const AuthContext& ctx, const std::string& id);

    // --- Rule Set management ---
    AdminResult listRuleSets(const AuthContext& ctx, const std::string& tenant_id, int limit, int offset = 0);
    AdminResult getActiveRuleSet(const AuthContext& ctx, const std::string& tenant_id);
    AdminResult createRuleSet(const AuthContext& ctx, const nlohmann::json& body);
    AdminResult activateRuleSet(const AuthContext& ctx, const std::string& tenant_id,
                                 int64_t version);

    // --- Usage prediction ---
    AdminResult predictUsage(const AuthContext& ctx, const std::string& tenant_id,
                              int history_days, int forecast_days);
    AdminResult predictBudgetExhaustion(const AuthContext& ctx, const std::string& tenant_id,
                                         double budget, int history_days);

    // --- Compliance reports ---
    AdminResult exportAuditReport(const AuthContext& ctx, const std::string& from,
                                   const std::string& to, const std::string& tenant_id,
                                   const std::string& format);
    AdminResult exportCostReport(const AuthContext& ctx, const std::string& from,
                                  const std::string& to, const std::string& tenant_id,
                                  const std::string& format);

    // --- MFA management ---
    AdminResult setupMfa(const AuthContext& ctx);
    AdminResult verifyMfa(const AuthContext& ctx, const nlohmann::json& body);
    AdminResult disableMfa(const AuthContext& ctx, const nlohmann::json& body);
    AdminResult recoveryMfa(const AuthContext& ctx, const nlohmann::json& body);

    // --- Phase 11.5 Autonomy approval workflow (TASK-20260518-02 E5.1) ---
    //
    // 5 endpoints:
    //   GET    /admin/api/autonomy/proposals          → listAutonomyProposals
    //   POST   /admin/api/autonomy/proposals/{id}/approve → approveAutonomyProposal
    //   POST   /admin/api/autonomy/proposals/{id}/reject  → rejectAutonomyProposal
    //   DELETE /admin/api/autonomy/proposals/{id}     → rollbackAutonomyProposal
    //   GET    /admin/api/autonomy/report             → autonomyReport
    //
    // All routes require TenantAdmin (or SuperAdmin) RBAC role and emit an
    // audit entry per call.  When the workflow / queue is null (autonomy
    // disabled in config or the SR17 env kill switch is set), every handler
    // returns AEGIS-6002 (AutonomyDisabled).
    AdminResult listAutonomyProposals(const AuthContext& ctx,
                                       const std::string& state_filter,
                                       const std::string& source_filter,
                                       int limit, int offset);
    AdminResult approveAutonomyProposal(const AuthContext& ctx,
                                         const std::string& id);
    AdminResult rejectAutonomyProposal(const AuthContext& ctx,
                                        const std::string& id,
                                        const nlohmann::json& body);
    AdminResult rollbackAutonomyProposal(const AuthContext& ctx,
                                          const std::string& id);
    AdminResult autonomyReport(const AuthContext& ctx);

private:
    static nlohmann::json ssoProviderToJson(const SsoProvider& p);

    // effectiveTenantId / nowTimestamp / generateId / auditAction /
    // auditCrossTenantAction / store_ / audit_ 已上移 AdminControllerBase。
    // tenantToJson / userToJson / apiKeyToJson 已迁至 AdminIamController。

    // TASK-20260605-05 Epic E2 — IAM 域子 controller（Facade 委托目标）。
    AdminIamController iam_;
    // TASK-20260605-05 Epic E6 — 治理域子 controller（模板/规则/审计查询+导出）。
    AdminGovernanceController governance_;

    AuthService* auth_service_;
    // TASK-20260702-02 P2-2：MFA 策略（默认值 = config 缺省，buildAdmin 覆盖）。
    int mfa_recovery_code_count_ = 8;
    int mfa_lockout_max_failures_ = 5;
    int mfa_lockout_window_sec_ = 900;
    OidcClient* oidc_client_ = nullptr;
    IdentityMapper* identity_mapper_ = nullptr;
    SavingsAggregator* savings_aggregator_ = nullptr;  // 非拥有指针
    SemanticCache* semantic_cache_ = nullptr;          // 非拥有指针，dashboard cache_hit_rate
    // TASK-20260711-01 / REV20260707-I13: license gate for AdvancedRouting.
    // Non-owning; may be null (legacy) → fall-open.
    const FeatureGate* feature_gate_ = nullptr;
    // TASK-20260527-02 — Case Study Numbers data sources（均为非拥有指针）.
    CostTracker* cost_tracker_ = nullptr;
    QualityMonitor* quality_monitor_ = nullptr;

    // Phase 11.5 (TASK-20260518-02 E5.1). Both non-owning; lifecycle is
    // governed by GatewayRuntime. Either may be null when autonomy is off.
    autonomy::AutonomyApprovalWorkflow* autonomy_workflow_ = nullptr;
    autonomy::ApprovalQueue* autonomy_queue_ = nullptr;
};

} // namespace aegisgate
