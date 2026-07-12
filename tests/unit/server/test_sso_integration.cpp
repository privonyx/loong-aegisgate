#include "server/admin_controller.h"
#include "auth/auth_service.h"
#include "auth/oidc_client.h"
#include "auth/identity_mapper.h"
#include "guardrail/audit.h"
#include "storage/sqlite_persistent_store.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace aegisgate;
using json = nlohmann::json;

namespace {

json makeFakeDiscoveryResponse() {
    return {
        {"issuer", "https://idp.example.com"},
        {"authorization_endpoint", "https://idp.example.com/authorize"},
        {"token_endpoint", "https://idp.example.com/token"},
        {"jwks_uri", "https://idp.example.com/.well-known/jwks.json"},
        {"userinfo_endpoint", "https://idp.example.com/userinfo"},
        {"end_session_endpoint", "https://idp.example.com/logout"}
    };
}

json makeFakeTokenResponse(const std::string& id_token = "fake.id.token") {
    return {
        {"id_token", id_token},
        {"access_token", "fake-access-token"},
        {"refresh_token", "fake-refresh-token"},
        {"token_type", "Bearer"},
        {"expires_in", 3600}
    };
}

OidcClient::HttpFetchFn createMockFetch(
    std::function<std::optional<std::string>(const std::string& url)> handler) {
    return [handler](const std::string& url, const std::string&,
                     const std::string&, const std::string&)
        -> std::optional<std::string> {
        return handler(url);
    };
}

} // namespace

class SsoIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<SQLitePersistentStore>(":memory:");
        ASSERT_TRUE(store_->initialize());

        gate_ = std::make_unique<FeatureGate>(
            FeatureGate::createUnlocked(Edition::Enterprise));
        config_ = std::make_unique<Config>();
        auth_svc_ = std::make_unique<AuthService>(
            store_.get(), config_.get(), gate_.get());
        audit_ = std::make_unique<AuditLogger>();
        identity_mapper_ = std::make_unique<IdentityMapper>(store_.get());

        super_ctx_.role = Role::SuperAdmin;
        super_ctx_.tenant_id = "t-super";
        super_ctx_.is_rbac_enabled = true;

        admin_ctx_.role = Role::TenantAdmin;
        admin_ctx_.tenant_id = "t1";
        admin_ctx_.is_rbac_enabled = true;

        dev_ctx_.role = Role::Developer;
        dev_ctx_.tenant_id = "t1";
        dev_ctx_.is_rbac_enabled = true;

        Tenant t;
        t.id = "t1"; t.name = "TestTenant"; t.status = "active";
        store_->insertTenant(t);
    }

    void TearDown() override {
        if (audit_) audit_->shutdown();
    }

    void insertTestProvider(const std::string& id = "prov1",
                            const std::string& tenant_id = "t1") {
        SsoProvider p;
        p.id = id;
        p.tenant_id = tenant_id;
        p.name = "Test IdP";
        p.issuer_url = "https://idp.example.com";
        p.client_id = "test-client-id";
        p.client_secret_enc = "test-secret";
        p.redirect_uri = "https://app.example.com/admin/auth/sso/callback";
        p.scopes = {"openid", "profile", "email"};
        p.jit_provisioning = true;
        p.default_role = "viewer";
        p.enabled = true;
        p.created_at = "2026-03-25T00:00:00Z";
        p.updated_at = p.created_at;
        store_->insertSsoProvider(p);
    }

    std::unique_ptr<SQLitePersistentStore> store_;
    std::unique_ptr<FeatureGate> gate_;
    std::unique_ptr<Config> config_;
    std::unique_ptr<AuthService> auth_svc_;
    std::unique_ptr<AuditLogger> audit_;
    std::unique_ptr<IdentityMapper> identity_mapper_;
    AuthContext super_ctx_, admin_ctx_, dev_ctx_;
};

// === SSO Login Initiation ===

