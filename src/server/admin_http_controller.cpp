#include "server/admin_http_controller.h"
#include "server/admin_http_helpers.h"
#include "server/gateway_runtime.h"
#include "server/admin_controller.h"
#include "server/admin_session.h"
#include "auth/jwt_utils.h"
#include "auth/oidc_client.h"
#include "auth/identity_mapper.h"
#include "guardrail/admin/guard_admin_controller.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

namespace aegisgate {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// TASK-20260605-05 — buildAdmin 已移至 admin_http_helpers（free function /
// 与 withAdminAuth 共用）。此处引入以保持未迁移 handler 的 `buildAdmin(runtime)`
// 调用点不变。
using admin_http::buildAdmin;

namespace {

// Phase 11.1 (TASK-20260523-01 R2.5) — bridge between the project Role enum
// and the Adaptive Guard reviewer allowlist. SuperAdmin / TenantAdmin map
// onto the reviewer roles already encoded in `allowedReviewerRoles()`. Other
// roles surface as their raw string, which the allowlist will reject.
std::string mapToReviewerRole(Role r) {
    switch (r) {
        case Role::SuperAdmin:  return "security_admin";
        case Role::TenantAdmin: return "platform_admin";
        default:                return roleToString(r);
    }
}

guard::GuardAdminContext toGuardCtx(const AuthContext& ctx) {
    return guard::GuardAdminContext{mapToReviewerRole(ctx.role), ctx.user_id,
                                     ctx.tenant_id};
}

drogon::HttpResponsePtr forwardGuardResult(
    const drogon::HttpRequestPtr& req,
    const guard::GuardAdminResult& res,
    std::function<drogon::HttpResponsePtr(int, const nlohmann::json&, const drogon::HttpRequestPtr&)> ok,
    std::function<drogon::HttpResponsePtr(ErrorCode, const std::string&, const drogon::HttpRequestPtr&)> err) {
    if (!res.is_error) {
        return ok(res.status, res.body, req);
    }
    // Adaptive Guard errors use stable string codes; map the most common
    // status codes onto the project ErrorCode enum so the existing admin
    // response envelope keeps working without bespoke wiring.
    auto map_status = [](int s) {
        switch (s) {
            case 400: return ErrorCode::InvalidRequest;
            case 403: return ErrorCode::InsufficientPermissions;
            case 404: return ErrorCode::ApprovalNotFound;
            case 409: return ErrorCode::InvalidRequest;
            case 429: return ErrorCode::RateLimitExceeded;
            case 503: return ErrorCode::CacheUnavailable;
            default:  return ErrorCode::InternalError;
        }
    };
    auto msg = res.body.value("/error/message"_json_pointer, res.error_code);
    return err(map_status(res.status), msg, req);
}

}  // namespace

RateLimiter& AdminHttpController::loginRateLimiter() {
    static RateLimiter limiter({kLoginRateMaxTokens, kLoginRateRefillRate});
    return limiter;
}

bool AdminHttpController::isOriginAllowed(const std::string& origin) const {
    return admin_http::isOriginAllowed(origin);
}

bool AdminHttpController::isIpAllowed(const std::string& ip) const {
    auto& config = GatewayRuntime::instance().config();
    return admin::isAdminIpAllowed(ip, config.adminAllowedIps());
}

drogon::HttpResponsePtr AdminHttpController::checkAdminIpAccess(
    const drogon::HttpRequestPtr& req) {
    auto client_ip = admin_http::clientIp(req);  // P2-5（SR-5）
    if (!isIpAllowed(client_ip)) {
        spdlog::warn("Admin access denied from IP: {}", client_ip);
        return makeAdminError(ErrorCode::InsufficientPermissions,
                              "Access denied: IP not in admin allowlist", req);
    }
    return nullptr;
}

void AdminHttpController::applyCorsHeaders(
    const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
    admin_http::applyCorsHeaders(req, resp);
}

void AdminHttpController::applyCspHeader(const drogon::HttpResponsePtr& resp) {
    admin_http::applyCspHeader(resp);
}

drogon::HttpResponsePtr AdminHttpController::makeAdminResponse(
    int status, const nlohmann::json& body, const drogon::HttpRequestPtr& req) {
    return admin_http::makeAdminResponse(status, body, req);
}

drogon::HttpResponsePtr AdminHttpController::makeAdminError(
    ErrorCode code, const std::string& msg, const drogon::HttpRequestPtr& req) {
    return admin_http::makeAdminError(code, msg, req);
}

drogon::HttpResponsePtr AdminHttpController::handlePreflight(
    const drogon::HttpRequestPtr& req) {
    return admin_http::handlePreflight(req);
}

std::optional<AuthContext> AdminHttpController::authenticateRequest(
    const drogon::HttpRequestPtr& req) {
    return admin_http::authenticateRequest(req);
}

std::optional<AuthContext> AdminHttpController::authenticatePendingMfaRequest(
    const drogon::HttpRequestPtr& req) {
    // TASK-20260604-01 P0-F / SR-2：放行"已认证但 MFA 未验证"的 SSO session，
    // 仅供 MFA 挑战端点使用，打破登录期循环依赖。
    auto& runtime = GatewayRuntime::instance();
    auto& config = runtime.config();
    return admin::resolvePendingMfaSession(
        req->getCookie(kSessionCookie),
        admin_http::clientIp(req),  // P2-5（SR-5）
        config.adminAllowedIps(),
        runtime.authService());
}

std::optional<AuthContext> AdminHttpController::authenticateMfaChallengeRequest(
    const drogon::HttpRequestPtr& req) {
    // TASK-20260703-02 C5：MFA 挑战端点统一认证。常规会话优先，被 MFA 闸门拒绝
    // 时回退预 MFA 态（首次绑定死锁修复）。setup/verify/recovery 共用此入口，
    // 结构上杜绝"某端点漏接 pending 回退"。
    auto& runtime = GatewayRuntime::instance();
    auto& config = runtime.config();
    return admin::resolveMfaChallengeSession(
        req->getCookie(kSessionCookie),
        admin_http::clientIp(req),  // P2-5（SR-5）
        config.adminAllowedIps(),
        runtime.authService(),
        config.adminJwtSecret());
}

// ---------------------------------------------------------------------------
// Login / Logout / Me
// ---------------------------------------------------------------------------

void AdminHttpController::login(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    if (auto denied = checkAdminIpAccess(req)) {
        callback(denied);
        return;
    }

    auto client_ip = admin_http::clientIp(req);  // P2-5（SR-5）
    if (!loginRateLimiter().allow(client_ip)) {
        spdlog::warn("Admin login rate limited: ip={}", client_ip);
        auto resp = makeAdminError(ErrorCode::RateLimitExceeded,
                                   "Too many login attempts, please try again later", req);
        resp->addHeader("Retry-After", "60");
        callback(resp);
        return;
    }

    auto& runtime = GatewayRuntime::instance();

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req->body());
    } catch (...) {
        callback(makeAdminError(ErrorCode::InvalidRequest, "Invalid JSON body", req));
        return;
    }

    auto api_key = body.value("api_key", std::string{});
    if (api_key.empty()) {
        callback(makeAdminError(ErrorCode::MissingRequiredField,
                                "Field 'api_key' is required", req));
        return;
    }

    auto* auth_svc = runtime.authService();
    if (!auth_svc) {
        callback(makeAdminError(ErrorCode::InvalidApiKey,
                                "Auth service not available", req));
        return;
    }

    auto auth_ctx = auth_svc->resolve(api_key);
    if (!auth_ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey,
                                "Invalid API key", req));
        return;
    }

    // SR-2（TASK-20260702-01）：api_key→JWT 登录无法完成 MFA 校验。当该主体按
    // 策略需要 MFA 时，拒绝签发 JWT（否则 resolveAdminSession 亦会拒绝该会话，
    // 提前返回明确错误引导走 SSO+MFA），防止绕过 MFA 闸门。
    if (auth_svc->isMfaRequired(*auth_ctx)) {
        spdlog::warn("Admin login refused: MFA required, api_key path cannot satisfy MFA");
        callback(makeAdminError(ErrorCode::InsufficientPermissions,
                                "MFA is required; sign in via SSO with MFA instead of API key",
                                req));
        return;
    }

    auto& config = runtime.config();
    auto secret = config.adminJwtSecret();
    if (secret.empty()) {
        callback(makeAdminError(ErrorCode::InternalError,
                                "JWT secret not configured", req));
        return;
    }

    int expire_seconds = config.adminJwtExpireSeconds();

    JwtPayload payload;
    payload.user_id = auth_ctx->user_id;
    payload.tenant_id = auth_ctx->tenant_id;
    payload.role = roleToString(auth_ctx->role);

    auto token = JwtUtils::sign(payload, secret, expire_seconds);

    nlohmann::json resp_body;
    resp_body["user_id"] = auth_ctx->user_id;
    resp_body["tenant_id"] = auth_ctx->tenant_id;
    resp_body["role"] = roleToString(auth_ctx->role);

    auto resp = makeAdminResponse(200, resp_body, req);

    std::string cookie_value = std::string(kSessionCookie) + "=" + token
        + "; HttpOnly; SameSite=Strict; Path=/admin; Max-Age="
        + std::to_string(expire_seconds);
    if (config.tlsEnabled()) {
        cookie_value += "; Secure";
    }
    resp->addHeader("Set-Cookie", cookie_value);

    spdlog::info("Admin login: user={} tenant={} role={}",
                 auth_ctx->user_id, auth_ctx->tenant_id,
                 roleToString(auth_ctx->role));
    callback(resp);
}

