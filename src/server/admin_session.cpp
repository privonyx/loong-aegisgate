#include "server/admin_session.h"

#include "auth/auth_service.h"
#include "auth/jwt_utils.h"

#include <arpa/inet.h>
#include <cstdint>
#include <sstream>

namespace aegisgate {
namespace admin {

namespace {

// 解析 IPv4 点分十进制为主机序 uint32；非合法 IPv4 返回 false。
bool parseIpv4(const std::string& s, uint32_t& out) {
    struct in_addr addr;
    if (inet_pton(AF_INET, s.c_str(), &addr) != 1) return false;
    out = ntohl(addr.s_addr);
    return true;
}

// IPv4 CIDR 匹配（"net/prefix"）。非 CIDR 或非法返回 false。
bool cidrMatchV4(const std::string& cidr, const std::string& ip) {
    auto slash = cidr.find('/');
    if (slash == std::string::npos) return false;
    int prefix = 0;
    try {
        prefix = std::stoi(cidr.substr(slash + 1));
    } catch (...) {
        return false;
    }
    if (prefix < 0 || prefix > 32) return false;
    uint32_t net_i = 0, ip_i = 0;
    if (!parseIpv4(cidr.substr(0, slash), net_i)) return false;
    if (!parseIpv4(ip, ip_i)) return false;
    if (prefix == 0) return true;
    uint32_t mask = (prefix == 32) ? 0xFFFFFFFFu
                                   : ~((1u << (32 - prefix)) - 1);
    return (net_i & mask) == (ip_i & mask);
}

// ip 是否命中某条目（精确 或 IPv4 CIDR）。
bool ipMatchesEntry(const std::string& entry, const std::string& ip) {
    if (entry == ip) return true;
    if (entry.find('/') != std::string::npos) return cidrMatchV4(entry, ip);
    return false;
}

bool ipInList(const std::string& ip, const std::vector<std::string>& list) {
    for (const auto& e : list) {
        if (ipMatchesEntry(e, ip)) return true;
    }
    return false;
}

}  // namespace

bool isAdminIpAllowed(const std::string& ip,
                      const std::vector<std::string>& allowed_ips) {
    if (allowed_ips.empty()) return true;
    return ipInList(ip, allowed_ips);
}

std::string resolveClientIp(const std::string& peer_ip,
                            const std::string& xff_header,
                            const std::vector<std::string>& trusted_proxies) {
    // SR-5：仅当 peer 属于可信代理才采信 XFF；否则/无配置 → 用 peer（防伪造）。
    if (trusted_proxies.empty() || xff_header.empty()) return peer_ip;
    if (!ipInList(peer_ip, trusted_proxies)) return peer_ip;

    // 逗号分隔，逐段 trim，从右向左取第一个非可信代理地址 = 真实客户端。
    std::vector<std::string> parts;
    std::stringstream ss(xff_header);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t b = item.find_first_not_of(" \t");
        size_t e = item.find_last_not_of(" \t");
        if (b == std::string::npos) continue;
        parts.push_back(item.substr(b, e - b + 1));
    }
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!ipInList(*it, trusted_proxies)) return *it;
    }
    return parts.empty() ? peer_ip : parts.front();
}

CorsDecision decideCors(const std::string& origin,
                        const std::vector<std::string>& allowed) {
    CorsDecision d;
    if (origin.empty()) return d;
    bool has_wildcard = false;
    for (const auto& o : allowed) {
        if (o == "*") {
            has_wildcard = true;
            continue;
        }
        if (o == origin) {
            // 具体 origin 精确匹配：回显 origin + 凭证 + Vary。
            d.allowed = true;
            d.allow_origin = origin;
            d.allow_credentials = true;
            d.vary_origin = true;
            return d;
        }
    }
    if (has_wildcard) {
        // SR-1：通配放行必须发 `Allow-Origin: *` 且**不**发凭证。
        d.allowed = true;
        d.allow_origin = "*";
        d.allow_credentials = false;
        d.vary_origin = false;
    }
    return d;
}

