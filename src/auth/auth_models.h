#pragma once
#include <optional>
#include <string>
#include <vector>

namespace aegisgate {

enum class Role : int {
    Viewer      = 0,
    Developer   = 1,
    TenantAdmin = 2,
    SuperAdmin  = 3
};

inline const char* roleToString(Role role) {
    switch (role) {
        case Role::Viewer:      return "viewer";
        case Role::Developer:   return "developer";
        case Role::TenantAdmin: return "tenant_admin";
        case Role::SuperAdmin:  return "super_admin";
    }
    return "viewer";
}

inline std::optional<Role> roleFromString(const std::string& str) {
    if (str == "viewer")       return Role::Viewer;
    if (str == "developer")    return Role::Developer;
    if (str == "tenant_admin") return Role::TenantAdmin;
    if (str == "super_admin")  return Role::SuperAdmin;
    return std::nullopt;
}

struct Tenant {
    std::string id;
    std::string name;
    std::string status = "active";
    std::vector<std::string> model_whitelist;
    double daily_cost_limit  = -1.0;
    double monthly_cost_limit = -1.0;
    int rate_limit_tokens    = -1;
    double rate_limit_refill = -1.0;
    std::string created_at;
    std::string updated_at;
};

struct User {
    std::string id;
    std::string tenant_id;
    std::string username;
    std::string display_name;
    Role role = Role::Viewer;
    std::string status = "active";
    std::string created_at;
    std::string updated_at;
};

struct ApiKeyRecord {
    std::string id;
    std::string user_id;
    std::string tenant_id;
    std::string name;
    std::string key_prefix;
    std::string key_hash;
    Role role = Role::Developer;
    std::string status = "active";
    std::string expires_at;
    std::string last_used_at;
    std::string created_at;
    std::string updated_at;
};

struct AuthContext {
    std::string tenant_id;
    std::string user_id;
    std::string api_key_id;
    Role role = Role::Viewer;
    bool is_rbac_enabled = false;
    std::string session_id;
    std::string auth_method;
    bool mfa_verified = false;
    std::string external_subject;
};

struct SsoProvider {
    std::string id;
    std::string tenant_id;
    std::string name;
    std::string issuer_url;
    std::string client_id;
    std::string client_secret_enc;
    std::string redirect_uri;
    std::vector<std::string> scopes;
    std::string claim_mapping_json;
    std::string group_role_mapping_json;
    bool jit_provisioning = true;
    std::string default_role = "viewer";
    bool enabled = true;
    std::string created_at;
    std::string updated_at;
};

struct IdentityMapping {
    std::string id;
    std::string tenant_id;
    std::string external_subject;
    std::string external_issuer;
    std::string user_id;
    std::string email;
    std::string last_login_at;
    std::string created_at;
};

struct Session {
    std::string id;
    std::string user_id;
    std::string tenant_id;
    std::string ip_address;
    std::string user_agent;
    std::string auth_method;
    bool mfa_verified = false;
    std::string created_at;
    std::string last_active_at;
    std::string expires_at;
};

struct MfaSecret {
    std::string user_id;
    std::string secret_enc;
    bool enabled = false;
    std::vector<std::string> recovery_codes_hash;
    std::string created_at;
};

struct ScimToken {
    std::string id;
    std::string tenant_id;
    std::string token_hash;
    std::string description;
    std::string created_at;
    std::string expires_at;
};

} // namespace aegisgate