void AdminHttpController::logout(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    nlohmann::json body;
    body["logged_out"] = true;
    auto resp = makeAdminResponse(200, body, req);

    auto& config = GatewayRuntime::instance().config();
    std::string cookie_value = std::string(kSessionCookie)
        + "=; HttpOnly; SameSite=Strict; Path=/admin; Max-Age=0";
    if (config.tlsEnabled()) {
        cookie_value += "; Secure";
    }
    resp->addHeader("Set-Cookie", cookie_value);
    callback(resp);
}

void AdminHttpController::me(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    auto ctx = authenticateRequest(req);
    if (!ctx) {
        // P0-F：常规认证失败时，若是预MFA态 SSO session → 返回 mfa_pending=true
        //（200），驱动前端跳 /mfa-challenge 而非登录页。
        auto pending = authenticatePendingMfaRequest(req);
        if (pending) {
            nlohmann::json body;
            body["user_id"] = pending->user_id;
            body["tenant_id"] = pending->tenant_id;
            body["role"] = roleToString(pending->role);
            body["mfa_pending"] = true;
            callback(makeAdminResponse(200, body, req));
            return;
        }
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }

    nlohmann::json body;
    body["user_id"] = ctx->user_id;
    body["tenant_id"] = ctx->tenant_id;
    body["role"] = roleToString(ctx->role);
    body["mfa_pending"] = false;
    callback(makeAdminResponse(200, body, req));
}

