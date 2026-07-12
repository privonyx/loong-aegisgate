#include "auth/auth_models.h"
#include <aegisgate/error_codes.h>
#include <gtest/gtest.h>

using namespace aegisgate;

TEST(RoleTest, ToStringCoversAll) {
    EXPECT_STREQ(roleToString(Role::Viewer), "viewer");
    EXPECT_STREQ(roleToString(Role::Developer), "developer");
    EXPECT_STREQ(roleToString(Role::TenantAdmin), "tenant_admin");
    EXPECT_STREQ(roleToString(Role::SuperAdmin), "super_admin");
}

TEST(RoleTest, FromStringValid) {
    EXPECT_EQ(roleFromString("viewer"), Role::Viewer);
    EXPECT_EQ(roleFromString("developer"), Role::Developer);
    EXPECT_EQ(roleFromString("tenant_admin"), Role::TenantAdmin);
    EXPECT_EQ(roleFromString("super_admin"), Role::SuperAdmin);
}

TEST(RoleTest, FromStringInvalid) {
    EXPECT_EQ(roleFromString(""), std::nullopt);
    EXPECT_EQ(roleFromString("admin"), std::nullopt);
    EXPECT_EQ(roleFromString("VIEWER"), std::nullopt);
}

TEST(RoleTest, RoundTrip) {
    for (auto r : {Role::Viewer, Role::Developer, Role::TenantAdmin, Role::SuperAdmin}) {
        auto str = roleToString(r);
        auto back = roleFromString(str);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(*back, r);
    }
}

TEST(RoleTest, Ordering) {
    EXPECT_LT(static_cast<int>(Role::Viewer), static_cast<int>(Role::Developer));
    EXPECT_LT(static_cast<int>(Role::Developer), static_cast<int>(Role::TenantAdmin));
    EXPECT_LT(static_cast<int>(Role::TenantAdmin), static_cast<int>(Role::SuperAdmin));
}

TEST(AuthContextTest, Defaults) {
    AuthContext ctx;
    EXPECT_TRUE(ctx.tenant_id.empty());
    EXPECT_TRUE(ctx.user_id.empty());
    EXPECT_TRUE(ctx.api_key_id.empty());
    EXPECT_EQ(ctx.role, Role::Viewer);
    EXPECT_FALSE(ctx.is_rbac_enabled);
}

TEST(TenantTest, Defaults) {
    Tenant t;
    EXPECT_EQ(t.status, "active");
    EXPECT_TRUE(t.model_whitelist.empty());
    EXPECT_DOUBLE_EQ(t.daily_cost_limit, -1.0);
    EXPECT_DOUBLE_EQ(t.monthly_cost_limit, -1.0);
    EXPECT_EQ(t.rate_limit_tokens, -1);
    EXPECT_DOUBLE_EQ(t.rate_limit_refill, -1.0);
}

TEST(UserTest, Defaults) {
    User u;
    EXPECT_EQ(u.role, Role::Viewer);
    EXPECT_EQ(u.status, "active");
}

TEST(ApiKeyRecordTest, Defaults) {
    ApiKeyRecord k;
    EXPECT_EQ(k.role, Role::Developer);
    EXPECT_EQ(k.status, "active");
    EXPECT_TRUE(k.expires_at.empty());
}

// --- SSO Models (TASK-20260324-03) ---

TEST(SsoProviderTest, Defaults) {
    SsoProvider p;
    EXPECT_TRUE(p.id.empty());
    EXPECT_TRUE(p.tenant_id.empty());
    EXPECT_TRUE(p.issuer_url.empty());
    EXPECT_TRUE(p.client_id.empty());
    EXPECT_TRUE(p.client_secret_enc.empty());
    EXPECT_TRUE(p.scopes.empty());
    EXPECT_TRUE(p.claim_mapping_json.empty());
    EXPECT_TRUE(p.group_role_mapping_json.empty());
    EXPECT_TRUE(p.jit_provisioning);
    EXPECT_EQ(p.default_role, "viewer");
    EXPECT_TRUE(p.enabled);
}

TEST(IdentityMappingTest, Defaults) {
    IdentityMapping m;
    EXPECT_TRUE(m.id.empty());
    EXPECT_TRUE(m.tenant_id.empty());
    EXPECT_TRUE(m.external_subject.empty());
    EXPECT_TRUE(m.external_issuer.empty());
    EXPECT_TRUE(m.user_id.empty());
    EXPECT_TRUE(m.email.empty());
}

