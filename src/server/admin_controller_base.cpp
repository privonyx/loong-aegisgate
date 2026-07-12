#include "server/admin_controller_base.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>

namespace aegisgate {

void AdminControllerBase::auditAction(const AuthContext& ctx,
                                      const std::string& action,
                                      const std::string& detail) {
    if (!audit_) return;

    std::string enriched_detail = detail;
    if (ctx.role == Role::SuperAdmin) {
        enriched_detail = "[SuperAdmin user=" + ctx.user_id + "] " + detail;
    }

    audit_->logAction("admin", ctx.tenant_id, "AdminController", action,
                      enriched_detail);
}

void AdminControllerBase::auditCrossTenantAction(const AuthContext& ctx,
                                                 const std::string& target_tenant_id,
                                                 const std::string& action,
                                                 const std::string& detail) {
    if (!audit_) return;

    std::string enriched = "[CrossTenant user=" + ctx.user_id
        + " from=" + ctx.tenant_id
        + " target=" + target_tenant_id + "] " + detail;

    audit_->logAction("admin", ctx.tenant_id, "AdminController",
                      "cross_tenant:" + action, enriched);

    if (ctx.tenant_id != target_tenant_id) {
        audit_->logAction("admin", target_tenant_id, "AdminController",
                          "cross_tenant:" + action, enriched);
    }
}

std::string AdminControllerBase::nowTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm buf{};
    gmtime_r(&tt, &buf);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &buf);
    return ts;
}

std::string AdminControllerBase::generateId() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << dist(rng) << dist(rng);
    return oss.str();
}

std::string AdminControllerBase::effectiveTenantId(
    const AuthContext& ctx, const std::string& requested) const {
    if (ctx.role == Role::SuperAdmin) return requested;
    return ctx.tenant_id;
}

} // namespace aegisgate
