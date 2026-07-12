#pragma once
#include "auth/auth_models.h"

namespace aegisgate::auth {

inline bool requireRole(const AuthContext& ctx, Role required) {
    if (!ctx.is_rbac_enabled) return true;
    return static_cast<int>(ctx.role) >= static_cast<int>(required);
}

inline bool requireTenantAccess(const AuthContext& ctx, const std::string& target_tenant_id) {
    if (!ctx.is_rbac_enabled) return true;
    if (ctx.role == Role::SuperAdmin) return true;
    return ctx.tenant_id == target_tenant_id;
}

inline bool authorize(const AuthContext& ctx, Role required, const std::string& target_tenant_id) {
    return requireRole(ctx, required) && requireTenantAccess(ctx, target_tenant_id);
}

// SR-1（TASK-20260702-01）：垂直提权防护。调用者不得授予/操作高于自身角色的
// 主体（创建/更新用户、签发/轮换 API Key）。RBAC 关闭时（legacy api_key
// SuperAdmin）沿用既有兼容语义放行。
inline bool canGrantRole(const AuthContext& ctx, Role target) {
    if (!ctx.is_rbac_enabled) return true;
    return static_cast<int>(target) <= static_cast<int>(ctx.role);
}

} // namespace aegisgate::auth