// ---------------------------------------------------------------------------
// Tenant endpoints
// ---------------------------------------------------------------------------

void AdminHttpController::createTenant(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            nlohmann::json body;
            try { body = nlohmann::json::parse(req->body()); }
            catch (...) { return AdminResult::error(ErrorCode::InvalidRequest, "Invalid JSON body"); }
            return admin.createTenant(ctx, body);
        });
}

void AdminHttpController::listTenants(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            int limit = 100, offset = 0;
            auto limit_str = req->getParameter("limit");
            auto offset_str = req->getParameter("offset");
            if (!limit_str.empty()) try { limit = std::stoi(limit_str); } catch (...) {}
            if (!offset_str.empty()) try { offset = std::stoi(offset_str); } catch (...) {}
            return admin.listTenants(ctx, limit, offset);
        });
}

void AdminHttpController::getTenant(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            return admin.getTenant(ctx, id);
        });
}

void AdminHttpController::updateTenant(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            nlohmann::json body;
            try { body = nlohmann::json::parse(req->body()); }
            catch (...) { return AdminResult::error(ErrorCode::InvalidRequest, "Invalid JSON body"); }
            return admin.updateTenant(ctx, id, body);
        });
}

void AdminHttpController::deleteTenant(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            return admin.deleteTenant(ctx, id);
        });
}

// ---------------------------------------------------------------------------
// User endpoints
// ---------------------------------------------------------------------------

void AdminHttpController::createUser(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            nlohmann::json body;
            try { body = nlohmann::json::parse(req->body()); }
            catch (...) { return AdminResult::error(ErrorCode::InvalidRequest, "Invalid JSON body"); }
            return admin.createUser(ctx, body);
        });
}

void AdminHttpController::listUsers(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            auto tenant_id = std::string(req->getParameter("tenant_id"));
            int limit = 100, offset = 0;
            auto limit_str = req->getParameter("limit");
            auto offset_str = req->getParameter("offset");
            if (!limit_str.empty()) try { limit = std::stoi(limit_str); } catch (...) {}
            if (!offset_str.empty()) try { offset = std::stoi(offset_str); } catch (...) {}
            return admin.listUsers(ctx, tenant_id, limit, offset);
        });
}

void AdminHttpController::getUser(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            return admin.getUser(ctx, id);
        });
}

void AdminHttpController::updateUser(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            nlohmann::json body;
            try { body = nlohmann::json::parse(req->body()); }
            catch (...) { return AdminResult::error(ErrorCode::InvalidRequest, "Invalid JSON body"); }
            return admin.updateUser(ctx, id, body);
        });
}

void AdminHttpController::deleteUser(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            return admin.deleteUser(ctx, id);
        });
}

// ---------------------------------------------------------------------------
// API Key endpoints
// ---------------------------------------------------------------------------

void AdminHttpController::createApiKey(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            nlohmann::json body;
            try { body = nlohmann::json::parse(req->body()); }
            catch (...) { return AdminResult::error(ErrorCode::InvalidRequest, "Invalid JSON body"); }
            return admin.createApiKey(ctx, body);
        });
}

void AdminHttpController::listApiKeys(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            auto tenant_id = std::string(req->getParameter("tenant_id"));
            int limit = 100, offset = 0;
            auto limit_str = req->getParameter("limit");
            auto offset_str = req->getParameter("offset");
            if (!limit_str.empty()) try { limit = std::stoi(limit_str); } catch (...) {}
            if (!offset_str.empty()) try { offset = std::stoi(offset_str); } catch (...) {}
            return admin.listApiKeys(ctx, tenant_id, limit, offset);
        });
}

void AdminHttpController::revokeApiKey(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            return admin.revokeApiKey(ctx, id);
        });
}

void AdminHttpController::rotateApiKey(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            return admin.rotateApiKey(ctx, id);
        });
}

// ---------------------------------------------------------------------------
// Audit / Cost / Dashboard
// ---------------------------------------------------------------------------

void AdminHttpController::queryAudits(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            auto tenant_id = std::string(req->getParameter("tenant_id"));
            int limit = 100, offset = 0;
            auto limit_str = req->getParameter("limit");
            auto offset_str = req->getParameter("offset");
            if (!limit_str.empty()) try { limit = std::stoi(limit_str); } catch (...) {}
            if (!offset_str.empty()) try { offset = std::stoi(offset_str); } catch (...) {}
            return admin.queryAudits(ctx, tenant_id, limit, offset);
        });
}

