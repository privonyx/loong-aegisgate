#include "server/admin_iam_controller.h"

#include <vector>

namespace aegisgate {

nlohmann::json AdminIamController::tenantToJson(const Tenant& t) {
    return {
        {"id", t.id}, {"name", t.name}, {"status", t.status},
        {"model_whitelist", t.model_whitelist},
        {"daily_cost_limit", t.daily_cost_limit},
        {"monthly_cost_limit", t.monthly_cost_limit},
        {"rate_limit_tokens", t.rate_limit_tokens},
        {"rate_limit_refill", t.rate_limit_refill},
        {"created_at", t.created_at}, {"updated_at", t.updated_at}
    };
}

nlohmann::json AdminIamController::userToJson(const User& u) {
    return {
        {"id", u.id}, {"tenant_id", u.tenant_id},
        {"username", u.username}, {"display_name", u.display_name},
        {"role", roleToString(u.role)}, {"status", u.status},
        {"created_at", u.created_at}, {"updated_at", u.updated_at}
    };
}

nlohmann::json AdminIamController::apiKeyToJson(const ApiKeyRecord& k) {
    return {
        {"id", k.id}, {"user_id", k.user_id}, {"tenant_id", k.tenant_id},
        {"name", k.name}, {"key_prefix", k.key_prefix},
        {"role", roleToString(k.role)}, {"status", k.status},
        {"expires_at", k.expires_at}, {"last_used_at", k.last_used_at},
        {"created_at", k.created_at}, {"updated_at", k.updated_at}
    };
}

// --- Tenant management ---

AdminResult AdminIamController::createTenant(const AuthContext& ctx, const nlohmann::json& body) {
    if (!auth::requireRole(ctx, Role::SuperAdmin))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    Tenant t;
    t.id = generateId();
    t.name = body.value("name", "");
    if (t.name.empty())
        return AdminResult::error(ErrorCode::MissingRequiredField, "Field 'name' is required");
    t.status = body.value("status", "active");
    if (body.contains("model_whitelist"))
        t.model_whitelist = body["model_whitelist"].get<std::vector<std::string>>();
    t.daily_cost_limit = body.value("daily_cost_limit", -1.0);
    t.monthly_cost_limit = body.value("monthly_cost_limit", -1.0);
    t.rate_limit_tokens = body.value("rate_limit_tokens", -1);
    t.rate_limit_refill = body.value("rate_limit_refill", -1.0);
    t.created_at = nowTimestamp();
    t.updated_at = t.created_at;

    auto existing = store_->listTenants(1000, 0);
    for (const auto& e : existing) {
        if (e.name == t.name)
            return AdminResult::error(ErrorCode::TenantNameExists);
    }

    if (!store_->insertTenant(t))
        return AdminResult::error(ErrorCode::InternalError, "Failed to create tenant");

    auditCrossTenantAction(ctx, t.id, "admin.create_tenant",
                           "tenant_id=" + t.id + " name=" + t.name);
    return AdminResult::ok(tenantToJson(t), 201);
}

AdminResult AdminIamController::listTenants(const AuthContext& ctx, int limit, int offset) {
    if (!auth::requireRole(ctx, Role::SuperAdmin))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    auto tenants = store_->listTenants(limit, offset);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : tenants) arr.push_back(tenantToJson(t));
    // P0-E：total = 全量计数（区别于当前页 count），供前端正确翻页。
    return AdminResult::ok({{"data", arr}, {"count", arr.size()},
                            {"total", store_->tenantCount()}});
}

AdminResult AdminIamController::getTenant(const AuthContext& ctx, const std::string& id) {
    if (!auth::authorize(ctx, Role::Viewer, id))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    auto tenant_id = effectiveTenantId(ctx, id);
    auto t = store_->getTenant(tenant_id);
    if (!t) return AdminResult::error(ErrorCode::InvalidRequest, "Tenant not found");
    return AdminResult::ok(tenantToJson(*t));
}

AdminResult AdminIamController::updateTenant(const AuthContext& ctx, const std::string& id, const nlohmann::json& body) {
    if (!auth::authorize(ctx, Role::SuperAdmin, id))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    auto t = store_->getTenant(id);
    if (!t) return AdminResult::error(ErrorCode::InvalidRequest, "Tenant not found");

    if (body.contains("name")) t->name = body["name"].get<std::string>();
    if (body.contains("status")) t->status = body["status"].get<std::string>();
    if (body.contains("model_whitelist"))
        t->model_whitelist = body["model_whitelist"].get<std::vector<std::string>>();
    if (body.contains("daily_cost_limit")) t->daily_cost_limit = body["daily_cost_limit"].get<double>();
    if (body.contains("monthly_cost_limit")) t->monthly_cost_limit = body["monthly_cost_limit"].get<double>();
    if (body.contains("rate_limit_tokens")) t->rate_limit_tokens = body["rate_limit_tokens"].get<int>();
    if (body.contains("rate_limit_refill")) t->rate_limit_refill = body["rate_limit_refill"].get<double>();
    t->updated_at = nowTimestamp();

    if (!store_->updateTenant(*t))
        return AdminResult::error(ErrorCode::InternalError, "Failed to update tenant");
    auditCrossTenantAction(ctx, id, "admin.update_tenant", "tenant_id=" + id);
    return AdminResult::ok(tenantToJson(*t));
}

