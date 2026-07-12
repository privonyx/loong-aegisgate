// TASK-20260603-02 — admin_session 共享会话解析 + 作用域纯谓词单元测试。
//
// 覆盖三个 P0 安全缺口的可测内核：
//   - SR-1（P0-1）：WS 推送跨租户隔离 → shouldDeliverAuditToConnection /
//     shouldDeliverGlobalMetrics 纯谓词（super 全局 / 非 super 仅本租户）。
//   - SR-2（P0-2）：WS+HTTP 认证统一 → resolveAdminSession（IP → SSO session →
//     MFA 闸门 → JWT fallback），与 AdminHttpController::authenticateRequest 同链。
//   - SR-3（P0-3）：SSO 入口 IP allowlist → isAdminIpAllowed 纯谓词。
//
// 设计：docs/specs/2026-06-03-admin-ws-p0-security-design.md §4-5。

#include "server/admin_session.h"
#include "server/case_study_builder.h"

#include "auth/auth_service.h"
#include "auth/jwt_utils.h"
#include "core/config.h"
#include "core/feature_gate.h"
#include "storage/sqlite_persistent_store.h"

#include <gtest/gtest.h>
#include <memory>

using namespace aegisgate;

namespace {

// ---------------------------------------------------------------------------
// Epic 0.1 — 作用域纯谓词（无 fixture，纯函数）
// ---------------------------------------------------------------------------

TEST(AdminSessionPredicateTest, IpAllowedEmptyAllowlistAllowsAll) {
    EXPECT_TRUE(admin::isAdminIpAllowed("9.9.9.9", {}));
}

TEST(AdminSessionPredicateTest, IpAllowedWhenInList) {
    EXPECT_TRUE(admin::isAdminIpAllowed("1.2.3.4", {"1.2.3.4", "5.6.7.8"}));
}

TEST(AdminSessionPredicateTest, IpDeniedWhenNotInList) {
    EXPECT_FALSE(admin::isAdminIpAllowed("9.9.9.9", {"1.2.3.4", "5.6.7.8"}));
}

// TASK-20260702-02 P2-5（SR-5）：CIDR 匹配 —— 兑现配置文档里的网段承诺。
TEST(AdminSessionPredicateTest, IpAllowedByCidrMatch) {
    EXPECT_TRUE(admin::isAdminIpAllowed("10.1.2.3", {"10.0.0.0/8"}));
    EXPECT_TRUE(admin::isAdminIpAllowed("192.168.5.7", {"192.168.0.0/16"}));
}

TEST(AdminSessionPredicateTest, IpDeniedOutsideCidr) {
    EXPECT_FALSE(admin::isAdminIpAllowed("11.1.2.3", {"10.0.0.0/8"}));
    EXPECT_FALSE(admin::isAdminIpAllowed("192.169.5.7", {"192.168.0.0/16"}));
}

TEST(AdminSessionPredicateTest, CidrAndExactMix) {
    EXPECT_TRUE(admin::isAdminIpAllowed("127.0.0.1", {"127.0.0.1", "10.0.0.0/8"}));
    EXPECT_TRUE(admin::isAdminIpAllowed("10.9.9.9", {"127.0.0.1", "10.0.0.0/8"}));
}

// TASK-20260702-02 P2-5（SR-5）：resolveClientIp —— 仅可信代理采信 XFF。
TEST(AdminSessionClientIpTest, TrustedProxyUsesXffClient) {
    EXPECT_EQ(admin::resolveClientIp("10.0.0.1", "203.0.113.9", {"10.0.0.0/8"}),
              "203.0.113.9");
}

TEST(AdminSessionClientIpTest, UntrustedPeerIgnoresXff) {
    // peer 不在可信代理内 → 不信任 XFF，用 peer（防伪造）。
    EXPECT_EQ(admin::resolveClientIp("8.8.8.8", "203.0.113.9", {"10.0.0.0/8"}),
              "8.8.8.8");
}

TEST(AdminSessionClientIpTest, NoTrustedProxiesNeverTrustsXff) {
    EXPECT_EQ(admin::resolveClientIp("10.0.0.1", "203.0.113.9", {}),
              "10.0.0.1");
}

TEST(AdminSessionClientIpTest, EmptyXffFallsBackToPeer) {
    EXPECT_EQ(admin::resolveClientIp("10.0.0.1", "", {"10.0.0.0/8"}),
              "10.0.0.1");
}

TEST(AdminSessionClientIpTest, ProxyChainReturnsRightmostNonTrusted) {
    // XFF: "real_client, inner_proxy"；inner_proxy 可信 → 取最右非可信 = 真实客户端。
    EXPECT_EQ(admin::resolveClientIp("10.0.0.1", "203.0.113.9, 10.0.0.5",
                                     {"10.0.0.0/8"}),
              "203.0.113.9");
}

TEST(AdminSessionPredicateTest, AuditSuperSeesAllTenants) {
    EXPECT_TRUE(admin::shouldDeliverAuditToConnection(true, "t1", "t2"));
    EXPECT_TRUE(admin::shouldDeliverAuditToConnection(true, "t1", "t1"));
}

TEST(AdminSessionPredicateTest, AuditNonSuperSeesOnlyOwnTenant) {
    EXPECT_TRUE(admin::shouldDeliverAuditToConnection(false, "t1", "t1"));
    EXPECT_FALSE(admin::shouldDeliverAuditToConnection(false, "t1", "t2"));
}

TEST(AdminSessionPredicateTest, GlobalMetricsSuperOnly) {
    EXPECT_TRUE(admin::shouldDeliverGlobalMetrics(true));
    EXPECT_FALSE(admin::shouldDeliverGlobalMetrics(false));
}

// ---------------------------------------------------------------------------
// Epic 0.2 — resolveAdminSession（真实 store + AuthService + session）
// ---------------------------------------------------------------------------

class ResolveAdminSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<SQLitePersistentStore>(":memory:");
        ASSERT_TRUE(store_->initialize());

