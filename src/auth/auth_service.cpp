#include "auth/auth_service.h"
#include "core/crypto.h"

namespace aegisgate {

AuthService::AuthService(PersistentStore* store, const Config* config, const FeatureGate* gate)
    : store_(store), config_(config), gate_(gate), session_mgr_(store, config) {}

bool AuthService::isRbacEnabled() const {
    return gate_ && gate_->isEnabled(Feature::RBAC);
}

namespace {
bool isReservedSystemUser(const std::string& user_id) {
    return user_id == "system.autorollback";
}
} // namespace

std::optional<AuthContext> AuthService::resolve(const std::string& bearer_token) const {
    if (bearer_token.empty()) return std::nullopt;
    auto ctx = isRbacEnabled() ? resolveRbac(bearer_token) : resolveLegacy(bearer_token);
    if (ctx && isReservedSystemUser(ctx->user_id)) return std::nullopt;
    return ctx;
}

std::optional<AuthContext> AuthService::resolveRbac(const std::string& token) const {
    auto prefix = auth::extractKeyPrefix(token);
    auto candidates = store_->getApiKeysByPrefix(prefix);
    if (candidates.empty()) return std::nullopt;

    auto token_hash = auth::hashApiKey(token);

    for (const auto& key : candidates) {
        if (!auth::verifyApiKey(key.key_hash, token_hash)) continue;

        if (key.status == "revoked") return std::nullopt;
        if (key.status == "expired") return std::nullopt;
        if (!key.expires_at.empty()) {
            auto now = std::chrono::system_clock::now();
            auto now_t = std::chrono::system_clock::to_time_t(now);
            struct tm buf{};
            gmtime_r(&now_t, &buf);
            char ts[32];
            strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &buf);
            if (std::string(ts) > key.expires_at) return std::nullopt;
        }

        auto user = store_->getUser(key.user_id);
        if (!user || user->status != "active") return std::nullopt;

        auto tenant = store_->getTenant(key.tenant_id);
        if (!tenant || tenant->status != "active") return std::nullopt;

        AuthContext ctx;
        ctx.tenant_id = key.tenant_id;
        ctx.user_id = key.user_id;
        ctx.api_key_id = key.id;
        ctx.role = key.role;
        ctx.is_rbac_enabled = true;
        return ctx;
    }

    return std::nullopt;
}

std::optional<AuthContext> AuthService::resolveLegacy(const std::string& token) const {
    if (!config_) return std::nullopt;

    auto token_hash = crypto::sha256(token);
    auto keys = config_->authApiKeys();
    for (const auto& configured : keys) {
        auto cfg_hash = crypto::sha256(configured);
        if (crypto::constantTimeEquals(token_hash, cfg_hash)) {
            AuthContext ctx;
            ctx.role = Role::SuperAdmin;
            ctx.is_rbac_enabled = false;
            return ctx;
        }
    }

    auto admin = config_->adminApiKey();
    if (!admin.empty()) {
        auto admin_hash = crypto::sha256(admin);
        if (crypto::constantTimeEquals(token_hash, admin_hash)) {
            AuthContext ctx;
            ctx.role = Role::SuperAdmin;
            ctx.is_rbac_enabled = false;
            return ctx;
        }
    }

    return std::nullopt;
}

std::optional<AuthContext> AuthService::resolveSession(const std::string& session_id) const {
    if (session_id.empty()) return std::nullopt;

    auto session = const_cast<SessionManager&>(session_mgr_).getSession(session_id);
    if (!session) return std::nullopt;

    auto user = store_->getUser(session->user_id);
    if (!user || user->status != "active") return std::nullopt;

    auto tenant = store_->getTenant(session->tenant_id);
    if (!tenant || tenant->status != "active") return std::nullopt;

    AuthContext ctx;
    ctx.tenant_id = session->tenant_id;
    ctx.user_id = session->user_id;
    ctx.role = user->role;
    ctx.is_rbac_enabled = true;
    ctx.session_id = session->id;
    ctx.auth_method = session->auth_method;
    ctx.mfa_verified = session->mfa_verified;
    return ctx;
}

bool AuthService::isSsoEnabled(const std::string& tenant_id) const {
    if (!config_ || !config_->ssoEnabled()) return false;
    if (!gate_ || !gate_->isEnabled(Feature::SSO)) return false;
    auto provider = store_->getSsoProviderByTenant(tenant_id);
    return provider.has_value();
}

bool AuthService::isMfaRequired(const AuthContext& ctx) const {
    if (!config_) return false;
    auto enforcement = config_->mfaEnforcement();
    if (enforcement == "disabled") return false;
    if (enforcement == "required") return true;
    if (enforcement == "optional") return false;
    // "role_based": SuperAdmin and TenantAdmin require MFA
    if (ctx.role >= Role::TenantAdmin) return true;
    return false;
}

SessionManager& AuthService::sessionManager() { return session_mgr_; }

} // namespace aegisgate
