#pragma once
#include "server/admin_controller_base.h"
#include "auth/auth_models.h"
#include "auth/authorization.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
#include <string>

namespace aegisgate {

// TASK-20260605-05 Epic E6 — 治理域子 controller（提示模板 5 + 规则集 4 +
// 审计查询 + 审计导出 = 11 方法）。纯 C++ + AdminResult，零 HTTP 依赖，可直测。
// 方法体逐字迁移自 AdminController（行为零变化）；过渡期由 Facade
// `AdminController` 持有并委托。继承 AdminControllerBase 复用
// effectiveTenantId / auditAction / nowTimestamp / generateId / store_。
class AdminGovernanceController : public AdminControllerBase {
public:
    AdminGovernanceController(PersistentStore* store, AuditLogger* audit)
        : AdminControllerBase(store, audit) {}

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

    // TASK-20260702-02 P2-4（SR-4）：规则集激活成功后回调（传 effectiveTenantId），
    // 由 buildAdmin 接线到 GatewayRuntime::refreshTenantRules 实现 activate 即时生效。
    void setRuleSetActivationHook(std::function<void(const std::string&)> hook) {
        rule_set_activation_hook_ = std::move(hook);
    }

    // --- Audit query & export ---
    AdminResult queryAudits(const AuthContext& ctx, const std::string& tenant_id, int limit, int offset);
    AdminResult exportAuditReport(const AuthContext& ctx, const std::string& from,
                                  const std::string& to, const std::string& tenant_id,
                                  const std::string& format);

private:
    std::function<void(const std::string&)> rule_set_activation_hook_;
};

} // namespace aegisgate