void AdminHttpController::queryCosts(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }

    auto tenant_id = std::string(req->getParameter("tenant_id"));
    auto model = std::string(req->getParameter("model"));
    int limit = 100, offset = 0;
    auto limit_str = req->getParameter("limit");
    auto offset_str = req->getParameter("offset");
    if (!limit_str.empty()) try { limit = std::stoi(limit_str); } catch (...) {}
    if (!offset_str.empty()) try { offset = std::stoi(offset_str); } catch (...) {}

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.queryCosts(*ctx, tenant_id, model, limit, offset);
    callback(makeAdminResponse(result.status, result.body, req));
}

void AdminHttpController::dashboardSummary(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.dashboardSummary(*ctx);
    callback(makeAdminResponse(result.status, result.body, req));
}

void AdminHttpController::savingsSummary(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }

    const std::string from = req->getParameter("from");
    const std::string to = req->getParameter("to");
    const std::string tenant_id = req->getParameter("tenant_id");

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.getSavingsSummary(*ctx, from, to, tenant_id);
    callback(makeAdminResponse(result.status, result.body, req));
}

// TASK-20260527-02 — MVP-5 prep / Case Study Numbers headline endpoint.
// Thin wrapper over AdminController::caseStudyHeadline. SR1 RBAC and SR2
// baseline_cost-internal-only are enforced inside the controller; this
// HTTP wrapper only authenticates + delegates.
void AdminHttpController::caseStudyHeadline(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.caseStudyHeadline(*ctx);
    callback(makeAdminResponse(result.status, result.body, req));
}

void AdminHttpController::securityEvents(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.getSecurityEvents(*ctx);
    callback(makeAdminResponse(result.status, result.body, req));
}

// ---------------------------------------------------------------------------
// SSO helpers
// ---------------------------------------------------------------------------

std::unordered_map<std::string, AdminHttpController::PendingSsoAuth>&
AdminHttpController::pendingSsoMap() {
    static std::unordered_map<std::string, PendingSsoAuth> map;
    return map;
}

std::mutex& AdminHttpController::pendingSsoMutex() {
    static std::mutex mu;
    return mu;
}

OidcClient& AdminHttpController::oidcClient() {
    static OidcClient client;
    return client;
}

IdentityMapper& AdminHttpController::identityMapper() {
    static IdentityMapper mapper(
        GatewayRuntime::instance().pipeline().persistent_store.get());
    return mapper;
}

void AdminHttpController::cleanupExpiredPendingSso() {
    auto now = std::chrono::steady_clock::now();
    auto& map = pendingSsoMap();
    for (auto it = map.begin(); it != map.end(); ) {
        if (now - it->second.created_at > kPendingSsoTtl) {
            it = map.erase(it);
        } else {
            ++it;
        }
    }
}

drogon::HttpResponsePtr AdminHttpController::makeRedirect(
    const std::string& url, const drogon::HttpRequestPtr& req) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k302Found);
    resp->addHeader("Location", url);
    applyCorsHeaders(req, resp);
    applyCspHeader(resp);
    return resp;
}

// ---------------------------------------------------------------------------
// SSO Auth Flow
// ---------------------------------------------------------------------------

void AdminHttpController::ssoLogin(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    // TASK-20260603-02 P0-3：SSO 入口与 /login 一致前置 IP allowlist 检查，
    // 此前 ssoLogin/ssoCallback 绕过 checkAdminIpAccess → 白名单外可发起 SSO 登录。
    if (auto denied = checkAdminIpAccess(req)) {
        callback(denied);
        return;
    }

    auto& runtime = GatewayRuntime::instance();
    auto tenant_id = std::string(req->getParameter("tenant_id"));
    if (tenant_id.empty()) {
        tenant_id = runtime.config().ssoDefaultProvider();
    }
    if (tenant_id.empty()) {
        callback(makeAdminError(ErrorCode::MissingRequiredField,
                                "Query parameter 'tenant_id' is required", req));
        return;
    }

    AdminController admin_ctrl(
        runtime.pipeline().persistent_store.get(),
        runtime.authService(), runtime.pipeline().audit_logger,
        &oidcClient(), &identityMapper(),
        runtime.savingsAggregator(), runtime.pipeline().semantic_cache);

    auto result = admin_ctrl.initiateSsoLogin(tenant_id);
    if (!result.error.empty()) {
        callback(makeAdminError(ErrorCode::InvalidRequest, result.error, req));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pendingSsoMutex());
        cleanupExpiredPendingSso();
        pendingSsoMap()[result.state] = PendingSsoAuth{
            result.code_verifier,
            result.nonce,
            tenant_id,
            std::chrono::steady_clock::now()
        };
    }

    spdlog::info("SSO login initiated: tenant={} state={}...", tenant_id,
                 result.state.substr(0, 8));
    callback(makeRedirect(result.redirect_url, req));
}

