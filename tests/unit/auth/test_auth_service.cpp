#include "auth/auth_service.h"
#include "auth/crypto_utils.h"
#include "storage/memory_persistent_store.h"
#include "storage/sqlite_persistent_store.h"
#include <gtest/gtest.h>
#include <fstream>
#include <cstdio>

using namespace aegisgate;

class AuthServiceRbacTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_.initialize();
        gate_ = std::make_unique<FeatureGate>(FeatureGate::createUnlocked(Edition::Enterprise));

        Tenant t; t.id = "t1"; t.name = "Acme"; t.status = "active";
        store_.insertTenant(t);

        User u; u.id = "u1"; u.tenant_id = "t1"; u.username = "alice";
        u.role = Role::Developer; u.status = "active";
        store_.insertUser(u);

        raw_key_ = auth::generateApiKey();
        ApiKeyRecord k;
        k.id = "k1"; k.user_id = "u1"; k.tenant_id = "t1";
        k.key_prefix = auth::extractKeyPrefix(raw_key_);
        k.key_hash = auth::hashApiKey(raw_key_);
        k.role = Role::Developer; k.status = "active";
        store_.insertApiKey(k);

        svc_ = std::make_unique<AuthService>(&store_, nullptr, gate_.get());
    }

    MemoryPersistentStore store_;
    std::unique_ptr<FeatureGate> gate_;
    std::unique_ptr<AuthService> svc_;
    std::string raw_key_;
};

TEST_F(AuthServiceRbacTest, ValidKeyReturnsContext) {
    auto ctx = svc_->resolve(raw_key_);
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(ctx->tenant_id, "t1");
    EXPECT_EQ(ctx->user_id, "u1");
    EXPECT_EQ(ctx->api_key_id, "k1");
    EXPECT_EQ(ctx->role, Role::Developer);
    EXPECT_TRUE(ctx->is_rbac_enabled);
}

TEST_F(AuthServiceRbacTest, InvalidKeyReturnsNullopt) {
    EXPECT_FALSE(svc_->resolve("sk-totally-invalid-key-12345678").has_value());
}

TEST_F(AuthServiceRbacTest, EmptyKeyReturnsNullopt) {
    EXPECT_FALSE(svc_->resolve("").has_value());
}

TEST_F(AuthServiceRbacTest, RevokedKeyReturnsNullopt) {
    store_.revokeApiKey("k1");
    EXPECT_FALSE(svc_->resolve(raw_key_).has_value());
}

TEST_F(AuthServiceRbacTest, ExpiredKeyReturnsNullopt) {
    ApiKeyRecord k;
    k.id = "k-expired"; k.user_id = "u1"; k.tenant_id = "t1";
    auto expired_key = auth::generateApiKey();
    k.key_prefix = auth::extractKeyPrefix(expired_key);
    k.key_hash = auth::hashApiKey(expired_key);
    k.role = Role::Developer; k.status = "active";
    k.expires_at = "2020-01-01T00:00:00Z";
    store_.insertApiKey(k);

    EXPECT_FALSE(svc_->resolve(expired_key).has_value());
}

TEST_F(AuthServiceRbacTest, DisabledUserReturnsNullopt) {
    User u = *store_.getUser("u1");
    u.status = "disabled";
    store_.updateUser(u);

    EXPECT_FALSE(svc_->resolve(raw_key_).has_value());
}

TEST_F(AuthServiceRbacTest, SuspendedTenantReturnsNullopt) {
    Tenant t = *store_.getTenant("t1");
    t.status = "suspended";
    store_.updateTenant(t);

    EXPECT_FALSE(svc_->resolve(raw_key_).has_value());
}

TEST_F(AuthServiceRbacTest, IsRbacEnabled) {
    EXPECT_TRUE(svc_->isRbacEnabled());
}

// --- Legacy mode tests ---

class AuthServiceLegacyTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.loadFromFile("config/aegisgate.yaml");
        gate_ = std::make_unique<FeatureGate>(Edition::Community);
        svc_ = std::make_unique<AuthService>(nullptr, &config_, gate_.get());
    }

    Config config_;
    std::unique_ptr<FeatureGate> gate_;
    std::unique_ptr<AuthService> svc_;
};

TEST_F(AuthServiceLegacyTest, IsNotRbacEnabled) {
    EXPECT_FALSE(svc_->isRbacEnabled());
}

TEST_F(AuthServiceLegacyTest, ValidLegacyKeyReturnsContext) {
    auto keys = config_.authApiKeys();
    if (keys.empty()) {
        GTEST_SKIP() << "No API keys configured in default config";
    }
    auto ctx = svc_->resolve(keys[0]);
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(ctx->role, Role::SuperAdmin);
    EXPECT_FALSE(ctx->is_rbac_enabled);
}

TEST_F(AuthServiceLegacyTest, InvalidLegacyKeyReturnsNullopt) {
    EXPECT_FALSE(svc_->resolve("invalid-legacy-key").has_value());
}

// --- Session / SSO / MFA tests ---

class AuthServiceSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_config_path_ = "/tmp/test_auth_svc_session_config.yaml";
        {
            std::ofstream ofs(tmp_config_path_);
            ofs << "edition: enterprise\n"
                << "sso:\n"
                << "  enabled: true\n"
                << "  default_provider: okta\n"
                << "mfa:\n"
                << "  enforcement: required\n"
                << "session:\n"
                << "  absolute_timeout: 3600\n"
                << "  idle_timeout: 1800\n"
                << "  max_concurrent: 5\n";
        }
        config_.loadFromFile(tmp_config_path_);

        store_ = std::make_unique<SQLitePersistentStore>(":memory:");
        store_->initialize();

        gate_ = std::make_unique<FeatureGate>(FeatureGate::createUnlocked(Edition::Enterprise));

        Tenant t; t.id = "t1"; t.name = "Acme"; t.status = "active";
        store_->insertTenant(t);

        User u; u.id = "u1"; u.tenant_id = "t1"; u.username = "alice";
        u.role = Role::Developer; u.status = "active";
        store_->insertUser(u);

        svc_ = std::make_unique<AuthService>(store_.get(), &config_, gate_.get());
    }

    void TearDown() override {
        svc_.reset();
        store_.reset();
        std::remove(tmp_config_path_.c_str());
    }

    std::string tmp_config_path_;
    Config config_;
    std::unique_ptr<SQLitePersistentStore> store_;
    std::unique_ptr<FeatureGate> gate_;
    std::unique_ptr<AuthService> svc_;
};

TEST_F(AuthServiceSessionTest, ResolveSession_ValidSession) {
    auto session = svc_->sessionManager().createSession(
        "u1", "t1", "sso", "10.0.0.1", "test-agent");
    ASSERT_TRUE(session.has_value());

    auto ctx = svc_->resolveSession(session->id);
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(ctx->tenant_id, "t1");
    EXPECT_EQ(ctx->user_id, "u1");
    EXPECT_EQ(ctx->role, Role::Developer);
    EXPECT_TRUE(ctx->is_rbac_enabled);
    EXPECT_EQ(ctx->session_id, session->id);
    EXPECT_EQ(ctx->auth_method, "sso");
    EXPECT_FALSE(ctx->mfa_verified);
}

TEST_F(AuthServiceSessionTest, ResolveSession_ExpiredSession) {
    Session s;
    s.id = "expired-session";
    s.user_id = "u1";
    s.tenant_id = "t1";
    s.ip_address = "10.0.0.1";
    s.user_agent = "test";
    s.auth_method = "sso";
    s.mfa_verified = false;
    s.created_at = "2020-01-01T00:00:00Z";
    s.last_active_at = "2020-01-01T00:00:00Z";
    s.expires_at = "2020-01-01T01:00:00Z";
    store_->insertSession(s);

    auto ctx = svc_->resolveSession("expired-session");
    EXPECT_FALSE(ctx.has_value());
}

