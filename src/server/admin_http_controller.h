#pragma once
#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>
#include <aegisgate/error_codes.h>
#include "auth/auth_models.h"
#include "auth/oidc_client.h"
#include "auth/identity_mapper.h"
#include "gateway/rate_limiter.h"
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <functional>

namespace aegisgate {

class AdminHttpController : public drogon::HttpController<AdminHttpController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminHttpController::login,
                  "/admin/api/auth/login", drogon::Post, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::logout,
                  "/admin/api/auth/logout", drogon::Post, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::me,
                  "/admin/api/me", drogon::Get, drogon::Options);

    ADD_METHOD_TO(AdminHttpController::createTenant,
                  "/admin/api/tenants", drogon::Post, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::listTenants,
                  "/admin/api/tenants", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::getTenant,
                  "/admin/api/tenants/{id}", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::updateTenant,
                  "/admin/api/tenants/{id}", drogon::Put, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::deleteTenant,
                  "/admin/api/tenants/{id}", drogon::Delete, drogon::Options);

    ADD_METHOD_TO(AdminHttpController::createUser,
                  "/admin/api/users", drogon::Post, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::listUsers,
                  "/admin/api/users", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::getUser,
                  "/admin/api/users/{id}", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::updateUser,
                  "/admin/api/users/{id}", drogon::Put, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::deleteUser,
                  "/admin/api/users/{id}", drogon::Delete, drogon::Options);

    ADD_METHOD_TO(AdminHttpController::createApiKey,
                  "/admin/api/keys", drogon::Post, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::listApiKeys,
                  "/admin/api/keys", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::revokeApiKey,
                  "/admin/api/keys/{id}/revoke", drogon::Post, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::rotateApiKey,
                  "/admin/api/keys/{id}/rotate", drogon::Post, drogon::Options);

    ADD_METHOD_TO(AdminHttpController::queryAudits,
                  "/admin/api/audits", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::queryCosts,
                  "/admin/api/costs", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::dashboardSummary,
                  "/admin/api/dashboard/summary", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::savingsSummary,
                  "/admin/api/savings/summary", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::securityEvents,
                  "/admin/api/security/events", drogon::Get, drogon::Options);
    // TASK-20260527-02 — MVP-5 prep / Case Study Numbers headline endpoint.
    ADD_METHOD_TO(AdminHttpController::caseStudyHeadline,
                  "/admin/api/case-study/headline", drogon::Get, drogon::Options);

    // SSO auth flow
    ADD_METHOD_TO(AdminHttpController::ssoLogin,
                  "/admin/auth/sso/login", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::ssoCallback,
                  "/admin/auth/sso/callback", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::ssoLogout,
                  "/admin/auth/sso/logout", drogon::Post, drogon::Options);

    // SSO provider CRUD
    ADD_METHOD_TO(AdminHttpController::listSsoProviders,
                  "/admin/api/sso/providers", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::createSsoProvider,
                  "/admin/api/sso/providers", drogon::Post, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::getSsoProvider,
                  "/admin/api/sso/providers/{id}", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::updateSsoProvider,
                  "/admin/api/sso/providers/{id}", drogon::Put, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::deleteSsoProvider,
                  "/admin/api/sso/providers/{id}", drogon::Delete, drogon::Options);

    // Prompt Template management
    ADD_METHOD_TO(AdminHttpController::listPromptTemplates,
                  "/admin/api/templates", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::createPromptTemplate,
                  "/admin/api/templates", drogon::Post, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::getPromptTemplate,
                  "/admin/api/templates/{id}", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::updatePromptTemplate,
                  "/admin/api/templates/{id}", drogon::Put, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::deletePromptTemplate,
                  "/admin/api/templates/{id}", drogon::Delete, drogon::Options);

    // Rule Set management
    ADD_METHOD_TO(AdminHttpController::listRuleSets,
                  "/admin/api/rules", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::getActiveRuleSet,
                  "/admin/api/rules/active", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::createRuleSet,
                  "/admin/api/rules", drogon::Post, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::activateRuleSet,
                  "/admin/api/rules/activate", drogon::Post, drogon::Options);

    // Usage prediction
    ADD_METHOD_TO(AdminHttpController::predictUsage,
                  "/admin/api/predict/usage", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::predictBudget,
                  "/admin/api/predict/budget", drogon::Get, drogon::Options);

    // Compliance reports (TASK-20260604-01 P0-C) — 业务已实现但缺 HTTP 路由。
    ADD_METHOD_TO(AdminHttpController::exportAuditReport,
                  "/admin/api/export/audit", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::exportCostReport,
                  "/admin/api/export/cost", drogon::Get, drogon::Options);

    // MFA endpoints
    ADD_METHOD_TO(AdminHttpController::mfaSetup,
                  "/admin/api/mfa/setup", drogon::Post, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::mfaVerify,
                  "/admin/api/mfa/verify", drogon::Post, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::mfaDisable,
                  "/admin/api/mfa/disable", drogon::Post, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::mfaRecovery,
                  "/admin/api/mfa/recovery", drogon::Post, drogon::Options);

    // TASK-20260602-01 Epic 3 — predictUsage / predictBudget 历史曾被重复
    // 注册（L116-119 + L131-135）→ drogon 启动期产生 duplicate route 警告，
    // 实际只有一份生效但易引起维护混淆。已合并到 L116-119（"Usage prediction"
    // 段，单一定义）。此处显式不再重复注册以防回归。

    // Phase 11.5 (TASK-20260518-02 E5.1) — Autonomy approval workflow
    ADD_METHOD_TO(AdminHttpController::listAutonomyProposals,
                  "/admin/api/autonomy/proposals", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::approveAutonomyProposal,
                  "/admin/api/autonomy/proposals/{id}/approve",
                  drogon::Post, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::rejectAutonomyProposal,
                  "/admin/api/autonomy/proposals/{id}/reject",
                  drogon::Post, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::rollbackAutonomyProposal,
                  "/admin/api/autonomy/proposals/{id}",
                  drogon::Delete, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::autonomyReport,
                  "/admin/api/autonomy/report", drogon::Get, drogon::Options);

    // Phase 11.1 (TASK-20260523-01 R2.5) — Adaptive Guard admin endpoints.
    // TASK-20260706-01：路由从 /v1/guard/* 迁到 /admin/api/guard/*，使其落入
    // 登录 cookie 的 Path=/admin 作用域（原 /v1/guard 因 cookie path 不匹配，
    // 浏览器不发送 aegis_session → 恒 401，Guard 页不可用）。鉴权链不变。
    ADD_METHOD_TO(AdminHttpController::postGuardFeedback,
                  "/admin/api/guard/feedback", drogon::Post, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::getGuardExplanation,
                  "/admin/api/guard/explanation/{id}", drogon::Get, drogon::Options);
    ADD_METHOD_TO(AdminHttpController::postGuardModelPromote,
                  "/admin/api/guard/model/promote", drogon::Post, drogon::Options);
    METHOD_LIST_END

    void login(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void logout(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void me(const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void createTenant(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void listTenants(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void getTenant(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                   std::string id);
    void updateTenant(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                      std::string id);
    void deleteTenant(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                      std::string id);

    void createUser(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void listUsers(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void getUser(const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                 std::string id);
    void updateUser(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                    std::string id);
    void deleteUser(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                    std::string id);

    void createApiKey(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void listApiKeys(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void revokeApiKey(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                      std::string id);
    void rotateApiKey(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                      std::string id);

    void queryAudits(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void queryCosts(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void dashboardSummary(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void savingsSummary(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void securityEvents(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    // TASK-20260527-02 — MVP-5 prep / Case Study Numbers headline endpoint.
    void caseStudyHeadline(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // SSO auth flow
    void ssoLogin(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void ssoCallback(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void ssoLogout(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // SSO provider CRUD
    void listSsoProviders(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void createSsoProvider(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void getSsoProvider(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                        std::string id);
    void updateSsoProvider(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                           std::string id);
    void deleteSsoProvider(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                           std::string id);

    // Prompt Template management
    void listPromptTemplates(const drogon::HttpRequestPtr& req,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void createPromptTemplate(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void getPromptTemplate(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                           std::string id);
    void updatePromptTemplate(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                              std::string id);
    void deletePromptTemplate(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                              std::string id);

    // Rule Set management
    void listRuleSets(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void getActiveRuleSet(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void createRuleSet(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void activateRuleSet(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // Usage prediction
    void predictUsage(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void predictBudget(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // Compliance reports (TASK-20260604-01 P0-C)
    void exportAuditReport(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void exportCostReport(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // MFA endpoints
    void mfaSetup(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void mfaVerify(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void mfaDisable(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void mfaRecovery(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // Phase 11.5 (TASK-20260518-02 E5.1) — Autonomy approval workflow
    void listAutonomyProposals(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void approveAutonomyProposal(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string id);
    void rejectAutonomyProposal(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string id);
    void rollbackAutonomyProposal(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string id);
    void autonomyReport(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // Phase 11.1 (TASK-20260523-01 R2.5) — Adaptive Guard handlers.
    void postGuardFeedback(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void getGuardExplanation(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string id);
    void postGuardModelPromote(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    std::optional<AuthContext> authenticateRequest(const drogon::HttpRequestPtr& req);
    // TASK-20260604-01 P0-F / SR-2：预MFA态解析，仅 mfaVerify/mfaRecovery/me 使用。
    std::optional<AuthContext> authenticatePendingMfaRequest(const drogon::HttpRequestPtr& req);
    // TASK-20260703-02 C5：MFA 挑战端点（setup/verify/recovery）统一认证 =
    // 常规会话优先，被 MFA 闸门拒绝时回退预 MFA 态（DRY 根治首次绑定死锁）。
    std::optional<AuthContext> authenticateMfaChallengeRequest(const drogon::HttpRequestPtr& req);
    void applyCorsHeaders(const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp);
    void applyCspHeader(const drogon::HttpResponsePtr& resp);

    drogon::HttpResponsePtr makeAdminResponse(int status, const nlohmann::json& body,
                                              const drogon::HttpRequestPtr& req);
    drogon::HttpResponsePtr makeAdminError(ErrorCode code, const std::string& msg,
                                           const drogon::HttpRequestPtr& req);
    drogon::HttpResponsePtr handlePreflight(const drogon::HttpRequestPtr& req);
    drogon::HttpResponsePtr makeRedirect(const std::string& url,
                                          const drogon::HttpRequestPtr& req);

    bool isOriginAllowed(const std::string& origin) const;
    bool isIpAllowed(const std::string& ip) const;
    drogon::HttpResponsePtr checkAdminIpAccess(const drogon::HttpRequestPtr& req);
    static RateLimiter& loginRateLimiter();

    // SSO pending auth state management
    struct PendingSsoAuth {
        std::string code_verifier;
        std::string nonce;
        std::string tenant_id;
        std::chrono::steady_clock::time_point created_at;
    };
    static std::unordered_map<std::string, PendingSsoAuth>& pendingSsoMap();
    static std::mutex& pendingSsoMutex();
    void cleanupExpiredPendingSso();

    static OidcClient& oidcClient();
    static IdentityMapper& identityMapper();

    static constexpr const char* kSessionCookie = "aegis_session";
    static constexpr const char* kCspValue =
        "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'";
    static constexpr double kLoginRateMaxTokens = 10.0;
    static constexpr double kLoginRateRefillRate = 0.1;
    static constexpr auto kPendingSsoTtl = std::chrono::minutes(5);
};

} // namespace aegisgate
