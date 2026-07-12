#include "server/admin_controller.h"
#include "auth/auth_service.h"
#include "auth/totp_service.h"
#include "auth/encryption.h"
#include "auth/scim_service.h"
#include "auth/identity_mapper.h"
#include "guardrail/audit.h"
#include "storage/sqlite_persistent_store.h"
#include "core/crypto.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace aegisgate;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Fixture: Phase3IntegrationTest
// ---------------------------------------------------------------------------

class Phase3IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<SQLitePersistentStore>(":memory:");
        ASSERT_TRUE(store_->initialize());

        gate_ = std::make_unique<FeatureGate>(
            FeatureGate::createUnlocked(Edition::Enterprise));
        auth_svc_ = std::make_unique<AuthService>(
            store_.get(), nullptr, gate_.get());
        audit_ = std::make_unique<AuditLogger>();
        ctrl_ = std::make_unique<AdminController>(
            store_.get(), auth_svc_.get(), audit_.get());
        scim_ = std::make_unique<ScimService>(store_.get());

        Tenant t;
        t.id = "t1"; t.name = "Test Tenant"; t.status = "active";
        t.created_at = "2026-01-01T00:00:00Z"; t.updated_at = t.created_at;
        store_->insertTenant(t);

        User u;
        u.id = "u1"; u.tenant_id = "t1"; u.username = "admin";
        u.display_name = "Admin User"; u.role = Role::SuperAdmin;
        u.status = "active"; u.created_at = "2026-01-01T00:00:00Z";
        u.updated_at = u.created_at;
        store_->insertUser(u);

        super_ctx_.tenant_id = "t1";
        super_ctx_.user_id = "u1";
        super_ctx_.role = Role::SuperAdmin;
        super_ctx_.is_rbac_enabled = true;
    }

    void TearDown() override {
        if (audit_) audit_->shutdown();
    }

    std::optional<Session> createTestSession(const std::string& user_id,
                                              const std::string& tenant_id) {
        return auth_svc_->sessionManager().createSession(
            user_id, tenant_id, "sso", "127.0.0.1", "TestAgent");
    }

    void enableMfaForUser(const std::string& user_id) {
        auto mfa = store_->getMfaSecret(user_id);
        ASSERT_TRUE(mfa.has_value());
        mfa->enabled = true;
        store_->upsertMfaSecret(*mfa);
    }

    void insertScimToken(const std::string& tenant_id,
                         const std::string& raw_token,
                         const std::string& expires_at = "") {
        ScimToken st;
        st.id = "scim-tok-1";
        st.tenant_id = tenant_id;
        st.token_hash = crypto::sha256(raw_token);
        st.description = "test token";
        st.created_at = "2026-01-01T00:00:00Z";
        st.expires_at = expires_at;
        store_->insertScimToken(st);
    }

    std::unique_ptr<SQLitePersistentStore> store_;
    std::unique_ptr<FeatureGate> gate_;
    std::unique_ptr<AuthService> auth_svc_;
    std::unique_ptr<AuditLogger> audit_;
    std::unique_ptr<AdminController> ctrl_;
    std::unique_ptr<ScimService> scim_;
    AuthContext super_ctx_;
};

class Phase3SecurityTest : public Phase3IntegrationTest {};

// ===========================================================================
// Phase3IntegrationTest
// ===========================================================================

TEST_F(Phase3IntegrationTest, ScimPushThenSsoLogin) {
    const std::string raw_token = "scim-secret-token-abc";
    insertScimToken("t1", raw_token);

    auto tenant_id = scim_->authenticateToken(raw_token);
    ASSERT_TRUE(tenant_id.has_value());
    EXPECT_EQ(*tenant_id, "t1");

    json scim_user = {
        {"userName", "jdoe"},
        {"displayName", "Jane Doe"},
        {"emails", {{{"value", "jdoe@example.com"}}}}
    };
    auto created = scim_->createUser("t1", scim_user);
    EXPECT_FALSE(created.contains("detail"));
    EXPECT_EQ(created["userName"], "jdoe");

    auto user_id = created["id"].get<std::string>();
    auto stored = store_->getUser(user_id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->username, "jdoe");
    EXPECT_EQ(stored->tenant_id, "t1");

    auto mappings = store_->listIdentityMappings("t1", 100, 0);
    bool found_mapping = false;
    for (const auto& m : mappings) {
        if (m.user_id == user_id && m.external_issuer == "scim") {
            found_mapping = true;
            break;
        }
    }
    EXPECT_TRUE(found_mapping);
}

TEST_F(Phase3IntegrationTest, MfaSetupAndVerifyFlow) {
    auto setup = ctrl_->setupMfa(super_ctx_);
    ASSERT_FALSE(setup.is_error);
    ASSERT_TRUE(setup.body.contains("recovery_codes"));
    auto codes = setup.body["recovery_codes"];
    ASSERT_GT(codes.size(), 0u);
    auto first_code = codes[0].get<std::string>();

    enableMfaForUser("u1");

    auto bad = ctrl_->verifyMfa(super_ctx_, {{"code", "000000"}});
    EXPECT_TRUE(bad.is_error);
    EXPECT_EQ(bad.error_code, ErrorCode::MfaInvalidCode);

    auto recovery = ctrl_->recoveryMfa(super_ctx_, {{"code", first_code}});
    EXPECT_FALSE(recovery.is_error);
    EXPECT_TRUE(recovery.body["verified"].get<bool>());
}