AdminResult AdminIamController::deleteTenant(const AuthContext& ctx, const std::string& id) {
    if (!auth::requireRole(ctx, Role::SuperAdmin))
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    if (!store_->deleteTenant(id))
        return AdminResult::error(ErrorCode::InvalidRequest, "Tenant not found");
    auditCrossTenantAction(ctx, id, "admin.delete_tenant", "tenant_id=" + id);
    return AdminResult::ok({{"deleted", true}});
}

// --- User management ---

AdminResult AdminIamController::createUser(const AuthContext& ctx, const nlohmann::json& body) {
    std::string target_tenant = body.value("tenant_id", std::string{});
    if (target_tenant.empty()) target_tenant = ctx.tenant_id;
    if (!auth::authorize(ctx, Role::TenantAdmin, target_tenant))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    User u;
    u.id = generateId();
    u.tenant_id = target_tenant;
    u.username = body.value("username", "");
    if (u.username.empty())
        return AdminResult::error(ErrorCode::MissingRequiredField, "Field 'username' is required");
    u.display_name = body.value("display_name", "");

    auto role_str = body.value("role", "viewer");
    auto role = roleFromString(role_str);
    if (!role) return AdminResult::error(ErrorCode::InvalidRequest, "Invalid role: " + role_str);
    if (!auth::canGrantRole(ctx, *role))
        return AdminResult::error(ErrorCode::InsufficientPermissions,
                                  "Cannot assign a role higher than your own");
    u.role = *role;
    u.status = "active";
    u.created_at = nowTimestamp();
    u.updated_at = u.created_at;

    if (store_->getUserByUsername(u.tenant_id, u.username))
        return AdminResult::error(ErrorCode::UsernameExists);

    if (!store_->insertUser(u))
        return AdminResult::error(ErrorCode::InternalError, "Failed to create user");
    auditAction(ctx, "admin.create_user", "user_id=" + u.id);
    return AdminResult::ok(userToJson(u), 201);
}

AdminResult AdminIamController::listUsers(
    const AuthContext& ctx, const std::string& tenant_id, int limit, int offset) {
    if (!tenant_id.empty() && !auth::requireTenantAccess(ctx, tenant_id))
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    auto eff = effectiveTenantId(ctx, tenant_id);
    if (!auth::requireRole(ctx, Role::Viewer))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    auto users = store_->listUsers(eff, limit, offset);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& u : users) arr.push_back(userToJson(u));
    // P0-E/SR-3：total 经 eff（effectiveTenantId）过滤，非 super 不泄漏全局计数。
    return AdminResult::ok({{"data", arr}, {"count", arr.size()},
                            {"total", store_->userCount(eff)}});
}

AdminResult AdminIamController::getUser(const AuthContext& ctx, const std::string& id) {
    auto u = store_->getUser(id);
    if (!u) return AdminResult::error(ErrorCode::InvalidRequest, "User not found");
    if (!auth::authorize(ctx, Role::Viewer, u->tenant_id))
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    return AdminResult::ok(userToJson(*u));
}

AdminResult AdminIamController::updateUser(
    const AuthContext& ctx, const std::string& id, const nlohmann::json& body) {
    auto u = store_->getUser(id);
    if (!u) return AdminResult::error(ErrorCode::InvalidRequest, "User not found");
    if (!auth::authorize(ctx, Role::TenantAdmin, u->tenant_id))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    if (body.contains("display_name")) u->display_name = body["display_name"].get<std::string>();
    if (body.contains("role")) {
        auto role = roleFromString(body["role"].get<std::string>());
        if (!role) return AdminResult::error(ErrorCode::InvalidRequest, "Invalid role");
        if (!auth::canGrantRole(ctx, *role))
            return AdminResult::error(ErrorCode::InsufficientPermissions,
                                      "Cannot assign a role higher than your own");
        u->role = *role;
    }
    if (body.contains("status")) u->status = body["status"].get<std::string>();
    u->updated_at = nowTimestamp();

    if (!store_->updateUser(*u))
        return AdminResult::error(ErrorCode::InternalError, "Failed to update user");
    auditAction(ctx, "admin.update_user", "user_id=" + id);
    return AdminResult::ok(userToJson(*u));
}

AdminResult AdminIamController::deleteUser(const AuthContext& ctx, const std::string& id) {
    auto u = store_->getUser(id);
    if (!u) return AdminResult::error(ErrorCode::InvalidRequest, "User not found");
    if (!auth::authorize(ctx, Role::TenantAdmin, u->tenant_id))
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    if (!store_->deleteUser(id))
        return AdminResult::error(ErrorCode::InternalError, "Failed to delete user");
    auditAction(ctx, "admin.delete_user", "user_id=" + id);
    return AdminResult::ok({{"deleted", true}});
}

