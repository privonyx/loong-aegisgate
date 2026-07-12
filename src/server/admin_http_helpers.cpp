#include "server/admin_http_helpers.h"

#include "server/gateway_runtime.h"
#include "server/admin_session.h"

#include <spdlog/spdlog.h>
#include <mutex>

namespace aegisgate {
namespace admin_http {

namespace {
constexpr const char* kSessionCookie = "aegis_session";
constexpr const char* kCspValue =
    "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'";
}  // namespace

bool isOriginAllowed(const std::string& origin) {
    auto& config = GatewayRuntime::instance().config();
    return admin::decideCors(origin, config.adminCorsOrigins()).allowed;
}

std::string clientIp(const drogon::HttpRequestPtr& req) {
    auto& config = GatewayRuntime::instance().config();
    return admin::resolveClientIp(
        req->peerAddr().toIp(),
        std::string(req->getHeader("X-Forwarded-For")),
        config.adminTrustedProxies());
}

void applyCorsHeaders(const drogon::HttpRequestPtr& req,
                      const drogon::HttpResponsePtr& resp) {
    auto origin = std::string(req->getHeader("Origin"));
    auto& config = GatewayRuntime::instance().config();
    auto origins = config.adminCorsOrigins();
    // SR-1：配置含 `*` 时提示凭证将被禁用（进程内仅告警一次，避免每请求刷屏）。
    static std::once_flag cors_wildcard_warn;
    std::call_once(cors_wildcard_warn, [&origins]() {
        for (const auto& o : origins) {
            if (o == "*") {
                spdlog::warn("admin.cors_origins contains '*'; "
                             "Access-Control-Allow-Credentials disabled for "
                             "wildcard (CORS spec forbids '*' + credentials)");
                break;
            }
        }
    });
    auto d = admin::decideCors(origin, origins);
    if (!d.allowed) return;
    resp->addHeader("Access-Control-Allow-Origin", d.allow_origin);
    if (d.allow_credentials) {
        resp->addHeader("Access-Control-Allow-Credentials", "true");
    }
    if (d.vary_origin) {
        resp->addHeader("Vary", "Origin");
    }
    resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    resp->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    resp->addHeader("Access-Control-Max-Age", "86400");
}

void applyCspHeader(const drogon::HttpResponsePtr& resp) {
    resp->addHeader("Content-Security-Policy", kCspValue);
}

drogon::HttpResponsePtr makeAdminResponse(int status, const nlohmann::json& body,
                                          const drogon::HttpRequestPtr& req) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    resp->setContentTypeString("application/json");
    resp->setBody(body.dump());
    applyCorsHeaders(req, resp);
    applyCspHeader(resp);
    return resp;
}

drogon::HttpResponsePtr makeAdminError(ErrorCode code, const std::string& msg,
                                       const drogon::HttpRequestPtr& req) {
    nlohmann::json err;
    auto message = msg.empty() ? std::string(toDefaultMessage(code)) : msg;
    err["error"]["code"] = toAegisCode(code);
    err["error"]["type"] = toErrorType(code);
    err["error"]["message"] = message;
    return makeAdminResponse(toHttpStatus(code), err, req);
}

drogon::HttpResponsePtr handlePreflight(const drogon::HttpRequestPtr& req) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k204NoContent);
    applyCorsHeaders(req, resp);
    return resp;
}

drogon::HttpResponsePtr forwardAdminResult(const AdminResult& result,
                                           const drogon::HttpRequestPtr& req) {
    return makeAdminResponse(result.status, result.body, req);
}

std::optional<AuthContext> authenticateRequest(const drogon::HttpRequestPtr& req) {
    // TASK-20260603-02 P0-2：与 WS 通道共用 admin::resolveAdminSession（同一
    // IP → SSO session（含 MFA 闸门）→ JWT fallback 链），消除认证分裂。
    auto& runtime = GatewayRuntime::instance();
    auto& config = runtime.config();
    // P2-5（SR-5）：反代后 peer 恒为代理 IP；仅可信代理时采信 XFF 还原真实客户端。
    return admin::resolveAdminSession(
        req->getCookie(kSessionCookie),
        clientIp(req),
        config.adminAllowedIps(),
        runtime.authService(),
        config.adminJwtSecret());
}

AdminController buildAdmin(GatewayRuntime& runtime) {
    AdminController ctrl(runtime.pipeline().persistent_store.get(),
                         runtime.authService(),
                         runtime.pipeline().audit_logger);
    ctrl.setSavingsAggregator(runtime.savingsAggregator());
    ctrl.setSemanticCache(runtime.pipeline().semantic_cache);
    // TASK-20260711-01 / REV20260707-I13: wire AdvancedRouting license
    // gate so getSavingsSummary rejects Community edition requests with
    // a license-specific 403 (feature-list.md declares savings summary
    // as an Enterprise-tier capability).
    ctrl.setFeatureGate(runtime.pipeline().feature_gate.get());
    // P2-2（SR-2）：MFA 恢复码数量 + 失败锁定策略从 config 注入。
    {
        auto& cfg = runtime.config();
        ctrl.setMfaPolicy(cfg.mfaRecoveryCodeCount(),
                          cfg.mfaLockoutMaxFailures(),
                          cfg.mfaLockoutWindowSeconds());
    }
    // P2-4（SR-4）：规则集激活即时生效钩子（activate → 刷新该租户运行时桶）。
    ctrl.setRuleSetActivationHook([&runtime](const std::string& tid) {
        runtime.refreshTenantRules(tid);
    });
    // Phase 11.5 (TASK-20260518-02 E5.1) — wire the autonomy workflow +
    // queue. Both may be null when autonomy is disabled; the handlers
    // surface AEGIS-6002 (AutonomyDisabled) in that case.
    ctrl.setAutonomyWorkflow(runtime.approvalWorkflow());
    ctrl.setAutonomyQueue(runtime.approvalQueue());
    // TASK-20260527-02 — Case Study Numbers data sources (D1=A 3 块).
    // Either may be null in stripped-down test runtimes; caseStudyHeadline
    // gracefully degrades to zero values rather than 500.
    ctrl.setCostTracker(runtime.pipeline().cost_tracker);
    ctrl.setQualityMonitor(runtime.pipeline().quality_monitor.get());
    return ctrl;
}

void withAdminAuth(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    const std::function<AdminResult(const AuthContext&, AdminController&)>& fn) {
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
    AdminResult result = fn(*ctx, admin_ctrl);
    callback(makeAdminResponse(result.status, result.body, req));
}

} // namespace admin_http
} // namespace aegisgate