TEST_F(AuthServiceSessionTest, ResolveSession_InactiveUser) {
    auto session = svc_->sessionManager().createSession(
        "u1", "t1", "sso", "10.0.0.1", "test-agent");
    ASSERT_TRUE(session.has_value());

    User u = *store_->getUser("u1");
    u.status = "disabled";
    store_->updateUser(u);

    auto ctx = svc_->resolveSession(session->id);
    EXPECT_FALSE(ctx.has_value());
}

TEST_F(AuthServiceSessionTest, ResolveSession_EmptyId) {
    EXPECT_FALSE(svc_->resolveSession("").has_value());
}

TEST_F(AuthServiceSessionTest, ResolveSession_NonexistentSession) {
    EXPECT_FALSE(svc_->resolveSession("nonexistent-id").has_value());
}

TEST_F(AuthServiceSessionTest, IsSsoEnabled_WithProvider) {
    SsoProvider provider;
    provider.id = "sso1";
    provider.tenant_id = "t1";
    provider.name = "okta";
    provider.issuer_url = "https://okta.example.com";
    provider.client_id = "client-id";
    provider.client_secret_enc = "encrypted-secret";
    provider.redirect_uri = "https://example.com/callback";
    provider.enabled = true;
    store_->insertSsoProvider(provider);

    EXPECT_TRUE(svc_->isSsoEnabled("t1"));
}

TEST_F(AuthServiceSessionTest, IsSsoEnabled_NoProvider) {
    EXPECT_FALSE(svc_->isSsoEnabled("t1"));
}

TEST_F(AuthServiceSessionTest, IsSsoEnabled_NonexistentTenant) {
    EXPECT_FALSE(svc_->isSsoEnabled("nonexistent-tenant"));
}

TEST_F(AuthServiceSessionTest, IsMfaRequired_EnforcementRequired) {
    AuthContext ctx;
    ctx.role = Role::Viewer;
    EXPECT_TRUE(svc_->isMfaRequired(ctx));
}

TEST_F(AuthServiceSessionTest, IsMfaRequired_EnforcementDisabled) {
    std::string cfg_path = "/tmp/test_auth_svc_mfa_disabled.yaml";
    {
        std::ofstream ofs(cfg_path);
        ofs << "mfa:\n  enforcement: disabled\n";
    }
    Config cfg;
    cfg.loadFromFile(cfg_path);

    AuthService svc(store_.get(), &cfg, gate_.get());
    AuthContext ctx;
    ctx.role = Role::SuperAdmin;
    EXPECT_FALSE(svc.isMfaRequired(ctx));
    std::remove(cfg_path.c_str());
}

TEST_F(AuthServiceSessionTest, IsMfaRequired_RoleBased_AdminRequired) {
    std::string cfg_path = "/tmp/test_auth_svc_mfa_role.yaml";
    {
        std::ofstream ofs(cfg_path);
        ofs << "mfa:\n  enforcement: role_based\n";
    }
    Config cfg;
    cfg.loadFromFile(cfg_path);

    AuthService svc(store_.get(), &cfg, gate_.get());

    AuthContext admin_ctx;
    admin_ctx.role = Role::TenantAdmin;
    EXPECT_TRUE(svc.isMfaRequired(admin_ctx));

    AuthContext viewer_ctx;
    viewer_ctx.role = Role::Viewer;
    EXPECT_FALSE(svc.isMfaRequired(viewer_ctx));
    std::remove(cfg_path.c_str());
}

TEST_F(AuthServiceSessionTest, SessionManager_Accessible) {
    auto& mgr = svc_->sessionManager();
    auto session = mgr.createSession("u1", "t1", "sso", "10.0.0.1", "test");
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->user_id, "u1");
}