void AdminHttpController::ssoCallback(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    // TASK-20260603-02 P0-3：callback 命中用户浏览器 IP，allowlist 语义成立；
    // 与 ssoLogin 一致前置 checkAdminIpAccess（D3=A：显式 403）。
    if (auto denied = checkAdminIpAccess(req)) {
        callback(denied);
        return;
    }

    auto error_param = std::string(req->getParameter("error"));
    if (!error_param.empty()) {
        auto desc = std::string(req->getParameter("error_description"));
        spdlog::warn("SSO callback error: {} - {}", error_param, desc);
        callback(makeAdminError(ErrorCode::InvalidRequest,
                                "SSO error: " + error_param + " - " + desc, req));
        return;
    }

    auto code = std::string(req->getParameter("code"));
    auto state = std::string(req->getParameter("state"));
    if (code.empty() || state.empty()) {
        callback(makeAdminError(ErrorCode::MissingRequiredField,
                                "Missing 'code' or 'state' parameter", req));
        return;
    }

    PendingSsoAuth pending;
    {
        std::lock_guard<std::mutex> lock(pendingSsoMutex());
        auto& map = pendingSsoMap();
        auto it = map.find(state);
        if (it == map.end()) {
            callback(makeAdminError(ErrorCode::InvalidRequest,
                                    "Invalid or expired state parameter", req));
            return;
        }
        pending = it->second;
        map.erase(it);
    }

    auto now = std::chrono::steady_clock::now();
    if (now - pending.created_at > kPendingSsoTtl) {
        callback(makeAdminError(ErrorCode::InvalidRequest,
                                "SSO state expired", req));
        return;
    }

    auto& runtime = GatewayRuntime::instance();
    AdminController admin_ctrl(
        runtime.pipeline().persistent_store.get(),
        runtime.authService(), runtime.pipeline().audit_logger,
        &oidcClient(), &identityMapper(),
        runtime.savingsAggregator(), runtime.pipeline().semantic_cache);

    auto ip = admin_http::clientIp(req);  // P2-5（SR-5）
    auto ua = std::string(req->getHeader("User-Agent"));

    auto result = admin_ctrl.handleSsoCallback(
        code, pending.code_verifier, pending.nonce,
        pending.tenant_id, ip, ua);

    if (!result.success) {
        spdlog::warn("SSO callback failed: {}", result.error);
        callback(makeAdminError(ErrorCode::InvalidRequest, result.error, req));
        return;
    }

    auto& config = runtime.config();
    int max_age = config.sessionAbsoluteTimeoutSeconds();

    std::string cookie_value = std::string(kSessionCookie) + "=" + result.session.id
        + "; HttpOnly; SameSite=Lax; Path=/admin; Max-Age=" + std::to_string(max_age);
    if (config.tlsEnabled()) {
        cookie_value += "; Secure";
    }

    auto resp = makeRedirect("/admin/", req);
    resp->addHeader("Set-Cookie", cookie_value);

    spdlog::info("SSO login successful: user={} tenant={}",
                 result.session.user_id, result.session.tenant_id);
    callback(resp);
}

void AdminHttpController::ssoLogout(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    auto ctx = authenticateRequest(req);

    auto& runtime = GatewayRuntime::instance();
    auto& config = runtime.config();

    std::string end_session_url;
    if (ctx) {
        AdminController admin_ctrl(
            runtime.pipeline().persistent_store.get(),
            runtime.authService(), runtime.pipeline().audit_logger,
            &oidcClient(), &identityMapper(),
            runtime.savingsAggregator(), runtime.pipeline().semantic_cache);

        end_session_url = admin_ctrl.handleSsoLogout(
            ctx->session_id, ctx->tenant_id);
    }

    std::string cookie_value = std::string(kSessionCookie)
        + "=; HttpOnly; SameSite=Lax; Path=/admin; Max-Age=0";
    if (config.tlsEnabled()) {
        cookie_value += "; Secure";
    }

    auto redirect_target = end_session_url.empty() ? "/admin/login" : end_session_url;
    auto resp = makeRedirect(redirect_target, req);
    resp->addHeader("Set-Cookie", cookie_value);

    spdlog::info("SSO logout: user={}", ctx ? ctx->user_id : "(none)");
    callback(resp);
}

// ---------------------------------------------------------------------------
// SSO Provider CRUD
// ---------------------------------------------------------------------------

void AdminHttpController::listSsoProviders(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }

    int limit = 100, offset = 0;
    auto limit_str = req->getParameter("limit");
    auto offset_str = req->getParameter("offset");
    if (!limit_str.empty()) try { limit = std::stoi(limit_str); } catch (...) {}
    if (!offset_str.empty()) try { offset = std::stoi(offset_str); } catch (...) {}

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.listSsoProviders(*ctx, limit, offset);
    callback(makeAdminResponse(result.status, result.body, req));
}

void AdminHttpController::createSsoProvider(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req->body());
    } catch (...) {
        callback(makeAdminError(ErrorCode::InvalidRequest, "Invalid JSON body", req));
        return;
    }

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.createSsoProvider(*ctx, body);
    callback(makeAdminResponse(result.status, result.body, req));
}

