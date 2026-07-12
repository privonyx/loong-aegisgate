#include "auth/auth_service.h"
#include "auth/authorization.h"
#include "auth/crypto_utils.h"
#include "server/admin_controller.h"
#include "storage/memory_persistent_store.h"
#include <gtest/gtest.h>

using namespace aegisgate;
using json = nlohmann::json;

class RbacE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        store_.initialize();
        gate_ = std::make_unique<FeatureGate>(FeatureGate::createUnlocked(Edition::Enterprise));
        auth_svc_ = std::make_unique<AuthService>(&store_, nullptr, gate_.get());
        ctrl_ = std::make_unique<AdminController>(&store_, auth_svc_.get());

        // Bootstrap: create SuperAdmin user and key via direct store access
        Tenant root_t; root_t.id = "root"; root_t.name = "Root"; root_t.status = "active";
        store_.insertTenant(root_t);

        User super_u; super_u.id = "u-super"; super_u.tenant_id = "root";
        super_u.username = "superadmin"; super_u.role = Role::SuperAdmin; super_u.status = "active";
        store_.insertUser(super_u);

        super_key_ = auth::generateApiKey();
        ApiKeyRecord sk; sk.id = "k-super"; sk.user_id = "u-super"; sk.tenant_id = "root";
        sk.key_prefix = auth::extractKeyPrefix(super_key_);
        sk.key_hash = auth::hashApiKey(super_key_);
        sk.role = Role::SuperAdmin; sk.status = "active";
        store_.insertApiKey(sk);
    }

    AuthContext resolveKey(const std::string& key) {
        auto ctx = auth_svc_->resolve(key);
        EXPECT_TRUE(ctx.has_value()) << "Failed to resolve key";
        return ctx.value_or(AuthContext{});
    }

    MemoryPersistentStore store_;
    std::unique_ptr<FeatureGate> gate_;
    std::unique_ptr<AuthService> auth_svc_;
    std::unique_ptr<AdminController> ctrl_;
    std::string super_key_;
};

TEST_F(RbacE2ETest, FullLifecycle) {
    // 1. Resolve SuperAdmin key
    auto super_ctx = resolveKey(super_key_);
    EXPECT_EQ(super_ctx.role, Role::SuperAdmin);

    // 2. Create a tenant
    auto r = ctrl_->createTenant(super_ctx, {{"name", "Acme Corp"}});
    ASSERT_EQ(r.status, 201);
    auto tenant_id = r.body["id"].get<std::string>();

    // 3. Create TenantAdmin user
    auto r2 = ctrl_->createUser(super_ctx, {
        {"username", "admin1"}, {"tenant_id", tenant_id}, {"role", "tenant_admin"}});
    ASSERT_EQ(r2.status, 201);
    auto admin_user_id = r2.body["id"].get<std::string>();

    // 4. Create TenantAdmin key
    auto r3 = ctrl_->createApiKey(super_ctx, {
        {"user_id", admin_user_id}, {"role", "tenant_admin"}});
    ASSERT_EQ(r3.status, 201);
    auto admin_key = r3.body["key"].get<std::string>();

    // 5. Resolve TenantAdmin key
    auto admin_ctx = resolveKey(admin_key);
    EXPECT_EQ(admin_ctx.role, Role::TenantAdmin);
    EXPECT_EQ(admin_ctx.tenant_id, tenant_id);

    // 6. TenantAdmin creates Developer user
    auto r4 = ctrl_->createUser(admin_ctx, {{"username", "dev1"}, {"role", "developer"}});
    ASSERT_EQ(r4.status, 201);
    auto dev_user_id = r4.body["id"].get<std::string>();

    // 7. Create Developer key
    auto r5 = ctrl_->createApiKey(admin_ctx, {{"user_id", dev_user_id}});
    ASSERT_EQ(r5.status, 201);
    auto dev_key = r5.body["key"].get<std::string>();

    // 8. Developer key resolves correctly
    auto dev_ctx = resolveKey(dev_key);
    EXPECT_EQ(dev_ctx.role, Role::Developer);

    // 9. Developer cannot access admin APIs
    auto r6 = ctrl_->createUser(dev_ctx, {{"username", "hacker"}});
    EXPECT_EQ(r6.status, 403);

    // 10. Revoke Developer key
    auto dev_key_id = r5.body["id"].get<std::string>();
    auto r7 = ctrl_->revokeApiKey(admin_ctx, dev_key_id);
    EXPECT_EQ(r7.status, 200);

    // 11. Revoked key fails authentication
    EXPECT_FALSE(auth_svc_->resolve(dev_key).has_value());
}

TEST_F(RbacE2ETest, TenantAdminCannotCreateCrossTenantUsers) {
    auto super_ctx = resolveKey(super_key_);

    // Create two tenants
    ctrl_->createTenant(super_ctx, {{"name", "A"}});
    auto r2 = ctrl_->createTenant(super_ctx, {{"name", "B"}});
    auto tenant_b_id = r2.body["id"].get<std::string>();

    // Create admin for tenant A
    auto r3 = ctrl_->createUser(super_ctx, {{"username", "admin-a"}, {"role", "tenant_admin"}});
    auto admin_a_id = r3.body["id"].get<std::string>();
    auto r4 = ctrl_->createApiKey(super_ctx, {{"user_id", admin_a_id}, {"role", "tenant_admin"}});
    auto admin_a_key = r4.body["key"].get<std::string>();
    auto admin_a_ctx = resolveKey(admin_a_key);

    // TenantAdmin A cannot create user in tenant B
    auto r5 = ctrl_->createUser(admin_a_ctx, {
        {"username", "intruder"}, {"tenant_id", tenant_b_id}});
    EXPECT_EQ(r5.status, 403);
}

TEST_F(RbacE2ETest, DisabledUserKeyFails) {
    auto super_ctx = resolveKey(super_key_);

    auto r1 = ctrl_->createUser(super_ctx, {{"username", "alice"}, {"role", "developer"}});
    auto user_id = r1.body["id"].get<std::string>();
    auto r2 = ctrl_->createApiKey(super_ctx, {{"user_id", user_id}});
    auto key = r2.body["key"].get<std::string>();

    EXPECT_TRUE(auth_svc_->resolve(key).has_value());

    // Disable user
    ctrl_->updateUser(super_ctx, user_id, {{"status", "disabled"}});

    EXPECT_FALSE(auth_svc_->resolve(key).has_value());
}

TEST_F(RbacE2ETest, SuspendedTenantKeyFails) {
    auto super_ctx = resolveKey(super_key_);

    auto r1 = ctrl_->createTenant(super_ctx, {{"name", "Suspended Corp"}});
    auto tid = r1.body["id"].get<std::string>();
    auto r2 = ctrl_->createUser(super_ctx, {
        {"username", "a"}, {"tenant_id", tid}, {"role", "developer"}});
    auto uid = r2.body["id"].get<std::string>();
    auto r3 = ctrl_->createApiKey(super_ctx, {{"user_id", uid}});
    auto key = r3.body["key"].get<std::string>();

    EXPECT_TRUE(auth_svc_->resolve(key).has_value());

    // Suspend tenant
    ctrl_->updateTenant(super_ctx, tid, {{"status", "suspended"}});

    EXPECT_FALSE(auth_svc_->resolve(key).has_value());
}
