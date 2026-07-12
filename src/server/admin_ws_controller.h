#pragma once
#include <drogon/WebSocketController.h>
#include <nlohmann/json.hpp>
#include <mutex>
#include <set>
#include <atomic>

namespace aegisgate {

class AdminWsController : public drogon::WebSocketController<AdminWsController> {
public:
    WS_PATH_LIST_BEGIN
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif
    WS_PATH_ADD("/admin/ws");
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    WS_PATH_LIST_END

    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                          std::string&& message,
                          const drogon::WebSocketMessageType& type) override;

    void handleNewConnection(const drogon::HttpRequestPtr& req,
                             const drogon::WebSocketConnectionPtr& conn) override;

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override;

    size_t connectionCount() const;

private:
    void startMetricsTimer();
    // TASK-20260527-02 — 30s throttled push of MVP-5 Case Study Numbers.
    // Pace is intentionally slower than metrics (2s) since case-study
    // numbers are aggregate baselines that change less frequently and we
    // do not want to thrash the front-end Row 4 cards.
    //
    // TASK-20260602-01 Epic 2 + D5: push is per-connection（not broadcast）
    // so each connection sees only its own tenant scope（SR-NEW1 WS-RBAC
    // consistency with HTTP /admin/api/case-study/headline endpoint）.
    void startCaseStudyTimer();
    void startAuditSubscription();
    void stopAuditSubscription();
    nlohmann::json buildMetricsSnapshot();

    mutable std::mutex connections_mutex_;
    std::set<drogon::WebSocketConnectionPtr> connections_;

    std::atomic<bool> timer_started_{false};
    std::atomic<size_t> audit_sub_id_{0};
};

} // namespace aegisgate