void AdminHttpController::getSsoProvider(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.getSsoProvider(*ctx, id);
    callback(makeAdminResponse(result.status, result.body, req));
}

void AdminHttpController::updateSsoProvider(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req->body());
    } catch (...) {
        callback(makeAdminError(ErrorCode::InvalidRequest, "Invalid JSON body", req));
        return;
    }

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.updateSsoProvider(*ctx, id, body);
    callback(makeAdminResponse(result.status, result.body, req));
}

void AdminHttpController::deleteSsoProvider(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.deleteSsoProvider(*ctx, id);
    callback(makeAdminResponse(result.status, result.body, req));
}

// ---------------------------------------------------------------------------
// MFA endpoints
// ---------------------------------------------------------------------------

// --- Prompt Template management ---

void AdminHttpController::listPromptTemplates(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            auto tenant_id = std::string(req->getParameter("tenant_id"));
            int limit = 100, offset = 0;
            auto ls = req->getParameter("limit"); if (!ls.empty()) try { limit = std::stoi(ls); } catch (...) {}
            auto os = req->getParameter("offset"); if (!os.empty()) try { offset = std::stoi(os); } catch (...) {}
            return admin.listPromptTemplates(ctx, tenant_id, limit, offset);
        });
}

void AdminHttpController::createPromptTemplate(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            nlohmann::json body;
            try { body = nlohmann::json::parse(req->body()); }
            catch (...) { return AdminResult::error(ErrorCode::InvalidRequest, "Invalid JSON body"); }
            return admin.createPromptTemplate(ctx, body);
        });
}

void AdminHttpController::getPromptTemplate(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            return admin.getPromptTemplate(ctx, id);
        });
}

void AdminHttpController::updatePromptTemplate(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            nlohmann::json body;
            try { body = nlohmann::json::parse(req->body()); }
            catch (...) { return AdminResult::error(ErrorCode::InvalidRequest, "Invalid JSON body"); }
            return admin.updatePromptTemplate(ctx, id, body);
        });
}

void AdminHttpController::deletePromptTemplate(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            return admin.deletePromptTemplate(ctx, id);
        });
}

// --- Rule Set management ---

void AdminHttpController::listRuleSets(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            auto tenant_id = std::string(req->getParameter("tenant_id"));
            int limit = 20;
            auto limit_str = req->getParameter("limit");
            if (!limit_str.empty()) try { limit = std::stoi(limit_str); } catch (...) {}
            int offset = 0;
            auto offset_str = req->getParameter("offset");
            if (!offset_str.empty()) try { offset = std::stoi(offset_str); } catch (...) {}
            return admin.listRuleSets(ctx, tenant_id, limit, offset);
        });
}

void AdminHttpController::getActiveRuleSet(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            auto tenant_id = std::string(req->getParameter("tenant_id"));
            return admin.getActiveRuleSet(ctx, tenant_id);
        });
}

void AdminHttpController::createRuleSet(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            nlohmann::json body;
            try { body = nlohmann::json::parse(req->body()); }
            catch (...) { return AdminResult::error(ErrorCode::InvalidRequest, "Invalid JSON body"); }
            return admin.createRuleSet(ctx, body);
        });
}

void AdminHttpController::activateRuleSet(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            nlohmann::json body;
            try { body = nlohmann::json::parse(req->body()); }
            catch (...) { return AdminResult::error(ErrorCode::InvalidRequest, "Invalid JSON body"); }

            auto tenant_id = body.value("tenant_id", ctx.tenant_id);
            int64_t version = body.value("version", int64_t(0));
            if (version <= 0)
                return AdminResult::error(ErrorCode::InvalidRequest, "Missing or invalid 'version'");
            return admin.activateRuleSet(ctx, tenant_id, version);
        });
}

void AdminHttpController::mfaSetup(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    // C5：与 verify/recovery 统一走 MFA 挑战认证，enforcement=required 首次绑定
    // 的 pending session 才能拿到 secret（此前只 authenticateRequest → 死锁）。
    auto ctx = authenticateMfaChallengeRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.setupMfa(*ctx);
    if (result.is_error) {
        callback(makeAdminError(result.error_code,
                 result.body.value("/error/message"_json_pointer, ""), req));
    } else {
        callback(makeAdminResponse(result.status, result.body, req));
    }
}

void AdminHttpController::mfaVerify(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    // P0-F / C5：常规认证优先，失败再试预MFA态（登录挑战场景）。统一入口。
    auto ctx = authenticateMfaChallengeRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }

    auto body = nlohmann::json::parse(req->bodyData(), req->bodyData() + req->bodyLength(), nullptr, false);
    if (body.is_discarded()) body = nlohmann::json::object();

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.verifyMfa(*ctx, body);
    if (result.is_error) {
        callback(makeAdminError(result.error_code,
                 result.body.value("/error/message"_json_pointer, ""), req));
    } else {
        callback(makeAdminResponse(result.status, result.body, req));
    }
}

