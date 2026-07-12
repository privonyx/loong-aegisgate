#include "storage/memory_persistent_store.h"
#include "auth/crypto_utils.h"
#include <gtest/gtest.h>

using namespace aegisgate;

class MemoryRbacTest : public ::testing::Test {
protected:
    void SetUp() override { store_.initialize(); }
    MemoryPersistentStore store_;
};

// --- Tenant CRUD ---

TEST_F(MemoryRbacTest, TenantInsertAndGet) {
    Tenant t;
    t.id = "t1"; t.name = "Acme"; t.status = "active";
    ASSERT_TRUE(store_.insertTenant(t));
    auto got = store_.getTenant("t1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "Acme");
    EXPECT_EQ(got->status, "active");
}

TEST_F(MemoryRbacTest, TenantInsertDuplicateFails) {
    Tenant t; t.id = "t1"; t.name = "A";
    ASSERT_TRUE(store_.insertTenant(t));
    EXPECT_FALSE(store_.insertTenant(t));
}

TEST_F(MemoryRbacTest, TenantGetNotFound) {
    EXPECT_FALSE(store_.getTenant("nonexistent").has_value());
}

TEST_F(MemoryRbacTest, TenantUpdateAndList) {
    Tenant t; t.id = "t1"; t.name = "Old";
    store_.insertTenant(t);
    t.name = "New";
    ASSERT_TRUE(store_.updateTenant(t));
    auto got = store_.getTenant("t1");
    EXPECT_EQ(got->name, "New");

    Tenant t2; t2.id = "t2"; t2.name = "Two";
    store_.insertTenant(t2);
    auto all = store_.listTenants();
    EXPECT_EQ(all.size(), 2u);
}

TEST_F(MemoryRbacTest, TenantDelete) {
    Tenant t; t.id = "t1"; t.name = "A";
    store_.insertTenant(t);
    ASSERT_TRUE(store_.deleteTenant("t1"));
    EXPECT_FALSE(store_.getTenant("t1").has_value());
    EXPECT_FALSE(store_.deleteTenant("t1"));
}

// --- User CRUD ---

TEST_F(MemoryRbacTest, UserInsertAndGet) {
    User u;
    u.id = "u1"; u.tenant_id = "t1"; u.username = "alice"; u.role = Role::Developer;
    ASSERT_TRUE(store_.insertUser(u));
    auto got = store_.getUser("u1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->username, "alice");
    EXPECT_EQ(got->role, Role::Developer);
}

TEST_F(MemoryRbacTest, UserDuplicateUsernameSameTenantFails) {
    User u1; u1.id = "u1"; u1.tenant_id = "t1"; u1.username = "alice";
    User u2; u2.id = "u2"; u2.tenant_id = "t1"; u2.username = "alice";
    ASSERT_TRUE(store_.insertUser(u1));
    EXPECT_FALSE(store_.insertUser(u2));
}

TEST_F(MemoryRbacTest, UserSameUsernameDifferentTenantOk) {
    User u1; u1.id = "u1"; u1.tenant_id = "t1"; u1.username = "alice";
    User u2; u2.id = "u2"; u2.tenant_id = "t2"; u2.username = "alice";
    ASSERT_TRUE(store_.insertUser(u1));
    ASSERT_TRUE(store_.insertUser(u2));
}

TEST_F(MemoryRbacTest, UserGetByUsername) {
    User u; u.id = "u1"; u.tenant_id = "t1"; u.username = "bob";
    store_.insertUser(u);
    auto got = store_.getUserByUsername("t1", "bob");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id, "u1");
    EXPECT_FALSE(store_.getUserByUsername("t2", "bob").has_value());
}

TEST_F(MemoryRbacTest, UserListFiltersByTenant) {
    User u1; u1.id = "u1"; u1.tenant_id = "t1"; u1.username = "a";
    User u2; u2.id = "u2"; u2.tenant_id = "t2"; u2.username = "b";
    store_.insertUser(u1);
    store_.insertUser(u2);
    EXPECT_EQ(store_.listUsers("t1").size(), 1u);
    EXPECT_EQ(store_.listUsers("t2").size(), 1u);
    EXPECT_EQ(store_.listUsers("").size(), 2u);
}

TEST_F(MemoryRbacTest, UserUpdateAndDelete) {
    User u; u.id = "u1"; u.tenant_id = "t1"; u.username = "a"; u.role = Role::Viewer;
    store_.insertUser(u);
    u.role = Role::TenantAdmin;
    ASSERT_TRUE(store_.updateUser(u));
    EXPECT_EQ(store_.getUser("u1")->role, Role::TenantAdmin);
    ASSERT_TRUE(store_.deleteUser("u1"));
    EXPECT_FALSE(store_.getUser("u1").has_value());
}

// --- API Key CRUD ---

TEST_F(MemoryRbacTest, ApiKeyInsertAndGetByHash) {
    auto raw_key = auth::generateApiKey();
    ApiKeyRecord k;
    k.id = "k1"; k.user_id = "u1"; k.tenant_id = "t1";
    k.key_prefix = auth::extractKeyPrefix(raw_key);
    k.key_hash = auth::hashApiKey(raw_key);
    k.role = Role::Developer;
    ASSERT_TRUE(store_.insertApiKey(k));
    auto got = store_.getApiKeyByHash(k.key_hash);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id, "k1");
    EXPECT_EQ(got->role, Role::Developer);
}