        gate_ = std::make_unique<FeatureGate>(
            FeatureGate::createUnlocked(Edition::Enterprise));

        // 默认 config（未 load → mfaEnforcement="disabled"）。
        config_no_mfa_ = std::make_unique<Config>();
        auth_no_mfa_ = std::make_unique<AuthService>(
            store_.get(), config_no_mfa_.get(), gate_.get());

        // 强制 MFA 的 config（load enforcement=required）。
        config_mfa_ = std::make_unique<Config>();
        ASSERT_TRUE(config_mfa_->loadFromString("mfa:\n  enforcement: required\n"));
        auth_mfa_ = std::make_unique<AuthService>(
            store_.get(), config_mfa_.get(), gate_.get());

        Tenant t;
        t.id = "t1"; t.name = "TestTenant"; t.status = "active";
        t.created_at = "2026-01-01T00:00:00Z"; t.updated_at = t.created_at;
        store_->insertTenant(t);

        User u;
        u.id = "u1"; u.tenant_id = "t1"; u.username = "alice";
        u.display_name = "Alice"; u.role = Role::TenantAdmin;
        u.status = "active"; u.created_at = "2026-01-01T00:00:00Z";
        u.updated_at = u.created_at;
        store_->insertUser(u);
    }

    std::string makeSession() {
        auto s = auth_no_mfa_->sessionManager().createSession(
            "u1", "t1", "sso", "1.2.3.4", "TestBrowser");
        EXPECT_TRUE(s.has_value());
        return s ? s->id : std::string{};
    }

    std::unique_ptr<SQLitePersistentStore> store_;
    std::unique_ptr<FeatureGate> gate_;
    std::unique_ptr<Config> config_no_mfa_;
    std::unique_ptr<Config> config_mfa_;
    std::unique_ptr<AuthService> auth_no_mfa_;
    std::unique_ptr<AuthService> auth_mfa_;
};

TEST_F(ResolveAdminSessionTest, IpNotInAllowlistRejected) {
    auto session_id = makeSession();
    auto ctx = admin::resolveAdminSession(
        session_id, "9.9.9.9", {"1.2.3.4"}, auth_no_mfa_.get(), "secret");
    EXPECT_FALSE(ctx.has_value());
}

TEST_F(ResolveAdminSessionTest, EmptyCookieRejected) {
    auto ctx = admin::resolveAdminSession(
        "", "1.2.3.4", {}, auth_no_mfa_.get(), "secret");
    EXPECT_FALSE(ctx.has_value());
}

TEST_F(ResolveAdminSessionTest, ValidSsoSessionResolves) {
    auto session_id = makeSession();
    auto ctx = admin::resolveAdminSession(
        session_id, "1.2.3.4", {}, auth_no_mfa_.get(), "secret");
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(ctx->user_id, "u1");
    EXPECT_EQ(ctx->tenant_id, "t1");
    EXPECT_EQ(ctx->role, Role::TenantAdmin);
    EXPECT_EQ(ctx->session_id, session_id);
}

TEST_F(ResolveAdminSessionTest, MfaRequiredButUnverifiedRejected) {
    auto session_id = makeSession();  // mfa_verified=false by default
    auto ctx = admin::resolveAdminSession(
        session_id, "1.2.3.4", {}, auth_mfa_.get(), "secret");
    EXPECT_FALSE(ctx.has_value());
}