void AdminHttpController::mfaDisable(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }

    auto body = nlohmann::json::parse(req->bodyData(), req->bodyData() + req->bodyLength(), nullptr, false);
    if (body.is_discarded()) body = nlohmann::json::object();

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.disableMfa(*ctx, body);
    if (result.is_error) {
        callback(makeAdminError(result.error_code,
                 result.body.value("/error/message"_json_pointer, ""), req));
    } else {
        callback(makeAdminResponse(result.status, result.body, req));
    }
}

void AdminHttpController::mfaRecovery(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }

    // P0-F / C5：常规认证优先，失败再试预MFA态（登录挑战场景）。统一入口。
    auto ctx = authenticateMfaChallengeRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }

    auto body = nlohmann::json::parse(req->bodyData(), req->bodyData() + req->bodyLength(), nullptr, false);
    if (body.is_discarded()) body = nlohmann::json::object();

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.recoveryMfa(*ctx, body);
    if (result.is_error) {
        callback(makeAdminError(result.error_code,
                 result.body.value("/error/message"_json_pointer, ""), req));
    } else {
        callback(makeAdminResponse(result.status, result.body, req));
    }
}

// ---------------------------------------------------------------------------
// Usage Prediction
// ---------------------------------------------------------------------------

void AdminHttpController::predictUsage(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }
    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }
    auto tenant_id = std::string(req->getParameter("tenant_id"));
    int history_days = 30;
    int forecast_days = 7;
    try { history_days = std::stoi(std::string(req->getParameter("history_days"))); } catch (...) {}
    try { forecast_days = std::stoi(std::string(req->getParameter("forecast_days"))); } catch (...) {}

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.predictUsage(*ctx, tenant_id, history_days, forecast_days);
    if (result.is_error) {
        callback(makeAdminError(result.error_code,
                 result.body.value("/error/message"_json_pointer, ""), req));
    } else {
        callback(makeAdminResponse(result.status, result.body, req));
    }
}

void AdminHttpController::predictBudget(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }
    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }
    auto tenant_id = std::string(req->getParameter("tenant_id"));
    double budget = 1000.0;
    int history_days = 30;
    try { budget = std::stod(std::string(req->getParameter("budget"))); } catch (...) {}
    try { history_days = std::stoi(std::string(req->getParameter("history_days"))); } catch (...) {}

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.predictBudgetExhaustion(*ctx, tenant_id, budget, history_days);
    if (result.is_error) {
        callback(makeAdminError(result.error_code,
                 result.body.value("/error/message"_json_pointer, ""), req));
    } else {
        callback(makeAdminResponse(result.status, result.body, req));
    }
}

// ---------------------------------------------------------------------------
// Compliance reports (TASK-20260604-01 P0-C) — 业务 + RBAC + 审计 + 租户隔离
// 已在 AdminController 实现（SR-4），此处仅做认证 + 取参 + 转发。
// ---------------------------------------------------------------------------

void AdminHttpController::exportAuditReport(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    admin_http::withAdminAuth(req, std::move(callback),
        [&](const AuthContext& ctx, AdminController& admin) -> AdminResult {
            auto from = std::string(req->getParameter("from"));
            auto to = std::string(req->getParameter("to"));
            auto tenant_id = std::string(req->getParameter("tenant_id"));
            auto format = std::string(req->getParameter("format"));
            if (format.empty()) format = "csv";
            return admin.exportAuditReport(ctx, from, to, tenant_id, format);
        });
}

void AdminHttpController::exportCostReport(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }
    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey, "Not authenticated", req));
        return;
    }
    auto from = std::string(req->getParameter("from"));
    auto to = std::string(req->getParameter("to"));
    auto tenant_id = std::string(req->getParameter("tenant_id"));
    auto format = std::string(req->getParameter("format"));
    if (format.empty()) format = "csv";

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.exportCostReport(*ctx, from, to, tenant_id, format);
    if (result.is_error) {
        callback(makeAdminError(result.error_code,
                 result.body.value("/error/message"_json_pointer, ""), req));
    } else {
        callback(makeAdminResponse(result.status, result.body, req));
    }
}

// ---------------------------------------------------------------------------
// Phase 11.5 (TASK-20260518-02 E5.1) — Autonomy approval workflow
// ---------------------------------------------------------------------------
//
// All five route into the AdminController autonomy methods. They follow the
// existing handler shape used by savings/dashboard: preflight → authenticate
// → buildAdmin → dispatch → makeAdminResponse.

void AdminHttpController::listAutonomyProposals(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }
    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey,
                                 "Not authenticated", req));
        return;
    }
    auto state_filter  = std::string(req->getParameter("state"));
    auto source_filter = std::string(req->getParameter("source"));
    int limit = 100, offset = 0;
    auto ls = req->getParameter("limit");
    auto os = req->getParameter("offset");
    if (!ls.empty()) try { limit  = std::stoi(ls); } catch (...) {}
    if (!os.empty()) try { offset = std::stoi(os); } catch (...) {}

    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.listAutonomyProposals(
        *ctx, state_filter, source_filter, limit, offset);
    if (result.is_error) {
        callback(makeAdminError(result.error_code,
                  result.body.value("/error/message"_json_pointer, ""), req));
    } else {
        callback(makeAdminResponse(result.status, result.body, req));
    }
}

