#pragma once
#include "auth/auth_models.h"

#include <optional>
#include <string>
#include <vector>

namespace aegisgate {

class AuthService;

namespace admin {

// TASK-20260603-02 P0-2：HTTP + WS 共用的 admin 会话解析。
// 链路与 AdminHttpController::authenticateRequest 完全一致：
//   IP allowlist → SSO session（含 MFA 闸门）→ JWT fallback。
// 返回 nullopt 表示拒绝（IP 不允许 / 无 cookie / MFA 未验证 / token 无效）。
std::optional<AuthContext> resolveAdminSession(
    const std::string& session_cookie,
    const std::string& client_ip,
    const std::vector<std::string>& allowed_ips,
    AuthService* auth_svc,
    const std::string& jwt_secret);

// TASK-20260604-01 P0-F / SR-2：预MFA态会话解析，仅供 MFA 挑战端点
// （mfaVerify / mfaRecovery）使用，打破"未验证 MFA 的 SSO session 无法调
// mfaVerify"的循环依赖。
// 语义：IP allowlist 通过 → SSO session 存在 → isMfaRequired && !mfa_verified
//       → 返回该 ctx（pending）；否则（已验证 / 不需 MFA / 无 session / IP 不允许）
//       返回 nullopt（交由正常 resolveAdminSession 处理或拒绝）。
// SR-2：解析出的 ctx 只能用于 verify/recovery；其它端点仍走 resolveAdminSession。
std::optional<AuthContext> resolvePendingMfaSession(
    const std::string& session_cookie,
    const std::string& client_ip,
    const std::vector<std::string>& allowed_ips,
    AuthService* auth_svc);

// TASK-20260703-02 Epic 1 / C5：MFA 挑战端点（mfaSetup / mfaVerify / mfaRecovery）
// 统一认证语义 = resolveAdminSession 优先，被 MFA 闸门拒绝时回退 resolvePendingMfaSession。
// 根因：mfaSetup 此前只走 resolveAdminSession → enforcement=required 时首次绑定的
// pending session（已过一次因子但 MFA 未验证）拿不到 secret → 死锁。抽为纯组合函数
// 供三端点共用，杜绝"某端点漏接 pending 回退"（DRY 根治）。
std::optional<AuthContext> resolveMfaChallengeSession(
    const std::string& session_cookie,
    const std::string& client_ip,
    const std::vector<std::string>& allowed_ips,
    AuthService* auth_svc,
    const std::string& jwt_secret);

// P0-3 + 复用：IP allowlist 纯谓词（空 allowlist = 全部允许，与现有 login 语义一致）。
// TASK-20260702-02 P2-5（SR-5）：allowed_ips 条目支持 IPv4 CIDR（如 10.0.0.0/8）
// 与精确匹配（IPv6 仅精确匹配）。此前仅精确串相等 → 配置文档里的 CIDR 段永不命中。
bool isAdminIpAllowed(const std::string& ip,
                      const std::vector<std::string>& allowed_ips);

// TASK-20260702-02 P2-5（SR-5）：解析真实客户端 IP（纯函数，不依赖 drogon）。
// 仅当 peer_ip 属于 trusted_proxies（精确或 CIDR）时才采信 xff_header（取最右一个
// 非可信代理的地址）；否则或 xff 为空或无可信代理配置 → 直接返回 peer_ip。
// 反向代理后取 TCP peer 会恒为代理 IP，此函数在受控前提下还原真实客户端。
std::string resolveClientIp(const std::string& peer_ip,
                            const std::string& xff_header,
                            const std::vector<std::string>& trusted_proxies);

// TASK-20260702-02 P2-1（SR-1）：CORS 决策纯函数（不依赖 GatewayRuntime 单例，
// 可单测）。安全铁律：通配 `*` 不得携带 Access-Control-Allow-Credentials（CORS
// 规范禁止 `*`+credentials；此前 applyCorsHeaders 命中 `*` 时回显请求 origin +
// credentials，等于允许任意站点带凭证跨域访问 /admin/*）。语义：具体 origin 精确
// 匹配优先 → 回显 origin + 凭证 + Vary: Origin；仅命中 `*` → 发 Allow-Origin: *
// 且不发凭证（保留无凭证公开场景）；空 origin 或不在名单 → 不放行。
struct CorsDecision {
    bool allowed = false;
    std::string allow_origin;      // Access-Control-Allow-Origin 的值
    bool allow_credentials = false;
    bool vary_origin = false;      // 回显具体 origin 时须 Vary: Origin 防缓存污染
};
CorsDecision decideCors(const std::string& origin,
                        const std::vector<std::string>& allowed);

// P0-1：audit entry 是否应投递给某连接（super 看全部 / 其他仅本租户）。
bool shouldDeliverAuditToConnection(bool is_super,
                                    const std::string& conn_tenant_id,
                                    const std::string& entry_tenant_id);

// P0-1：全局聚合 metrics 是否应投递给某连接（D1=A：仅 super）。
bool shouldDeliverGlobalMetrics(bool is_super);

} // namespace admin
} // namespace aegisgate