TEST_F(Phase3IntegrationTest, SessionLifecycle) {
    auto session = createTestSession("u1", "t1");
    ASSERT_TRUE(session.has_value());

    auto ctx = auth_svc_->resolveSession(session->id);
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(ctx->user_id, "u1");
    EXPECT_EQ(ctx->tenant_id, "t1");

    auth_svc_->sessionManager().deleteSession(session->id);

    auto gone = auth_svc_->resolveSession(session->id);
    EXPECT_FALSE(gone.has_value());
}

TEST_F(Phase3IntegrationTest, ScimUserCrudFlow) {
    const std::string raw_token = "scim-crud-token";
    insertScimToken("t1", raw_token);

    auto tid = scim_->authenticateToken(raw_token);
    ASSERT_TRUE(tid.has_value());

    json create_body = {
        {"userName", "cruduser"},
        {"displayName", "CRUD User"},
        {"emails", {{{"value", "crud@example.com"}}}}
    };
    auto created = scim_->createUser("t1", create_body);
    ASSERT_FALSE(created.contains("detail"));
    auto uid = created["id"].get<std::string>();
    EXPECT_EQ(created["userName"], "cruduser");
    EXPECT_TRUE(created["active"].get<bool>());
    EXPECT_TRUE(created.contains("meta"));

    auto fetched = scim_->getUser("t1", uid);
    EXPECT_EQ(fetched["id"], uid);
    EXPECT_EQ(fetched["userName"], "cruduser");

    auto updated = scim_->updateUser("t1", uid, {{"displayName", "Updated Name"}});
    EXPECT_EQ(updated["displayName"], "Updated Name");
    EXPECT_EQ(updated["id"], uid);

    auto deleted = scim_->deleteUser("t1", uid);
    EXPECT_TRUE(deleted.is_object());
    EXPECT_TRUE(deleted.empty());

    auto after = scim_->getUser("t1", uid);
    EXPECT_FALSE(after["active"].get<bool>());
}

TEST_F(Phase3IntegrationTest, MfaRecoveryCodeFlow) {
    auto setup = ctrl_->setupMfa(super_ctx_);
    ASSERT_FALSE(setup.is_error);
    auto codes = setup.body["recovery_codes"];
    ASSERT_GE(codes.size(), 3u);

    enableMfaForUser("u1");

    auto first_code = codes[0].get<std::string>();
    auto r1 = ctrl_->recoveryMfa(super_ctx_, {{"code", first_code}});
    EXPECT_FALSE(r1.is_error);
    EXPECT_TRUE(r1.body["verified"].get<bool>());
    int remaining = r1.body["remaining_codes"].get<int>();
    EXPECT_EQ(remaining, static_cast<int>(codes.size()) - 1);

    auto r2 = ctrl_->recoveryMfa(super_ctx_, {{"code", first_code}});
    EXPECT_TRUE(r2.is_error);
    EXPECT_EQ(r2.error_code, ErrorCode::MfaInvalidCode);

    auto second_code = codes[1].get<std::string>();
    auto r3 = ctrl_->recoveryMfa(super_ctx_, {{"code", second_code}});
    EXPECT_FALSE(r3.is_error);
    EXPECT_EQ(r3.body["remaining_codes"].get<int>(), remaining - 1);
}

// ===========================================================================
// Phase3SecurityTest
// ===========================================================================

TEST_F(Phase3SecurityTest, ScimTokenInvalid) {
    auto result = scim_->authenticateToken("totally-random-invalid-token");
    EXPECT_FALSE(result.has_value());
}

TEST_F(Phase3SecurityTest, ScimTokenExpired) {
    insertScimToken("t1", "expired-token", "2020-01-01T00:00:00Z");

    auto result = scim_->authenticateToken("expired-token");
    EXPECT_FALSE(result.has_value());
}

TEST_F(Phase3SecurityTest, DeveloperCannotManageSsoProviders) {
    AuthContext dev_ctx;
    dev_ctx.tenant_id = "t1";
    dev_ctx.user_id = "u1";
    dev_ctx.role = Role::Developer;
    dev_ctx.is_rbac_enabled = true;

    json body = {
        {"tenant_id", "t1"},
        {"name", "Attacker IdP"},
        {"issuer_url", "https://evil.example.com"},
        {"client_id", "evil-client"}
    };

    auto cr = ctrl_->createSsoProvider(dev_ctx, body);
    EXPECT_TRUE(cr.is_error);
    EXPECT_EQ(cr.error_code, ErrorCode::InsufficientPermissions);

    auto dr = ctrl_->deleteSsoProvider(dev_ctx, "nonexistent");
    EXPECT_TRUE(dr.is_error);
    EXPECT_EQ(dr.error_code, ErrorCode::InsufficientPermissions);
}