// --- API Key management ---

AdminResult AdminIamController::createApiKey(const AuthContext& ctx, const nlohmann::json& body) {
    std::string user_id = body.value("user_id", std::string{});
    if (user_id.empty())
        return AdminResult::error(ErrorCode::MissingRequiredField, "Field 'user_id' is required");

    auto user = store_->getUser(user_id);
    if (!user) return AdminResult::error(ErrorCode::InvalidRequest, "User not found");
    if (!auth::authorize(ctx, Role::TenantAdmin, user->tenant_id))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    auto raw_key = auth::generateApiKey();

    ApiKeyRecord k;
    k.id = generateId();
    k.user_id = user_id;
    k.tenant_id = user->tenant_id;
    k.name = body.value("name", "");
    k.key_prefix = auth::extractKeyPrefix(raw_key);
    k.key_hash = auth::hashApiKey(raw_key);
    auto role_str = body.value("role", roleToString(user->role));
    auto role = roleFromString(role_str);
    k.role = role.value_or(user->role);
    if (!auth::canGrantRole(ctx, k.role))
        return AdminResult::error(ErrorCode::InsufficientPermissions,
                                  "Cannot issue a key with a role higher than your own");
    k.status = "active";
    if (body.contains("expires_at")) k.expires_at = body["expires_at"].get<std::string>();
    k.created_at = nowTimestamp();
    k.updated_at = k.created_at;

    if (!store_->insertApiKey(k))
        return AdminResult::error(ErrorCode::InternalError, "Failed to create API key");

    auditAction(ctx, "admin.create_api_key", "key_id=" + k.id);
    auto j = apiKeyToJson(k);
    j["key"] = raw_key;
    return AdminResult::ok(j, 201);
}

AdminResult AdminIamController::listApiKeys(
    const AuthContext& ctx, const std::string& tenant_id, int limit, int offset) {
    if (!tenant_id.empty() && !auth::requireTenantAccess(ctx, tenant_id))
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    auto eff = effectiveTenantId(ctx, tenant_id);
    if (!auth::requireRole(ctx, Role::Viewer))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    auto keys = store_->listApiKeys(eff, limit, offset);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& k : keys) arr.push_back(apiKeyToJson(k));
    // P0-E/SR-3：total 经 eff 过滤。
    return AdminResult::ok({{"data", arr}, {"count", arr.size()},
                            {"total", store_->apiKeyCount(eff)}});
}

AdminResult AdminIamController::revokeApiKey(const AuthContext& ctx, const std::string& id) {
    auto keys_all = store_->listApiKeys("", 10000, 0);
    const ApiKeyRecord* found = nullptr;
    for (const auto& k : keys_all) {
        if (k.id == id) { found = &k; break; }
    }
    if (!found) return AdminResult::error(ErrorCode::InvalidRequest, "API key not found");
    if (!auth::authorize(ctx, Role::TenantAdmin, found->tenant_id))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    if (!store_->revokeApiKey(id))
        return AdminResult::error(ErrorCode::InternalError, "Failed to revoke API key");
    auditAction(ctx, "admin.revoke_api_key", "key_id=" + id);
    return AdminResult::ok({{"revoked", true}});
}

AdminResult AdminIamController::rotateApiKey(const AuthContext& ctx, const std::string& id) {
    auto keys_all = store_->listApiKeys("", 10000, 0);
    const ApiKeyRecord* found = nullptr;
    for (const auto& k : keys_all) {
        if (k.id == id) { found = &k; break; }
    }
    if (!found) return AdminResult::error(ErrorCode::InvalidRequest, "API key not found");
    if (!auth::authorize(ctx, Role::TenantAdmin, found->tenant_id))
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    if (!auth::canGrantRole(ctx, found->role))
        return AdminResult::error(ErrorCode::InsufficientPermissions,
                                  "Cannot rotate a key with a role higher than your own");

    store_->revokeApiKey(id);

    auto raw_key = auth::generateApiKey();
    ApiKeyRecord new_key;
    new_key.id = generateId();
    new_key.user_id = found->user_id;
    new_key.tenant_id = found->tenant_id;
    new_key.name = found->name + " (rotated)";
    new_key.key_prefix = auth::extractKeyPrefix(raw_key);
    new_key.key_hash = auth::hashApiKey(raw_key);
    new_key.role = found->role;
    new_key.status = "active";
    new_key.created_at = nowTimestamp();
    new_key.updated_at = new_key.created_at;

    if (!store_->insertApiKey(new_key))
        return AdminResult::error(ErrorCode::InternalError, "Failed to create rotated key");

    auditAction(ctx, "admin.rotate_api_key", "old_key_id=" + id + " new_key_id=" + new_key.id);
    auto j = apiKeyToJson(new_key);
    j["key"] = raw_key;
    j["rotated_from"] = id;
    return AdminResult::ok(j, 201);
}

} // namespace aegisgate