TEST_F(SsoIntegrationTest, InitiateSsoLogin_ReturnsRedirectUrl) {
    insertTestProvider();

    auto mock_fetch = createMockFetch([](const std::string& url)
        -> std::optional<std::string> {
        if (url.find("openid-configuration") != std::string::npos)
            return makeFakeDiscoveryResponse().dump();
        return std::nullopt;
    });

    OidcClient oidc(mock_fetch);
    AdminController ctrl(store_.get(), auth_svc_.get(), audit_.get(),
                         &oidc, identity_mapper_.get());

    auto result = ctrl.initiateSsoLogin("t1");
    EXPECT_TRUE(result.error.empty()) << result.error;
    EXPECT_FALSE(result.redirect_url.empty());
    EXPECT_FALSE(result.state.empty());
    EXPECT_FALSE(result.nonce.empty());
    EXPECT_FALSE(result.code_verifier.empty());
    EXPECT_NE(result.redirect_url.find("response_type=code"), std::string::npos);
    EXPECT_NE(result.redirect_url.find("client_id=test-client-id"), std::string::npos);
    EXPECT_NE(result.redirect_url.find("code_challenge_method=S256"), std::string::npos);
}

TEST_F(SsoIntegrationTest, InitiateSsoLogin_NoProvider) {
    OidcClient oidc;
    AdminController ctrl(store_.get(), auth_svc_.get(), audit_.get(),
                         &oidc, identity_mapper_.get());

    auto result = ctrl.initiateSsoLogin("non-existent-tenant");
    EXPECT_FALSE(result.error.empty());
    EXPECT_TRUE(result.redirect_url.empty());
}

TEST_F(SsoIntegrationTest, InitiateSsoLogin_NoOidcClient) {
    AdminController ctrl(store_.get(), auth_svc_.get(), audit_.get());
    auto result = ctrl.initiateSsoLogin("t1");
    EXPECT_EQ(result.error, "OIDC client not configured");
}

// === SSO Callback ===

TEST_F(SsoIntegrationTest, HandleSsoCallback_InvalidState) {
    insertTestProvider();

    auto mock_fetch = createMockFetch([](const std::string& url)
        -> std::optional<std::string> {
        if (url.find("openid-configuration") != std::string::npos)
            return makeFakeDiscoveryResponse().dump();
        if (url.find("token") != std::string::npos)
            return makeFakeTokenResponse().dump();
        return std::nullopt;
    });

    OidcClient oidc(mock_fetch);
    AdminController ctrl(store_.get(), auth_svc_.get(), audit_.get(),
                         &oidc, identity_mapper_.get());

    auto result = ctrl.handleSsoCallback(
        "auth-code", "wrong-verifier", "nonce", "non-existent", "1.2.3.4", "TestBrowser");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
}

TEST_F(SsoIntegrationTest, HandleSsoCallback_TokenExchangeFails) {
    insertTestProvider();

    auto mock_fetch = createMockFetch([](const std::string& url)
        -> std::optional<std::string> {
        if (url.find("openid-configuration") != std::string::npos)
            return makeFakeDiscoveryResponse().dump();
        return std::nullopt;
    });

    OidcClient oidc(mock_fetch);
    AdminController ctrl(store_.get(), auth_svc_.get(), audit_.get(),
                         &oidc, identity_mapper_.get());

    auto result = ctrl.handleSsoCallback(
        "auth-code", "verifier", "nonce", "t1", "1.2.3.4", "TestBrowser");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error, "Token exchange failed");
}

TEST_F(SsoIntegrationTest, HandleSsoCallback_NoServices) {
    AdminController ctrl(store_.get(), nullptr, audit_.get());
    auto result = ctrl.handleSsoCallback(
        "code", "verifier", "nonce", "t1", "1.2.3.4", "TestBrowser");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error, "SSO services not configured");
}

// === SSO Logout ===