TEST_F(MemoryRbacTest, ApiKeyGetByPrefix) {
    ApiKeyRecord k; k.id = "k1"; k.tenant_id = "t1";
    k.key_prefix = "sk-abcde"; k.key_hash = "hash1";
    store_.insertApiKey(k);
    auto results = store_.getApiKeysByPrefix("sk-abcde");
    EXPECT_EQ(results.size(), 1u);
    EXPECT_TRUE(store_.getApiKeysByPrefix("sk-zzzzz").empty());
}

TEST_F(MemoryRbacTest, ApiKeyListFiltersByTenant) {
    ApiKeyRecord k1; k1.id = "k1"; k1.tenant_id = "t1"; k1.key_prefix = "p1"; k1.key_hash = "h1";
    ApiKeyRecord k2; k2.id = "k2"; k2.tenant_id = "t2"; k2.key_prefix = "p2"; k2.key_hash = "h2";
    store_.insertApiKey(k1);
    store_.insertApiKey(k2);
    EXPECT_EQ(store_.listApiKeys("t1").size(), 1u);
    EXPECT_EQ(store_.listApiKeys("").size(), 2u);
}

TEST_F(MemoryRbacTest, ApiKeyRevoke) {
    ApiKeyRecord k; k.id = "k1"; k.tenant_id = "t1";
    k.key_prefix = "p"; k.key_hash = "h"; k.status = "active";
    store_.insertApiKey(k);
    ASSERT_TRUE(store_.revokeApiKey("k1"));
    auto got = store_.getApiKeyByHash("h");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, "revoked");
}

TEST_F(MemoryRbacTest, ApiKeyUpdateAndRevoke) {
    ApiKeyRecord k; k.id = "k1"; k.tenant_id = "t1";
    k.key_prefix = "p"; k.key_hash = "h"; k.name = "old";
    store_.insertApiKey(k);
    k.name = "new";
    ASSERT_TRUE(store_.updateApiKey(k));
    EXPECT_EQ(store_.getApiKeyByHash("h")->name, "new");
}

// --- Tenant cost aggregation ---

TEST_F(MemoryRbacTest, TenantCostInPeriod) {
    CostRecord c1;
    c1.request_id = "r1"; c1.tenant_id = "t1"; c1.model = "gpt-4";
    c1.total_cost = 0.05; c1.timestamp = "2026-03-22T10:00:00Z";
    CostRecord c2;
    c2.request_id = "r2"; c2.tenant_id = "t1"; c2.model = "gpt-4";
    c2.total_cost = 0.10; c2.timestamp = "2026-03-22T12:00:00Z";
    CostRecord c3;
    c3.request_id = "r3"; c3.tenant_id = "t2"; c3.model = "gpt-4";
    c3.total_cost = 1.00; c3.timestamp = "2026-03-22T11:00:00Z";

    store_.insertCostRecord(c1);
    store_.insertCostRecord(c2);
    store_.insertCostRecord(c3);

    double t1_cost = store_.getTenantCostInPeriod("t1", "2026-03-22T00:00:00Z", "2026-03-23T00:00:00Z");
    EXPECT_NEAR(t1_cost, 0.15, 0.001);

    double t2_cost = store_.getTenantCostInPeriod("t2", "2026-03-22T00:00:00Z", "2026-03-23T00:00:00Z");
    EXPECT_NEAR(t2_cost, 1.00, 0.001);

    double t1_partial = store_.getTenantCostInPeriod("t1", "2026-03-22T11:00:00Z", "2026-03-23T00:00:00Z");
    EXPECT_NEAR(t1_partial, 0.10, 0.001);
}