TEST_F(Phase3SecurityTest, ViewerCannotDisableMfa) {
    User viewer;
    viewer.id = "u-viewer"; viewer.tenant_id = "t1"; viewer.username = "viewer1";
    viewer.display_name = "Viewer"; viewer.role = Role::Viewer;
    viewer.status = "active"; viewer.created_at = "2026-01-01T00:00:00Z";
    viewer.updated_at = viewer.created_at;
    store_->insertUser(viewer);

    AuthContext viewer_ctx;
    viewer_ctx.tenant_id = "t1";
    viewer_ctx.user_id = "u-viewer";
    viewer_ctx.role = Role::Viewer;
    viewer_ctx.is_rbac_enabled = true;

    auto r = ctrl_->disableMfa(viewer_ctx, {{"code", "123456"}});
    EXPECT_TRUE(r.is_error);
    EXPECT_EQ(r.error_code, ErrorCode::InsufficientPermissions);
}

TEST_F(Phase3SecurityTest, MfaRequiredSessionCannotAccessProtected) {
    auto session = createTestSession("u1", "t1");
    ASSERT_TRUE(session.has_value());
    EXPECT_FALSE(session->mfa_verified);

    auto ctx = auth_svc_->resolveSession(session->id);
    ASSERT_TRUE(ctx.has_value());
    EXPECT_FALSE(ctx->mfa_verified);
}

TEST_F(Phase3SecurityTest, SessionExpiredDenied) {
    auto session = createTestSession("u1", "t1");
    ASSERT_TRUE(session.has_value());

    Session expired = *session;
    expired.expires_at = "2020-01-01T00:00:00Z";
    expired.last_active_at = "2020-01-01T00:00:00Z";
    store_->deleteSession(session->id);
    store_->insertSession(expired);

    auto ctx = auth_svc_->resolveSession(session->id);
    EXPECT_FALSE(ctx.has_value());
}

TEST_F(Phase3SecurityTest, InactiveUserDenied) {
    User inactive;
    inactive.id = "u-inactive"; inactive.tenant_id = "t1";
    inactive.username = "ghost"; inactive.display_name = "Ghost";
    inactive.role = Role::Developer; inactive.status = "active";
    inactive.created_at = "2026-01-01T00:00:00Z";
    inactive.updated_at = inactive.created_at;
    store_->insertUser(inactive);

    auto session = createTestSession("u-inactive", "t1");
    ASSERT_TRUE(session.has_value());

    inactive.status = "inactive";
    store_->updateUser(inactive);

    auto ctx = auth_svc_->resolveSession(session->id);
    EXPECT_FALSE(ctx.has_value());
}

TEST_F(Phase3SecurityTest, SuspendedTenantDenied) {
    Tenant t2;
    t2.id = "t-susp"; t2.name = "Suspended Co"; t2.status = "active";
    t2.created_at = "2026-01-01T00:00:00Z"; t2.updated_at = t2.created_at;
    store_->insertTenant(t2);

    User u2;
    u2.id = "u-susp"; u2.tenant_id = "t-susp"; u2.username = "susp-user";
    u2.display_name = "Susp User"; u2.role = Role::Developer;
    u2.status = "active"; u2.created_at = "2026-01-01T00:00:00Z";
    u2.updated_at = u2.created_at;
    store_->insertUser(u2);

    auto session = createTestSession("u-susp", "t-susp");
    ASSERT_TRUE(session.has_value());

    t2.status = "suspended";
    store_->updateTenant(t2);

    auto ctx = auth_svc_->resolveSession(session->id);
    EXPECT_FALSE(ctx.has_value());
}

TEST_F(Phase3SecurityTest, MfaInvalidCodeRejected) {
    ctrl_->setupMfa(super_ctx_);
    enableMfaForUser("u1");

    auto r = ctrl_->verifyMfa(super_ctx_, {{"code", "000000"}});
    EXPECT_TRUE(r.is_error);
    EXPECT_EQ(r.error_code, ErrorCode::MfaInvalidCode);
}

TEST_F(Phase3SecurityTest, MfaRecoveryInvalidCodeRejected) {
    ctrl_->setupMfa(super_ctx_);
    enableMfaForUser("u1");

    auto r = ctrl_->recoveryMfa(super_ctx_, {{"code", "XXXXXXXX"}});
    EXPECT_TRUE(r.is_error);
    EXPECT_EQ(r.error_code, ErrorCode::MfaInvalidCode);
}

TEST_F(Phase3SecurityTest, MfaDisableRequiresValidCode) {
    ctrl_->setupMfa(super_ctx_);
    enableMfaForUser("u1");

    auto r = ctrl_->disableMfa(super_ctx_, {{"code", "999999"}});
    EXPECT_TRUE(r.is_error);
    EXPECT_EQ(r.error_code, ErrorCode::MfaInvalidCode);
}