void AdminHttpController::approveAutonomyProposal(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {
    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }
    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey,
                                 "Not authenticated", req));
        return;
    }
    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.approveAutonomyProposal(*ctx, id);
    if (result.is_error) {
        callback(makeAdminError(result.error_code,
                  result.body.value("/error/message"_json_pointer, ""), req));
    } else {
        callback(makeAdminResponse(result.status, result.body, req));
    }
}

void AdminHttpController::rejectAutonomyProposal(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {
    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }
    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey,
                                 "Not authenticated", req));
        return;
    }
    nlohmann::json body;
    auto raw = std::string(req->getBody());
    if (!raw.empty()) {
        try { body = nlohmann::json::parse(raw); }
        catch (...) {
            callback(makeAdminError(ErrorCode::InvalidRequest,
                                     "Body must be JSON", req));
            return;
        }
    }
    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.rejectAutonomyProposal(*ctx, id, body);
    if (result.is_error) {
        callback(makeAdminError(result.error_code,
                  result.body.value("/error/message"_json_pointer, ""), req));
    } else {
        callback(makeAdminResponse(result.status, result.body, req));
    }
}

void AdminHttpController::rollbackAutonomyProposal(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {
    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }
    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey,
                                 "Not authenticated", req));
        return;
    }
    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.rollbackAutonomyProposal(*ctx, id);
    if (result.is_error) {
        callback(makeAdminError(result.error_code,
                  result.body.value("/error/message"_json_pointer, ""), req));
    } else {
        callback(makeAdminResponse(result.status, result.body, req));
    }
}

void AdminHttpController::autonomyReport(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }
    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey,
                                 "Not authenticated", req));
        return;
    }
    auto& runtime = GatewayRuntime::instance();
    auto admin_ctrl = buildAdmin(runtime);
    auto result = admin_ctrl.autonomyReport(*ctx);
    if (result.is_error) {
        callback(makeAdminError(result.error_code,
                  result.body.value("/error/message"_json_pointer, ""), req));
    } else {
        callback(makeAdminResponse(result.status, result.body, req));
    }
}

// ---------------------------------------------------------------------------
// Phase 11.1 (TASK-20260523-01 R2.5) — Adaptive Guard handlers
// ---------------------------------------------------------------------------

void AdminHttpController::postGuardFeedback(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }
    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey,
                                 "Not authenticated", req));
        return;
    }
    auto* gac = GatewayRuntime::instance().guardAdminController();
    if (!gac) {
        callback(makeAdminError(ErrorCode::CacheUnavailable,
                                 "Adaptive Guard not wired", req));
        return;
    }
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(std::string{req->getBody()});
    } catch (...) {
        callback(makeAdminError(ErrorCode::InvalidRequest,
                                 "invalid_json", req));
        return;
    }
    auto res = gac->postFeedback(toGuardCtx(*ctx), body);
    callback(forwardGuardResult(req, res,
        [this](int s, const nlohmann::json& b,
                const drogon::HttpRequestPtr& r) {
            return makeAdminResponse(s, b, r);
        },
        [this](ErrorCode c, const std::string& m,
                const drogon::HttpRequestPtr& r) {
            return makeAdminError(c, m, r);
        }));
}

void AdminHttpController::getGuardExplanation(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {
    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }
    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey,
                                 "Not authenticated", req));
        return;
    }
    auto* gac = GatewayRuntime::instance().guardAdminController();
    if (!gac) {
        callback(makeAdminError(ErrorCode::CacheUnavailable,
                                 "Adaptive Guard not wired", req));
        return;
    }
    auto res = gac->getExplanation(toGuardCtx(*ctx), id);
    callback(forwardGuardResult(req, res,
        [this](int s, const nlohmann::json& b,
                const drogon::HttpRequestPtr& r) {
            return makeAdminResponse(s, b, r);
        },
        [this](ErrorCode c, const std::string& m,
                const drogon::HttpRequestPtr& r) {
            return makeAdminError(c, m, r);
        }));
}

void AdminHttpController::postGuardModelPromote(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    if (req->method() == drogon::Options) {
        callback(handlePreflight(req));
        return;
    }
    auto ctx = authenticateRequest(req);
    if (!ctx) {
        callback(makeAdminError(ErrorCode::InvalidApiKey,
                                 "Not authenticated", req));
        return;
    }
    auto* gac = GatewayRuntime::instance().guardAdminController();
    if (!gac) {
        callback(makeAdminError(ErrorCode::CacheUnavailable,
                                 "Adaptive Guard not wired", req));
        return;
    }
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(std::string{req->getBody()});
    } catch (...) {
        callback(makeAdminError(ErrorCode::InvalidRequest,
                                 "invalid_json", req));
        return;
    }
    auto res = gac->promoteModel(toGuardCtx(*ctx), body);
    callback(forwardGuardResult(req, res,
        [this](int s, const nlohmann::json& b,
                const drogon::HttpRequestPtr& r) {
            return makeAdminResponse(s, b, r);
        },
        [this](ErrorCode c, const std::string& m,
                const drogon::HttpRequestPtr& r) {
            return makeAdminError(c, m, r);
        }));
}

} // namespace aegisgate
