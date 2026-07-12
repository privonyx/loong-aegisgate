// Phase 9.3 Epic 5 Task 5.3 — AuthInterceptor (Bearer + SR1 role gate).
//
// Every test wires a real AuthService with RBAC enabled so the decision
// tree matches production behaviour. Fixtures pre-register three API keys:
//   * viewer_key  — Role::Viewer    (should be PERMISSION_DENIED)
//   * admin_key   — Role::TenantAdmin (should be PERMISSION_DENIED too — SR1
//                                        requires SuperAdmin for the control
//                                        plane; TenantAdmin is not enough)
//   * super_key   — Role::SuperAdmin  (should be OK)
// We also exercise pure failure modes (missing header / malformed Bearer /
// unknown key) without hitting the resolver more than necessary.

#include "auth/auth_service.h"
#include "auth/auth_models.h"
#include "auth/crypto_utils.h"
#include "control_plane/grpc/auth_interceptor.h"
#include "core/feature_gate.h"
#include "storage/memory_persistent_store.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>

namespace aegisgate {
namespace {

using control_plane::grpc_adapter::AuthInterceptor;

class AuthInterceptorTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryPersistentStore>();
        ASSERT_TRUE(store_->initialize());

        // Enterprise edition unlocks RBAC so AuthService routes through
        // resolveRbac() instead of the legacy config-file allow-list. That
        // matches deployed control-plane environments.
        gate_ = std::make_unique<FeatureGate>(
            FeatureGate::createUnlocked(Edition::Enterprise));
        auth_svc_ = std::make_unique<AuthService>(
            store_.get(), nullptr, gate_.get());

        // Minimal org tree.
        Tenant t;
        t.id = "t1";
        t.name = "Tenant One";
        t.status = "active";
        store_->insertTenant(t);

        User su;
        su.id = "u-super";
        su.tenant_id = "t1";
        su.username = "super";
        su.role = Role::SuperAdmin;
        su.status = "active";
        store_->insertUser(su);

        User ta;
        ta.id = "u-admin";
        ta.tenant_id = "t1";
        ta.username = "tadmin";
        ta.role = Role::TenantAdmin;
        ta.status = "active";
        store_->insertUser(ta);

        User viewer;
        viewer.id = "u-viewer";
        viewer.tenant_id = "t1";
        viewer.username = "view";
        viewer.role = Role::Viewer;
        viewer.status = "active";
        store_->insertUser(viewer);

        super_key_ = registerApiKey("k-super", "u-super", Role::SuperAdmin,
                                     "sk-super-xxxxxxxxxxxxxxxxxxxxxxxx");
        admin_key_ = registerApiKey("k-admin", "u-admin", Role::TenantAdmin,
                                     "sk-admin-xxxxxxxxxxxxxxxxxxxxxxxx");
        viewer_key_ = registerApiKey("k-view", "u-viewer", Role::Viewer,
                                      "sk-view-xxxxxxxxxxxxxxxxxxxxxxxxx");

        interceptor_ = std::make_unique<AuthInterceptor>(auth_svc_.get());
    }

    std::string registerApiKey(const std::string& key_id,
                                const std::string& user_id,
                                Role role,
                                const std::string& raw_key) {
        ApiKeyRecord k;
        k.id = key_id;
        k.user_id = user_id;
        k.tenant_id = "t1";
        k.name = key_id;
        k.key_prefix = auth::extractKeyPrefix(raw_key);
        k.key_hash = auth::hashApiKey(raw_key);
        k.role = role;
        k.status = "active";
        store_->insertApiKey(k);
        return raw_key;
    }

    static std::multimap<std::string, std::string> makeHeaders(
        const std::string& authorization) {
        std::multimap<std::string, std::string> m;
        m.emplace("authorization", authorization);
        return m;
    }

    std::unique_ptr<MemoryPersistentStore> store_;
    std::unique_ptr<FeatureGate>           gate_;
    std::unique_ptr<AuthService>           auth_svc_;
    std::unique_ptr<AuthInterceptor>       interceptor_;
    std::string super_key_;
    std::string admin_key_;
    std::string viewer_key_;
};

// ---------------------------------------------------------------------------
// Pure parser — exercised directly so tests stay fast when resolution fails.
// ---------------------------------------------------------------------------

TEST(AuthInterceptorParser, AcceptsCanonicalBearer) {
    EXPECT_EQ(AuthInterceptor::parseBearer("Bearer sk-abc"), "sk-abc");
}

TEST(AuthInterceptorParser, CaseInsensitiveScheme) {
    EXPECT_EQ(AuthInterceptor::parseBearer("bearer sk-abc"), "sk-abc");
    EXPECT_EQ(AuthInterceptor::parseBearer("BEARER sk-abc"), "sk-abc");
    EXPECT_EQ(AuthInterceptor::parseBearer("bEaReR sk-abc"), "sk-abc");
}

TEST(AuthInterceptorParser, StripsSurroundingSpaces) {
    EXPECT_EQ(AuthInterceptor::parseBearer("  Bearer   sk-abc  "), "sk-abc");
}

TEST(AuthInterceptorParser, RejectsWrongScheme) {
    EXPECT_EQ(AuthInterceptor::parseBearer("Basic dXNlcjpwdw=="), "");
    EXPECT_EQ(AuthInterceptor::parseBearer("Token sk-abc"), "");
}