TEST(SessionTest, Defaults) {
    Session s;
    EXPECT_TRUE(s.id.empty());
    EXPECT_TRUE(s.user_id.empty());
    EXPECT_TRUE(s.tenant_id.empty());
    EXPECT_TRUE(s.auth_method.empty());
    EXPECT_FALSE(s.mfa_verified);
}

TEST(MfaSecretTest, Defaults) {
    MfaSecret m;
    EXPECT_TRUE(m.user_id.empty());
    EXPECT_TRUE(m.secret_enc.empty());
    EXPECT_FALSE(m.enabled);
    EXPECT_TRUE(m.recovery_codes_hash.empty());
}

TEST(ScimTokenTest, Defaults) {
    ScimToken t;
    EXPECT_TRUE(t.id.empty());
    EXPECT_TRUE(t.tenant_id.empty());
    EXPECT_TRUE(t.token_hash.empty());
}

TEST(AuthContextTest, SsoFieldDefaults) {
    AuthContext ctx;
    EXPECT_TRUE(ctx.session_id.empty());
    EXPECT_TRUE(ctx.auth_method.empty());
    EXPECT_FALSE(ctx.mfa_verified);
    EXPECT_TRUE(ctx.external_subject.empty());
}

// --- SSO ErrorCodes ---

TEST(ErrorCodeTest, SsoCodesExist) {
    EXPECT_EQ(toAegisCode(ErrorCode::SsoConfigMissing), "AEGIS-1010");
    EXPECT_EQ(toAegisCode(ErrorCode::SsoCallbackInvalid), "AEGIS-1011");
    EXPECT_EQ(toAegisCode(ErrorCode::SsoTokenInvalid), "AEGIS-1012");
    EXPECT_EQ(toAegisCode(ErrorCode::SsoProviderError), "AEGIS-1013");
    EXPECT_EQ(toAegisCode(ErrorCode::SsoAccountNotMapped), "AEGIS-1014");
}

TEST(ErrorCodeTest, MfaCodesExist) {
    EXPECT_EQ(toAegisCode(ErrorCode::MfaRequired), "AEGIS-1020");
    EXPECT_EQ(toAegisCode(ErrorCode::MfaInvalidCode), "AEGIS-1021");
    EXPECT_EQ(toAegisCode(ErrorCode::MfaNotSetup), "AEGIS-1022");
    EXPECT_EQ(toAegisCode(ErrorCode::MfaRecoveryUsed), "AEGIS-1023");
}

TEST(ErrorCodeTest, SessionCodesExist) {
    EXPECT_EQ(toAegisCode(ErrorCode::SessionExpired), "AEGIS-1030");
    EXPECT_EQ(toAegisCode(ErrorCode::SessionNotFound), "AEGIS-1031");
    EXPECT_EQ(toAegisCode(ErrorCode::SessionLimitReached), "AEGIS-1032");
}

TEST(ErrorCodeTest, ScimCodeExists) {
    EXPECT_EQ(toAegisCode(ErrorCode::ScimTokenInvalid), "AEGIS-1040");
}

TEST(ErrorCodeTest, SsoHttpStatuses) {
    EXPECT_EQ(toHttpStatus(ErrorCode::SsoConfigMissing), 404);
    EXPECT_EQ(toHttpStatus(ErrorCode::SsoCallbackInvalid), 400);
    EXPECT_EQ(toHttpStatus(ErrorCode::SsoTokenInvalid), 401);
    EXPECT_EQ(toHttpStatus(ErrorCode::MfaRequired), 403);
    EXPECT_EQ(toHttpStatus(ErrorCode::MfaInvalidCode), 401);
    EXPECT_EQ(toHttpStatus(ErrorCode::SessionExpired), 401);
    EXPECT_EQ(toHttpStatus(ErrorCode::SessionNotFound), 401);
    EXPECT_EQ(toHttpStatus(ErrorCode::ScimTokenInvalid), 401);
}

TEST(ErrorCodeTest, SsoErrorTypes) {
    EXPECT_STREQ(toErrorType(ErrorCode::SsoConfigMissing), "authentication_error");
    EXPECT_STREQ(toErrorType(ErrorCode::MfaRequired), "authentication_error");
    EXPECT_STREQ(toErrorType(ErrorCode::SessionExpired), "authentication_error");
    EXPECT_STREQ(toErrorType(ErrorCode::ScimTokenInvalid), "authentication_error");
}
