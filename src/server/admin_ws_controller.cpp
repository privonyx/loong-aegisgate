#include "server/admin_ws_controller.h"
#include "server/admin_session.h"
#include "server/case_study_builder.h"
#include "server/gateway_runtime.h"
#include "auth/auth_models.h"
#include "auth/jwt_utils.h"
#include "cache/semantic_cache.h"
#include "observe/cost_tracker.h"
#include "observe/metrics.h"
#include "observe/quality_monitor.h"
#include "guardrail/audit.h"
#include <spdlog/spdlog.h>

namespace aegisgate {

void AdminWsController::handleNewConnection(
    const drogon::HttpRequestPtr& req,
    const drogon::WebSocketConnectionPtr& conn) {

    auto& runtime = GatewayRuntime::instance();
    auto& config = runtime.config();

    // TASK-20260603-02 P0-2：与 HTTP 通道共用 admin::resolveAdminSession，统一
    // IP allowlist → SSO session（含 MFA 闸门）→ JWT fallback 链，消除 WS 认证
    // 落后于 HTTP（此前 WS 仅 JwtUtils::verify、无 SSO session / 无 MFA 闸门）。
    //
    // NOTE: Drogon parses the request Cookie header into its cookie map for
    // WebSocket handshakes but does NOT expose it via getHeader("Cookie").
    // Use getCookie() — same accessor as AdminHttpController::authenticateRequest.
    // P2-5（SR-5）：反代后 peer 恒为代理 IP；仅可信代理时采信 XFF 还原真实客户端。
    auto client_ip = admin::resolveClientIp(
        req->peerAddr().toIp(),
        std::string(req->getHeader("X-Forwarded-For")),
        config.adminTrustedProxies());
    auto ctx = admin::resolveAdminSession(
        req->getCookie("aegis_session"),
        client_ip,
        config.adminAllowedIps(),
        runtime.authService(),
        config.adminJwtSecret());

    if (!ctx) {
        spdlog::warn("Admin WS access denied from IP: {}", client_ip);
        conn->shutdown(drogon::CloseCode::kViolation, "Authentication failed");
        return;
    }

    conn->setContext(std::make_shared<AuthContext>(*ctx));

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.insert(conn);
    }

    spdlog::info("Admin WS connected: user={} tenant={}, total={}",
                 ctx->user_id, ctx->tenant_id, connectionCount());

    if (!timer_started_.exchange(true)) {
        startMetricsTimer();
        startCaseStudyTimer();
        startAuditSubscription();
    }
}

void AdminWsController::handleNewMessage(
    const drogon::WebSocketConnectionPtr& conn,
    std::string&& message,
    const drogon::WebSocketMessageType& type) {

    if (type == drogon::WebSocketMessageType::Ping) {
        conn->send("", drogon::WebSocketMessageType::Pong);
        return;
    }

    try {
        auto msg = nlohmann::json::parse(message);
        auto cmd = msg.value("type", std::string{});
        if (cmd == "ping") {
            nlohmann::json pong;
            pong["type"] = "pong";
            conn->send(pong.dump());
        }
    } catch (...) {
        // Ignore malformed messages
    }
}

void AdminWsController::handleConnectionClosed(
    const drogon::WebSocketConnectionPtr& conn) {

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.erase(conn);
    }

    spdlog::info("Admin WS disconnected, remaining={}", connectionCount());

    if (connectionCount() == 0) {
        stopAuditSubscription();
    }
}

size_t AdminWsController::connectionCount() const {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    return connections_.size();
}

void AdminWsController::startMetricsTimer() {
    auto loop = drogon::app().getLoop();
    if (!loop) return;

    loop->runEvery(2.0, [this] {
        if (connectionCount() == 0) return;
        // TASK-20260603-02 P0-1 / D1=A：全局聚合 metrics 仅推送给 SuperAdmin 连接。
        // 非 super 连接不再收 metrics（跨租户聚合泄漏），仍可用 HTTP dashboard RBAC。
        auto snapshot = buildMetricsSnapshot();
        auto data = snapshot.dump();
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto& conn : connections_) {
            auto ctx = conn->getContext<AuthContext>();
            if (!ctx) continue;
            if (admin::shouldDeliverGlobalMetrics(ctx->role == Role::SuperAdmin)) {
                conn->send(data);
            }
        }
    });
}