TEST(AuthInterceptorParser, RejectsEmptyToken) {
    EXPECT_EQ(AuthInterceptor::parseBearer("Bearer "), "");
    EXPECT_EQ(AuthInterceptor::parseBearer("Bearer"), "");
}

TEST(AuthInterceptorParser, RejectsMissingSeparator) {
    // "Bearersk-abc" is NOT a valid credential per RFC 6750.
    EXPECT_EQ(AuthInterceptor::parseBearer("Bearersk-abc"), "");
}

// ---------------------------------------------------------------------------
// Authorization flow (full AuthService behind it)
// ---------------------------------------------------------------------------

TEST_F(AuthInterceptorTest, MissingAuthorizationHeaderIsUnauthenticated) {
    std::multimap<std::string, std::string> empty;
    auto r = interceptor_->authorizeHeaders(empty);
    EXPECT_EQ(r.status.error_code(), grpc::UNAUTHENTICATED);
}

TEST_F(AuthInterceptorTest, MalformedBearerIsUnauthenticated) {
    auto r = interceptor_->authorizeHeaders(makeHeaders("not-a-bearer"));
    EXPECT_EQ(r.status.error_code(), grpc::UNAUTHENTICATED);
}

TEST_F(AuthInterceptorTest, UnknownApiKeyIsUnauthenticated) {
    auto r = interceptor_->authorizeHeaders(
        makeHeaders("Bearer sk-does-not-exist-00000000"));
    EXPECT_EQ(r.status.error_code(), grpc::UNAUTHENTICATED);
}

TEST_F(AuthInterceptorTest, SuperAdminKeyIsAccepted) {
    auto r = interceptor_->authorizeHeaders(
        makeHeaders("Bearer " + super_key_));
    ASSERT_TRUE(r.status.ok()) << r.status.error_message();
    EXPECT_EQ(r.context.user_id, "u-super");
    EXPECT_EQ(r.context.role, Role::SuperAdmin);
    EXPECT_TRUE(r.context.is_rbac_enabled);
}

TEST_F(AuthInterceptorTest, TenantAdminIsPermissionDenied_SR1) {
    // TenantAdmin authenticates successfully but MUST be rejected with
    // PERMISSION_DENIED (not UNAUTHENTICATED) so the client learns the
    // account is valid but under-privileged.
    auto r = interceptor_->authorizeHeaders(
        makeHeaders("Bearer " + admin_key_));
    EXPECT_EQ(r.status.error_code(), grpc::PERMISSION_DENIED);
    EXPECT_EQ(r.context.user_id, "");
}

TEST_F(AuthInterceptorTest, ViewerIsPermissionDenied_SR1) {
    auto r = interceptor_->authorizeHeaders(
        makeHeaders("Bearer " + viewer_key_));
    EXPECT_EQ(r.status.error_code(), grpc::PERMISSION_DENIED);
}

TEST_F(AuthInterceptorTest, RevokedKeyIsUnauthenticated) {
    // Mutate the stored record to 'revoked' and retry.
    auto keys = store_->getApiKeysByPrefix(
        auth::extractKeyPrefix(super_key_));
    ASSERT_EQ(keys.size(), 1u);
    auto rec = keys.front();
    rec.status = "revoked";
    store_->updateApiKey(rec);

    auto r = interceptor_->authorizeHeaders(
        makeHeaders("Bearer " + super_key_));
    EXPECT_EQ(r.status.error_code(), grpc::UNAUTHENTICATED);
}

TEST_F(AuthInterceptorTest, StatusMessageDoesNotLeakToken) {
    // SR8: error messages must never contain the submitted token or any
    // raw header value. This guards against log-scraping attacks.
    const std::string token = "sk-deadbeefdeadbeefdeadbeefdeadbeef";
    auto r = interceptor_->authorizeHeaders(makeHeaders("Bearer " + token));
    EXPECT_EQ(r.status.error_code(), grpc::UNAUTHENTICATED);
    EXPECT_EQ(r.status.error_message().find(token), std::string::npos);
}

TEST_F(AuthInterceptorTest, MakeUserExtractorReturnsEmptyWhenUnauthorized) {
    // The UserExtractor seam used by ConfigServiceImpl erases the
    // PERMISSION_DENIED distinction by design (Impl handles only
    // UNAUTHENTICATED at that boundary). Nonetheless, the extractor must
    // not expose the resolved user_id for a non-SuperAdmin.
    auto extractor = AuthInterceptor::makeUserExtractor(interceptor_.get());
    grpc::ServerContext ctx;
    ctx.AddInitialMetadata("authorization", "Bearer " + admin_key_);
    EXPECT_EQ(extractor(&ctx), "");
}

TEST_F(AuthInterceptorTest, NullInterceptorPointerReturnsEmptyUser) {
    auto extractor = AuthInterceptor::makeUserExtractor(nullptr);
    grpc::ServerContext ctx;
    EXPECT_EQ(extractor(&ctx), "");
}

TEST(AuthInterceptorTest_NullResolver, NullAuthServiceFailsClosed) {
    // A misconfigured deployment (forgot to wire AuthService) must fail
    // every request — we never silently authenticate as anonymous.
    AuthInterceptor interceptor(nullptr);
    std::multimap<std::string, std::string> headers;
    headers.emplace("authorization", "Bearer anything");
    auto r = interceptor.authorizeHeaders(headers);
    EXPECT_EQ(r.status.error_code(), grpc::UNAUTHENTICATED);
}

} // namespace
} // namespace aegisgate