TEST_F(ResolveAdminSessionTest, ValidJwtFallbackResolves) {
    // 无对应 session → resolveSession 失败 → 落 JWT fallback。
    JwtPayload p;
    p.user_id = "u-jwt"; p.tenant_id = "t-jwt"; p.role = "developer";
    auto token = JwtUtils::sign(p, "jwt-secret", 3600);

    auto ctx = admin::resolveAdminSession(
        token, "1.2.3.4", {}, auth_no_mfa_.get(), "jwt-secret");
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(ctx->user_id, "u-jwt");
    EXPECT_EQ(ctx->tenant_id, "t-jwt");
    EXPECT_EQ(ctx->role, Role::Developer);
}

// TASK-20260702-01 P0-2/SR-2：MFA 强制时 JWT/api_key→JWT fallback 不得绕过
// MFA 闸门。JWT 通道无法携带 MFA 验证态，enforcement=required（或 role_based
// 下的 TenantAdmin+）必须拒绝落入 fallback 的会话。
TEST_F(ResolveAdminSessionTest, JwtFallbackRejectedWhenMfaRequired) {
    JwtPayload p;
    p.user_id = "u-jwt"; p.tenant_id = "t-jwt"; p.role = "developer";
    auto token = JwtUtils::sign(p, "jwt-secret", 3600);
    auto ctx = admin::resolveAdminSession(
        token, "1.2.3.4", {}, auth_mfa_.get(), "jwt-secret");
    EXPECT_FALSE(ctx.has_value()) << "SR-2: MFA 强制时 JWT fallback 不得授予会话";
}

TEST_F(ResolveAdminSessionTest, BadCookieNeitherSessionNorJwtRejected) {
    auto ctx = admin::resolveAdminSession(
        "not-a-session-or-jwt", "1.2.3.4", {}, auth_no_mfa_.get(), "jwt-secret");
    EXPECT_FALSE(ctx.has_value());
}

TEST_F(ResolveAdminSessionTest, EmptyJwtSecretNoFallback) {
    JwtPayload p;
    p.user_id = "u-jwt"; p.tenant_id = "t-jwt"; p.role = "developer";
    auto token = JwtUtils::sign(p, "jwt-secret", 3600);
    auto ctx = admin::resolveAdminSession(
        token, "1.2.3.4", {}, auth_no_mfa_.get(), "");
    EXPECT_FALSE(ctx.has_value());
}

// ---------------------------------------------------------------------------
// TASK-20260604-01 Epic E — resolvePendingMfaSession（P0-F / SR-2）
//
// 预MFA态：仅放行「需要 MFA 但未验证」的 SSO session（供 verify/recovery 端点），
// 拒绝其它一切。Mutation 验证：删 !mfa_verified 条件 → MfaAlreadyVerified FAIL。
// ---------------------------------------------------------------------------

TEST_F(ResolveAdminSessionTest, PendingMfaResolvesWhenMfaRequiredUnverified) {
    auto session_id = makeSession();  // mfa_verified=false
    auto ctx = admin::resolvePendingMfaSession(
        session_id, "1.2.3.4", {}, auth_mfa_.get());
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(ctx->user_id, "u1");
    EXPECT_EQ(ctx->tenant_id, "t1");
}

TEST_F(ResolveAdminSessionTest, PendingMfaNulloptWhenMfaNotRequired) {
    auto session_id = makeSession();
    // 不需要 MFA → 应让正常 resolveAdminSession 处理 → 此处 nullopt。
    auto ctx = admin::resolvePendingMfaSession(
        session_id, "1.2.3.4", {}, auth_no_mfa_.get());
    EXPECT_FALSE(ctx.has_value());
}

TEST_F(ResolveAdminSessionTest, PendingMfaNulloptWhenAlreadyVerified) {
    auto session_id = makeSession();
    ASSERT_TRUE(auth_mfa_->sessionManager().setMfaVerified(session_id, true));
    auto ctx = admin::resolvePendingMfaSession(
        session_id, "1.2.3.4", {}, auth_mfa_.get());
    EXPECT_FALSE(ctx.has_value());  // 已验证 → 不再 pending
}

TEST_F(ResolveAdminSessionTest, PendingMfaNulloptOnEmptyCookie) {
    auto ctx = admin::resolvePendingMfaSession("", "1.2.3.4", {}, auth_mfa_.get());
    EXPECT_FALSE(ctx.has_value());
}

TEST_F(ResolveAdminSessionTest, PendingMfaNulloptWhenIpNotAllowed) {
    auto session_id = makeSession();
    auto ctx = admin::resolvePendingMfaSession(
        session_id, "9.9.9.9", {"1.2.3.4"}, auth_mfa_.get());
    EXPECT_FALSE(ctx.has_value());
}