TEST_F(SsoIntegrationTest, HandleSsoLogout_ReturnsEndSessionUrl) {
    insertTestProvider();

    auto mock_fetch = createMockFetch([](const std::string& url)
        -> std::optional<std::string> {
        if (url.find("openid-configuration") != std::string::npos)
            return makeFakeDiscoveryResponse().dump();
        return std::nullopt;
    });

    OidcClient oidc(mock_fetch);
    AdminController ctrl(store_.get(), auth_svc_.get(), audit_.get(),
                         &oidc, identity_mapper_.get());

    User u;
    u.id = "u-logout"; u.tenant_id = "t1"; u.username = "logout-user";
    u.role = Role::Developer; u.status = "active";
    store_->insertUser(u);

    auto session = auth_svc_->sessionManager().createSession(
        "u-logout", "t1", "sso", "1.2.3.4", "TestBrowser");
    ASSERT_TRUE(session.has_value());

    auto end_url = ctrl.handleSsoLogout(session->id, "t1");
    EXPECT_EQ(end_url, "https://idp.example.com/logout");

    auto deleted_session = auth_svc_->sessionManager().getSession(session->id);
    EXPECT_FALSE(deleted_session.has_value());
}

TEST_F(SsoIntegrationTest, HandleSsoLogout_NoProvider) {
    OidcClient oidc;
    AdminController ctrl(store_.get(), auth_svc_.get(), audit_.get(),
                         &oidc, identity_mapper_.get());

    auto end_url = ctrl.handleSsoLogout("some-session", "non-existent");
    EXPECT_TRUE(end_url.empty());
}

// === SSO Provider CRUD ===

TEST_F(SsoIntegrationTest, CrudSsoProvider_CreateAndGet) {
    AdminController ctrl(store_.get(), auth_svc_.get(), audit_.get());

    json body = {
        {"tenant_id", "t1"},
        {"name", "Okta"},
        {"issuer_url", "https://okta.example.com"},
        {"client_id", "okta-client"},
        {"client_secret", "okta-secret"},
        {"redirect_uri", "https://app.example.com/callback"},
        {"scopes", {"openid", "profile"}},
        {"jit_provisioning", true},
        {"default_role", "developer"}
    };

    auto cr = ctrl.createSsoProvider(super_ctx_, body);
    EXPECT_EQ(cr.status, 201);
    EXPECT_FALSE(cr.is_error);
    EXPECT_EQ(cr.body["name"], "Okta");
    EXPECT_EQ(cr.body["issuer_url"], "https://okta.example.com");
    EXPECT_EQ(cr.body["client_id"], "okta-client");
    EXPECT_TRUE(cr.body["has_client_secret"].get<bool>());
    EXPECT_FALSE(cr.body.contains("client_secret_enc"));
    EXPECT_EQ(cr.body["default_role"], "developer");

    auto id = cr.body["id"].get<std::string>();
    auto gr = ctrl.getSsoProvider(super_ctx_, id);
    EXPECT_EQ(gr.status, 200);
    EXPECT_EQ(gr.body["id"], id);
}

TEST_F(SsoIntegrationTest, CrudSsoProvider_MissingRequired) {
    AdminController ctrl(store_.get(), auth_svc_.get(), audit_.get());

    auto r = ctrl.createSsoProvider(super_ctx_, {{"name", "bad"}});
    EXPECT_EQ(r.status, 400);
    EXPECT_TRUE(r.is_error);
}

TEST_F(SsoIntegrationTest, CrudSsoProvider_NonSuperAdminDenied) {
    AdminController ctrl(store_.get(), auth_svc_.get(), audit_.get());

    json body = {
        {"tenant_id", "t1"},
        {"issuer_url", "https://idp.example.com"},
        {"client_id", "test"}
    };

    auto cr = ctrl.createSsoProvider(admin_ctx_, body);
    EXPECT_EQ(cr.status, 403);

    auto lr = ctrl.listSsoProviders(dev_ctx_, 100, 0);
    EXPECT_EQ(lr.status, 403);
}