bool shouldDeliverAuditToConnection(bool is_super,
                                    const std::string& conn_tenant_id,
                                    const std::string& entry_tenant_id) {
    return is_super || conn_tenant_id == entry_tenant_id;
}

bool shouldDeliverGlobalMetrics(bool is_super) {
    return is_super;
}

std::optional<AuthContext> resolveAdminSession(
    const std::string& session_cookie,
    const std::string& client_ip,
    const std::vector<std::string>& allowed_ips,
    AuthService* auth_svc,
    const std::string& jwt_secret) {

    if (!isAdminIpAllowed(client_ip, allowed_ips)) return std::nullopt;
    if (session_cookie.empty()) return std::nullopt;

    // SSO session 优先（含 MFA 闸门）。
    if (auth_svc) {
        auto session_ctx = auth_svc->resolveSession(session_cookie);
        if (session_ctx) {
            if (auth_svc->isMfaRequired(*session_ctx) && !session_ctx->mfa_verified) {
                return std::nullopt;
            }
            return session_ctx;
        }
    }

    // JWT token fallback。
    if (jwt_secret.empty()) return std::nullopt;
    auto payload = JwtUtils::verify(session_cookie, jwt_secret);
    if (!payload) return std::nullopt;

    auto role = roleFromString(payload->role);
    AuthContext ctx;
    ctx.user_id = payload->user_id;
    ctx.tenant_id = payload->tenant_id;
    ctx.role = role.value_or(Role::Viewer);
    ctx.is_rbac_enabled = auth_svc ? auth_svc->isRbacEnabled() : false;
    // SR-2（TASK-20260702-01）：JWT / api_key→JWT 通道无法携带 MFA 验证态
    // （mfa_verified 恒 false）。当该主体按策略需要 MFA 时，fallback 会话必须
    // 拒绝，否则等于绕过 SSO session 上的 MFA 闸门。HTTP 与 WS 共用此咽喉点。
    if (auth_svc && auth_svc->isMfaRequired(ctx)) return std::nullopt;
    return ctx;
}

std::optional<AuthContext> resolvePendingMfaSession(
    const std::string& session_cookie,
    const std::string& client_ip,
    const std::vector<std::string>& allowed_ips,
    AuthService* auth_svc) {

    if (!isAdminIpAllowed(client_ip, allowed_ips)) return std::nullopt;
    if (session_cookie.empty()) return std::nullopt;
    if (!auth_svc) return std::nullopt;

    auto session_ctx = auth_svc->resolveSession(session_cookie);
    if (!session_ctx) return std::nullopt;

    // 仅放行"需要 MFA 但尚未验证"的 session（预MFA态）。MFA 已验证或不需 MFA
    // → nullopt，交由正常 resolveAdminSession 处理（SR-2：闸门不被绕过）。
    if (auth_svc->isMfaRequired(*session_ctx) && !session_ctx->mfa_verified) {
        return session_ctx;
    }
    return std::nullopt;
}

// TASK-20260703-02 Epic 1 / C5：MFA 挑战端点统一认证 = 常规会话优先，被 MFA 闸门
// 拒绝时回退预 MFA 态。三端点（setup/verify/recovery）共用，杜绝漏接 pending 回退。
std::optional<AuthContext> resolveMfaChallengeSession(
    const std::string& session_cookie,
    const std::string& client_ip,
    const std::vector<std::string>& allowed_ips,
    AuthService* auth_svc,
    const std::string& jwt_secret) {

    auto ctx = resolveAdminSession(session_cookie, client_ip, allowed_ips,
                                   auth_svc, jwt_secret);
    if (!ctx) {
        ctx = resolvePendingMfaSession(session_cookie, client_ip, allowed_ips,
                                       auth_svc);
    }
    return ctx;
}

} // namespace admin
} // namespace aegisgate