TEST_F(ResolveAdminSessionTest, PendingMfaNulloptOnBadSession) {
    auto ctx = admin::resolvePendingMfaSession(
        "not-a-session", "1.2.3.4", {}, auth_mfa_.get());
    EXPECT_FALSE(ctx.has_value());
}

// ---------------------------------------------------------------------------
// TASK-20260703-02 Epic 1 — C5 MFA 首次绑定死锁修复
//
// resolveMfaChallengeSession = resolveAdminSession ?: resolvePendingMfaSession，
// 统一 MFA 挑战端点（setup/verify/recovery）认证语义：常规会话优先，被 MFA 闸门
// 拒绝时回退预 MFA 态。根因：mfaSetup 此前只走 resolveAdminSession → enforcement=
// required 首次绑定的 pending session 拿不到 secret → 死锁。
// Mutation：删 pending 回退 → MfaChallengeAllowsPending FAIL。
// ---------------------------------------------------------------------------

TEST_F(ResolveAdminSessionTest, MfaChallengeAllowsPendingWhenMfaRequiredUnverified) {
    auto session_id = makeSession();  // mfa_verified=false
    // 常规解析被 MFA 闸门拒绝（首次绑定死锁根因）。
    auto regular = admin::resolveAdminSession(
        session_id, "1.2.3.4", {}, auth_mfa_.get(), "jwt-secret");
    ASSERT_FALSE(regular.has_value());
    // MFA 挑战端点语义：回退预 MFA 态 → 放行（供 setup/verify/recovery）。
    auto ctx = admin::resolveMfaChallengeSession(
        session_id, "1.2.3.4", {}, auth_mfa_.get(), "jwt-secret");
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(ctx->user_id, "u1");
}

TEST_F(ResolveAdminSessionTest, MfaChallengeRejectsEmptyCookie) {
    auto ctx = admin::resolveMfaChallengeSession(
        "", "1.2.3.4", {}, auth_mfa_.get(), "jwt-secret");
    EXPECT_FALSE(ctx.has_value());
}

TEST_F(ResolveAdminSessionTest, MfaChallengeUsesRegularWhenVerified) {
    auto session_id = makeSession();
    ASSERT_TRUE(auth_mfa_->sessionManager().setMfaVerified(session_id, true));
    // 已验证 → 常规解析通过，直接返回（不依赖 pending）。
    auto ctx = admin::resolveMfaChallengeSession(
        session_id, "1.2.3.4", {}, auth_mfa_.get(), "jwt-secret");
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(ctx->user_id, "u1");
}

// ---------------------------------------------------------------------------
// Epic 4.1 — 跨通道 RBAC 一致性（SR-NEW1 第 2 次实战印证）
//
// 锚定不变量：WS 三条推送通道（audit / metrics / case_study）的作用域决策与
// HTTP caseStudyHeadline 共用的 buildCaseStudySnapshot scope 同向。即同一
// (is_super, tenant) 决策下，super = 全局视图、非 super = 仅本租户，三通道一致。
// 首次印证：TASK-20260602-01 case_study per-connection 推送。
// ---------------------------------------------------------------------------

TEST(AdminSessionCrossChannelTest, SuperSeesGlobalAcrossAllChannels) {
    const bool is_super = true;

    // case_study（HTTP + WS 共用 builder）：super → scope=global
    admin::CaseStudyInputs in;
    in.include_envelope = true;
    in.is_super = is_super;
    auto cs = admin::buildCaseStudySnapshot(in);
    EXPECT_EQ(cs["scope"].get<std::string>(), "global");

    // audit（WS）：super 看跨租户 entry
    EXPECT_TRUE(admin::shouldDeliverAuditToConnection(is_super, "t1", "t2"));
    // metrics（WS）：super 收全局聚合
    EXPECT_TRUE(admin::shouldDeliverGlobalMetrics(is_super));
}

TEST(AdminSessionCrossChannelTest, NonSuperRestrictedToOwnTenantAcrossAllChannels) {
    const bool is_super = false;

    // case_study：非 super → scope=tenant + 自身 tenant_id
    admin::CaseStudyInputs in;
    in.include_envelope = true;
    in.is_super = is_super;
    in.tenant_id = "t1";
    auto cs = admin::buildCaseStudySnapshot(in);
    EXPECT_EQ(cs["scope"].get<std::string>(), "tenant");
    EXPECT_EQ(cs["tenant_id"].get<std::string>(), "t1");

    // audit：仅本租户，无跨租户泄漏
    EXPECT_TRUE(admin::shouldDeliverAuditToConnection(is_super, "t1", "t1"));
    EXPECT_FALSE(admin::shouldDeliverAuditToConnection(is_super, "t1", "t2"));
    // metrics：非 super 不收全局聚合
    EXPECT_FALSE(admin::shouldDeliverGlobalMetrics(is_super));
}

} // namespace
