#include "auth/auth_service.h"
#include "auth/authorization.h"
#include "auth/crypto_utils.h"
#include "server/admin_controller.h"
#include "storage/memory_persistent_store.h"
#include <gtest/gtest.h>

using namespace aegisgate;
using json = nlohmann::json;

class TenantIsolationTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_.initialize();
        gate_ = std::make_unique<FeatureGate>(FeatureGate::createUnlocked(Edition::Enterprise));
        auth_svc_ = std::make_unique<AuthService>(&store_, nullptr, gate_.get());
        ctrl_ = std::make_unique<AdminController>(&store_, auth_svc_.get());

        // Bootstrap root tenant + SuperAdmin
        Tenant root; root.id = "root"; root.name = "Root"; root.status = "active";
        store_.insertTenant(root);
        User su; su.id = "u-super"; su.tenant_id = "root";
        su.username = "admin"; su.role = Role::SuperAdmin; su.status = "active";
        store_.insertUser(su);
        super_key_ = auth::generateApiKey();
        ApiKeyRecord sk; sk.id = "k-super"; sk.user_id = "u-super"; sk.tenant_id = "root";
        sk.key_prefix = auth::extractKeyPrefix(super_key_);
        sk.key_hash = auth::hashApiKey(super_key_);
        sk.role = Role::SuperAdmin; sk.status = "active";
        store_.insertApiKey(sk);
        super_ctx_ = auth_svc_->resolve(super_key_).value();

        // Create tenant A and B
        auto ra = ctrl_->createTenant(super_ctx_, {{"name", "Tenant A"}});
        tenant_a_id_ = ra.body["id"].get<std::string>();
        auto rb = ctrl_->createTenant(super_ctx_, {{"name", "Tenant B"}});
        tenant_b_id_ = rb.body["id"].get<std::string>();

        // Create viewer in each tenant
        auto ua = ctrl_->createUser(super_ctx_, {
            {"username", "viewer-a"}, {"tenant_id", tenant_a_id_}, {"role", "viewer"}});
        auto ub = ctrl_->createUser(super_ctx_, {
            {"username", "viewer-b"}, {"tenant_id", tenant_b_id_}, {"role", "viewer"}});

        auto ka = ctrl_->createApiKey(super_ctx_, {
            {"user_id", ua.body["id"].get<std::string>()}, {"role", "viewer"}});
        auto kb = ctrl_->createApiKey(super_ctx_, {
            {"user_id", ub.body["id"].get<std::string>()}, {"role", "viewer"}});

        viewer_a_key_ = ka.body["key"].get<std::string>();
        viewer_b_key_ = kb.body["key"].get<std::string>();
        viewer_a_ctx_ = auth_svc_->resolve(viewer_a_key_).value();
        viewer_b_ctx_ = auth_svc_->resolve(viewer_b_key_).value();

        // Add audit data for each tenant
        AuditEntry ea; ea.request_id = "r-a"; ea.tenant_id = tenant_a_id_;
        ea.timestamp = "2026-03-22T10:00:00Z"; ea.action = "chat";
        store_.insertAudit(ea);

        AuditEntry eb; eb.request_id = "r-b"; eb.tenant_id = tenant_b_id_;
        eb.timestamp = "2026-03-22T11:00:00Z"; eb.action = "chat";
        store_.insertAudit(eb);

        // Add cost data
        CostRecord ca; ca.request_id = "r-a"; ca.tenant_id = tenant_a_id_;
        ca.model = "gpt-4"; ca.total_cost = 0.05; ca.timestamp = "2026-03-22T10:00:00Z";
        store_.insertCostRecord(ca);

        CostRecord cb; cb.request_id = "r-b"; cb.tenant_id = tenant_b_id_;
        cb.model = "gpt-4"; cb.total_cost = 1.00; cb.timestamp = "2026-03-22T11:00:00Z";
        store_.insertCostRecord(cb);
    }

    MemoryPersistentStore store_;
    std::unique_ptr<FeatureGate> gate_;
    std::unique_ptr<AuthService> auth_svc_;
    std::unique_ptr<AdminController> ctrl_;
    std::string super_key_, viewer_a_key_, viewer_b_key_;
    AuthContext super_ctx_, viewer_a_ctx_, viewer_b_ctx_;
    std::string tenant_a_id_, tenant_b_id_;
};

TEST_F(TenantIsolationTest, ViewerACannotQueryTenantBAudits) {
    auto r = ctrl_->queryAudits(viewer_a_ctx_, tenant_b_id_, 100, 0);
    EXPECT_EQ(r.status, 403);
}

TEST_F(TenantIsolationTest, ViewerACanQueryOwnAudits) {
    auto r = ctrl_->queryAudits(viewer_a_ctx_, tenant_a_id_, 100, 0);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["count"], 1);
}

TEST_F(TenantIsolationTest, ViewerBCannotQueryTenantACosts) {
    auto r = ctrl_->queryCosts(viewer_b_ctx_, tenant_a_id_, "", 100, 0);
    EXPECT_EQ(r.status, 403);
}

TEST_F(TenantIsolationTest, SuperAdminCanCrossTenantQuery) {
    auto r = ctrl_->queryAudits(super_ctx_, tenant_b_id_, 100, 0);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["count"], 1);

    auto r2 = ctrl_->queryCosts(super_ctx_, tenant_a_id_, "", 100, 0);
    EXPECT_EQ(r2.status, 200);
}

TEST_F(TenantIsolationTest, ViewerCannotListCrossTenantUsers) {
    auto r = ctrl_->listUsers(viewer_a_ctx_, tenant_b_id_, 100, 0);
    EXPECT_EQ(r.status, 403);
}

TEST_F(TenantIsolationTest, ViewerCannotListCrossTenantKeys) {
    auto r = ctrl_->listApiKeys(viewer_a_ctx_, tenant_b_id_, 100, 0);
    EXPECT_EQ(r.status, 403);
}

TEST_F(TenantIsolationTest, SuspendedTenantRejectsAuth) {
    ctrl_->updateTenant(super_ctx_, tenant_a_id_, {{"status", "suspended"}});
    EXPECT_FALSE(auth_svc_->resolve(viewer_a_key_).has_value());
}

TEST_F(TenantIsolationTest, DisabledUserRejectsAuth) {
    auto user = store_.getUserByUsername(tenant_a_id_, "viewer-a");
    ASSERT_TRUE(user.has_value());
    User u = *user;
    u.status = "disabled";
    store_.updateUser(u);
    EXPECT_FALSE(auth_svc_->resolve(viewer_a_key_).has_value());
}