TEST_F(SsoIntegrationTest, CrudSsoProvider_ListProviders) {
    AdminController ctrl(store_.get(), auth_svc_.get(), audit_.get());

    json body1 = {
        {"tenant_id", "t1"}, {"name", "IdP1"},
        {"issuer_url", "https://idp1.example.com"}, {"client_id", "c1"}
    };
    json body2 = {
        {"tenant_id", "t1"}, {"name", "IdP2"},
        {"issuer_url", "https://idp2.example.com"}, {"client_id", "c2"}
    };

    ctrl.createSsoProvider(super_ctx_, body1);
    ctrl.createSsoProvider(super_ctx_, body2);

    auto lr = ctrl.listSsoProviders(super_ctx_, 100, 0);
    EXPECT_EQ(lr.status, 200);
    EXPECT_EQ(lr.body["data"].size(), 2u);
    EXPECT_EQ(lr.body["count"], 2);
}

TEST_F(SsoIntegrationTest, CrudSsoProvider_Update) {
    AdminController ctrl(store_.get(), auth_svc_.get(), audit_.get());

    json body = {
        {"tenant_id", "t1"}, {"name", "Original"},
        {"issuer_url", "https://idp.example.com"}, {"client_id", "c1"}
    };
    auto cr = ctrl.createSsoProvider(super_ctx_, body);
    auto id = cr.body["id"].get<std::string>();

    auto ur = ctrl.updateSsoProvider(super_ctx_, id,
        {{"name", "Updated"}, {"enabled", false}});
    EXPECT_EQ(ur.status, 200);
    EXPECT_EQ(ur.body["name"], "Updated");
    EXPECT_FALSE(ur.body["enabled"].get<bool>());
}

TEST_F(SsoIntegrationTest, CrudSsoProvider_Delete) {
    AdminController ctrl(store_.get(), auth_svc_.get(), audit_.get());

    json body = {
        {"tenant_id", "t1"}, {"name", "ToDelete"},
        {"issuer_url", "https://idp.example.com"}, {"client_id", "c1"}
    };
    auto cr = ctrl.createSsoProvider(super_ctx_, body);
    auto id = cr.body["id"].get<std::string>();

    auto dr = ctrl.deleteSsoProvider(super_ctx_, id);
    EXPECT_EQ(dr.status, 200);
    EXPECT_TRUE(dr.body["deleted"].get<bool>());

    auto gr = ctrl.getSsoProvider(super_ctx_, id);
    EXPECT_EQ(gr.status, 400);
}

TEST_F(SsoIntegrationTest, CrudSsoProvider_UpdateNotFound) {
    AdminController ctrl(store_.get(), auth_svc_.get(), audit_.get());
    auto r = ctrl.updateSsoProvider(super_ctx_, "nonexistent", {{"name", "x"}});
    EXPECT_EQ(r.status, 400);
}

TEST_F(SsoIntegrationTest, CrudSsoProvider_DeleteNotFound) {
    AdminController ctrl(store_.get(), auth_svc_.get(), audit_.get());
    auto r = ctrl.deleteSsoProvider(super_ctx_, "nonexistent");
    EXPECT_EQ(r.status, 400);
}

TEST_F(SsoIntegrationTest, CrudSsoProvider_ClaimAndGroupMapping) {
    AdminController ctrl(store_.get(), auth_svc_.get(), audit_.get());

    json body = {
        {"tenant_id", "t1"},
        {"name", "WithMappings"},
        {"issuer_url", "https://idp.example.com"},
        {"client_id", "c1"},
        {"claim_mapping", {{"email", "email"}, {"username", "preferred_username"}}},
        {"group_role_mapping", {{"admins", "super_admin"}, {"devs", "developer"}}}
    };

    auto cr = ctrl.createSsoProvider(super_ctx_, body);
    EXPECT_EQ(cr.status, 201);
    EXPECT_TRUE(cr.body.contains("claim_mapping"));
    EXPECT_TRUE(cr.body.contains("group_role_mapping"));
    EXPECT_EQ(cr.body["claim_mapping"]["email"], "email");
    EXPECT_EQ(cr.body["group_role_mapping"]["admins"], "super_admin");
}
