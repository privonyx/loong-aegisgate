#pragma once
#include "server/admin_controller_base.h"
#include "auth/auth_models.h"
#include "auth/authorization.h"
#include "auth/crypto_utils.h"
#include <nlohmann/json.hpp>
#include <string>

namespace aegisgate {

// TASK-20260605-05 Epic E2 — IAM 域子 controller（租户 / 用户 / API 密钥
// 14 方法）。纯 C++ + AdminResult，零 HTTP 依赖，可直测。方法体逐字迁移自
// AdminController（行为零变化）；过渡期由 Facade `AdminController` 持有并委托。
class AdminIamController : public AdminControllerBase {
public:
    AdminIamController(PersistentStore* store, AuditLogger* audit)
        : AdminControllerBase(store, audit) {}

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

private:
    static nlohmann::json tenantToJson(const Tenant& t);
    static nlohmann::json userToJson(const User& u);
    static nlohmann::json apiKeyToJson(const ApiKeyRecord& k);
};

} // namespace aegisgate