// TASK-20260527-02 — Case Study Numbers WS push (30s throttle).
// TASK-20260602-01 Epic 2 + D5 — Per-connection push for SR-NEW1 WS-RBAC
// consistency: each connection sees only its own tenant scope (matches
// AdminController::caseStudyHeadline HTTP RBAC behavior).
void AdminWsController::startCaseStudyTimer() {
    auto loop = drogon::app().getLoop();
    if (!loop) return;

    loop->runEvery(30.0, [this] {
        if (connectionCount() == 0) return;
        auto& runtime = GatewayRuntime::instance();

        // 构造一次 Inputs base 以便所有连接共用 source 指针。
        admin::CaseStudyInputs base;
        base.cost_tracker      = runtime.pipeline().cost_tracker;
        base.semantic_cache    = runtime.pipeline().semantic_cache;
        base.quality_monitor   = runtime.pipeline().quality_monitor.get();
        base.savings_aggregator = runtime.savingsAggregator();
        base.include_envelope  = false;  // WS data 块 only; envelope by type+data

        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto& conn : connections_) {
            auto ctx = conn->getContext<AuthContext>();
            if (!ctx) continue;

            const bool is_super = ctx->role == Role::SuperAdmin;

            admin::CaseStudyInputs in = base;
            in.is_super = is_super;
            in.tenant_id = ctx->tenant_id;

            nlohmann::json msg;
            msg["type"] = "case_study";
            msg["data"] = admin::buildCaseStudySnapshot(in);
            // 同时携带 scope / tenant_id 以便前端 router（不在 data 体内即可读）。
            msg["data"]["scope"] = is_super ? "global" : "tenant";
            if (!is_super) {
                msg["data"]["tenant_id"] = ctx->tenant_id;
            }
            conn->send(msg.dump());
        }
    });
}

void AdminWsController::startAuditSubscription() {
    auto& runtime = GatewayRuntime::instance();
    auto* audit = runtime.pipeline().audit_logger;
    if (!audit) return;

    // TASK-20260603-02 P0-1：审计流逐连接投递（弃 broadcastJson）。super 看全部
    // 租户的 entry，其余连接仅收 tenant_id 匹配自身的 entry → 消除跨租户审计泄漏。
    auto sub_id = audit->subscribe([this](const AuditEntry& entry) {
        nlohmann::json msg;
        msg["type"] = "audit";
        msg["data"]["request_id"] = entry.request_id;
        msg["data"]["timestamp"] = entry.timestamp;
        msg["data"]["tenant_id"] = entry.tenant_id;
        msg["data"]["action"] = entry.action;
        msg["data"]["stage"] = entry.stage_name;
        msg["data"]["detail"] = entry.detail;
        auto data = msg.dump();

        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto& conn : connections_) {
            auto ctx = conn->getContext<AuthContext>();
            if (!ctx) continue;
            const bool is_super = ctx->role == Role::SuperAdmin;
            if (admin::shouldDeliverAuditToConnection(
                    is_super, ctx->tenant_id, entry.tenant_id)) {
                conn->send(data);
            }
        }
    });

    audit_sub_id_.store(sub_id);
}

void AdminWsController::stopAuditSubscription() {
    auto id = audit_sub_id_.exchange(0);
    if (id == 0) return;

    auto& runtime = GatewayRuntime::instance();
    auto* audit = runtime.pipeline().audit_logger;
    if (audit) {
        audit->unsubscribe(id);
    }
}

nlohmann::json AdminWsController::buildMetricsSnapshot() {
    auto& runtime = GatewayRuntime::instance();
    auto* store = runtime.pipeline().persistent_store.get();

    nlohmann::json snapshot;
    snapshot["type"] = "metrics";

    int64_t total_requests = 0;
    int64_t total_cost_records = 0;
    int active_tenants = 0;
    float cache_hit_rate = 0.0f;

    if (store) {
        total_requests = store->auditCount();
        total_cost_records = store->costRecordCount();
        auto tenants = store->listTenants(10000, 0);
        active_tenants = 0;
        for (const auto& t : tenants) {
            if (t.status == "active") ++active_tenants;
        }
    }

    auto* cache = runtime.pipeline().semantic_cache;
    if (cache) {
        auto stats = cache->getStats();
        cache_hit_rate = stats.hit_rate;
    }

    snapshot["data"]["total_requests"] = total_requests;
    snapshot["data"]["active_tenants"] = active_tenants;
    snapshot["data"]["total_cost_records"] = total_cost_records;
    snapshot["data"]["cache_hit_rate"] = cache_hit_rate;

    return snapshot;
}

// TASK-20260602-01 Epic 2 — buildCaseStudySnapshot() 已抽离到
// src/server/case_study_builder.{h,cpp}，与 AdminController::caseStudyHeadline
// 共用，消除双份构造的 schema 漂移风险。本控制器的 30s timer 改为
// per-connection 推送（见 startCaseStudyTimer），不再有独立 buildSnapshot 方法。

} // namespace aegisgate
