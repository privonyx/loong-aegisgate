#include "storage/sqlite_persistent_store.h"
#include "auth/crypto_utils.h"
#include <gtest/gtest.h>
#include <filesystem>

using namespace aegisgate;

class SqliteRbacTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = "test_sqlite_rbac_" + std::to_string(getpid()) + ".db";
        store_ = std::make_unique<SQLitePersistentStore>(db_path_);
        ASSERT_TRUE(store_->initialize());
    }
    void TearDown() override {
        store_->close();
        std::filesystem::remove(db_path_);
    }

    std::string db_path_;
    std::unique_ptr<SQLitePersistentStore> store_;
};

// --- Tenant CRUD ---

TEST_F(SqliteRbacTest, TenantInsertAndGet) {
    Tenant t;
    t.id = "t1"; t.name = "Acme"; t.status = "active";
    t.model_whitelist = {"gpt-4", "claude-3"};
    t.daily_cost_limit = 10.0; t.monthly_cost_limit = 100.0;
    ASSERT_TRUE(store_->insertTenant(t));

    auto got = store_->getTenant("t1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "Acme");
    EXPECT_EQ(got->model_whitelist.size(), 2u);
    EXPECT_EQ(got->model_whitelist[0], "gpt-4");
    EXPECT_DOUBLE_EQ(got->daily_cost_limit, 10.0);
}

TEST_F(SqliteRbacTest, TenantInsertDuplicate) {
    Tenant t; t.id = "t1"; t.name = "A";
    ASSERT_TRUE(store_->insertTenant(t));
    EXPECT_FALSE(store_->insertTenant(t));
}

TEST_F(SqliteRbacTest, TenantUpdateAndList) {
    Tenant t; t.id = "t1"; t.name = "Old"; t.created_at = "2026-01-01";
    store_->insertTenant(t);
    t.name = "New"; t.updated_at = "2026-03-22";
    ASSERT_TRUE(store_->updateTenant(t));

    auto got = store_->getTenant("t1");
    EXPECT_EQ(got->name, "New");

    Tenant t2; t2.id = "t2"; t2.name = "Two"; t2.created_at = "2026-01-02";
    store_->insertTenant(t2);
    auto all = store_->listTenants();
    EXPECT_EQ(all.size(), 2u);
}

TEST_F(SqliteRbacTest, TenantDelete) {
    Tenant t; t.id = "t1"; t.name = "A";
    store_->insertTenant(t);
    ASSERT_TRUE(store_->deleteTenant("t1"));
    EXPECT_FALSE(store_->getTenant("t1").has_value());
    EXPECT_FALSE(store_->deleteTenant("t1"));
}

// --- User CRUD ---

TEST_F(SqliteRbacTest, UserInsertAndGet) {
    Tenant t; t.id = "t1"; t.name = "T";
    store_->insertTenant(t);

    User u; u.id = "u1"; u.tenant_id = "t1"; u.username = "alice"; u.role = Role::Developer;
    ASSERT_TRUE(store_->insertUser(u));

    auto got = store_->getUser("u1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->username, "alice");
    EXPECT_EQ(got->role, Role::Developer);
}

TEST_F(SqliteRbacTest, UserDuplicateUsernameSameTenant) {
    Tenant t; t.id = "t1"; t.name = "T";
    store_->insertTenant(t);

    User u1; u1.id = "u1"; u1.tenant_id = "t1"; u1.username = "alice";
    User u2; u2.id = "u2"; u2.tenant_id = "t1"; u2.username = "alice";
    ASSERT_TRUE(store_->insertUser(u1));
    EXPECT_FALSE(store_->insertUser(u2));
}

TEST_F(SqliteRbacTest, UserSameUsernameDifferentTenant) {
    Tenant t1; t1.id = "t1"; t1.name = "T1";
    Tenant t2; t2.id = "t2"; t2.name = "T2";
    store_->insertTenant(t1);
    store_->insertTenant(t2);

    User u1; u1.id = "u1"; u1.tenant_id = "t1"; u1.username = "alice";
    User u2; u2.id = "u2"; u2.tenant_id = "t2"; u2.username = "alice";
    ASSERT_TRUE(store_->insertUser(u1));
    ASSERT_TRUE(store_->insertUser(u2));
}

TEST_F(SqliteRbacTest, UserGetByUsername) {
    Tenant t; t.id = "t1"; t.name = "T";
    store_->insertTenant(t);
    User u; u.id = "u1"; u.tenant_id = "t1"; u.username = "bob";
    store_->insertUser(u);

    auto got = store_->getUserByUsername("t1", "bob");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id, "u1");
    EXPECT_FALSE(store_->getUserByUsername("t2", "bob").has_value());
}

TEST_F(SqliteRbacTest, UserListFiltersByTenant) {
    Tenant t1; t1.id = "t1"; t1.name = "T1";
    Tenant t2; t2.id = "t2"; t2.name = "T2";
    store_->insertTenant(t1); store_->insertTenant(t2);

    User u1; u1.id = "u1"; u1.tenant_id = "t1"; u1.username = "a";
    User u2; u2.id = "u2"; u2.tenant_id = "t2"; u2.username = "b";
    store_->insertUser(u1); store_->insertUser(u2);

    EXPECT_EQ(store_->listUsers("t1").size(), 1u);
    EXPECT_EQ(store_->listUsers("t2").size(), 1u);
    EXPECT_EQ(store_->listUsers("").size(), 2u);
}

// --- API Key CRUD ---

TEST_F(SqliteRbacTest, ApiKeyInsertAndGetByHash) {
    Tenant t; t.id = "t1"; t.name = "T"; store_->insertTenant(t);
    User u; u.id = "u1"; u.tenant_id = "t1"; u.username = "a"; store_->insertUser(u);

    auto raw = auth::generateApiKey();
    ApiKeyRecord k;
    k.id = "k1"; k.user_id = "u1"; k.tenant_id = "t1";
    k.key_prefix = auth::extractKeyPrefix(raw);
    k.key_hash = auth::hashApiKey(raw);
    k.role = Role::Developer;
    ASSERT_TRUE(store_->insertApiKey(k));

    auto got = store_->getApiKeyByHash(k.key_hash);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id, "k1");
    EXPECT_EQ(got->role, Role::Developer);
}

TEST_F(SqliteRbacTest, ApiKeyGetByPrefix) {
    Tenant t; t.id = "t1"; t.name = "T"; store_->insertTenant(t);
    User u; u.id = "u1"; u.tenant_id = "t1"; u.username = "a"; store_->insertUser(u);

    ApiKeyRecord k; k.id = "k1"; k.user_id = "u1"; k.tenant_id = "t1";
    k.key_prefix = "sk-abcde"; k.key_hash = "hash1";
    store_->insertApiKey(k);
    auto results = store_->getApiKeysByPrefix("sk-abcde");
    EXPECT_EQ(results.size(), 1u);
    EXPECT_TRUE(store_->getApiKeysByPrefix("sk-zzzzz").empty());
}

TEST_F(SqliteRbacTest, ApiKeyRevoke) {
    Tenant t; t.id = "t1"; t.name = "T"; store_->insertTenant(t);
    User u; u.id = "u1"; u.tenant_id = "t1"; u.username = "a"; store_->insertUser(u);

    ApiKeyRecord k; k.id = "k1"; k.user_id = "u1"; k.tenant_id = "t1";
    k.key_prefix = "p"; k.key_hash = "h"; k.status = "active";
    store_->insertApiKey(k);
    ASSERT_TRUE(store_->revokeApiKey("k1"));
    auto got = store_->getApiKeyByHash("h");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, "revoked");
}

// --- Cross-tenant isolation ---

TEST_F(SqliteRbacTest, CrossTenantIsolation) {
    Tenant t1; t1.id = "t1"; t1.name = "T1"; store_->insertTenant(t1);
    Tenant t2; t2.id = "t2"; t2.name = "T2"; store_->insertTenant(t2);

    User u1; u1.id = "u1"; u1.tenant_id = "t1"; u1.username = "a"; store_->insertUser(u1);
    User u2; u2.id = "u2"; u2.tenant_id = "t2"; u2.username = "b"; store_->insertUser(u2);

    ApiKeyRecord k1; k1.id = "k1"; k1.user_id = "u1"; k1.tenant_id = "t1";
    k1.key_prefix = "p1"; k1.key_hash = "h1";
    ApiKeyRecord k2; k2.id = "k2"; k2.user_id = "u2"; k2.tenant_id = "t2";
    k2.key_prefix = "p2"; k2.key_hash = "h2";
    store_->insertApiKey(k1); store_->insertApiKey(k2);

    EXPECT_EQ(store_->listUsers("t1").size(), 1u);
    EXPECT_EQ(store_->listApiKeys("t1").size(), 1u);
    EXPECT_EQ(store_->listUsers("t2").size(), 1u);
    EXPECT_EQ(store_->listApiKeys("t2").size(), 1u);
}

// --- Whitelist JSON round-trip with special characters ---

TEST_F(SqliteRbacTest, WhitelistSpecialCharsRoundTrip) {
    Tenant t;
    t.id = "t-special"; t.name = "Special"; t.status = "active";
    t.model_whitelist = {"gpt-4", "model/with-slash", "model\"quoted", "模型中文"};
    ASSERT_TRUE(store_->insertTenant(t));

    auto got = store_->getTenant("t-special");
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(got->model_whitelist.size(), 4u);
    EXPECT_EQ(got->model_whitelist[0], "gpt-4");
    EXPECT_EQ(got->model_whitelist[1], "model/with-slash");
    EXPECT_EQ(got->model_whitelist[2], "model\"quoted");
    EXPECT_EQ(got->model_whitelist[3], "模型中文");
}

TEST_F(SqliteRbacTest, WhitelistEmptyRoundTrip) {
    Tenant t;
    t.id = "t-empty"; t.name = "Empty"; t.status = "active";
    t.model_whitelist = {};
    ASSERT_TRUE(store_->insertTenant(t));

    auto got = store_->getTenant("t-empty");
    ASSERT_TRUE(got.has_value());
    EXPECT_TRUE(got->model_whitelist.empty());
}

// --- Tenant cost aggregation ---

TEST_F(SqliteRbacTest, TenantCostInPeriod) {
    CostRecord c1;
    c1.request_id = "r1"; c1.tenant_id = "t1"; c1.model = "gpt-4";
    c1.total_cost = 0.05; c1.timestamp = "2026-03-22T10:00:00Z";
    CostRecord c2;
    c2.request_id = "r2"; c2.tenant_id = "t1"; c2.model = "gpt-4";
    c2.total_cost = 0.10; c2.timestamp = "2026-03-22T12:00:00Z";
    store_->insertCostRecord(c1);
    store_->insertCostRecord(c2);

    double cost = store_->getTenantCostInPeriod("t1", "2026-03-22T00:00:00Z", "2026-03-23T00:00:00Z");
    EXPECT_NEAR(cost, 0.15, 0.001);
}

// --- Pagination total counts (TASK-20260604-01 P0-E/D1=A) ---

TEST_F(SqliteRbacTest, PaginationTotalCounts) {
    Tenant t1; t1.id = "t1"; t1.name = "T1"; store_->insertTenant(t1);
    Tenant t2; t2.id = "t2"; t2.name = "T2"; store_->insertTenant(t2);
    EXPECT_EQ(store_->tenantCount(), 2);

    User u1; u1.id = "u1"; u1.tenant_id = "t1"; u1.username = "a"; store_->insertUser(u1);
    User u2; u2.id = "u2"; u2.tenant_id = "t1"; u2.username = "b"; store_->insertUser(u2);
    User u3; u3.id = "u3"; u3.tenant_id = "t2"; u3.username = "c"; store_->insertUser(u3);
    EXPECT_EQ(store_->userCount("t1"), 2);
    EXPECT_EQ(store_->userCount("t2"), 1);
    EXPECT_EQ(store_->userCount(""), 3);  // 空 = 全局（SuperAdmin 视角）

    ApiKeyRecord k1; k1.id = "k1"; k1.user_id = "u1"; k1.tenant_id = "t1";
    k1.key_prefix = "p1"; k1.key_hash = "h1"; store_->insertApiKey(k1);
    ApiKeyRecord k2; k2.id = "k2"; k2.user_id = "u3"; k2.tenant_id = "t2";
    k2.key_prefix = "p2"; k2.key_hash = "h2"; store_->insertApiKey(k2);
    EXPECT_EQ(store_->apiKeyCount("t1"), 1);
    EXPECT_EQ(store_->apiKeyCount("t2"), 1);
    EXPECT_EQ(store_->apiKeyCount(""), 2);

    PersistentStore::PromptTemplateRecord tpl;
    tpl.id = "tpl1"; tpl.tenant_id = "t1"; tpl.name = "greet";
    tpl.content = "hi"; tpl.created_at = "2026-06-04T00:00:00Z";
    store_->insertPromptTemplate(tpl);
    tpl.id = "tpl2"; tpl.name = "bye"; store_->insertPromptTemplate(tpl);
    EXPECT_EQ(store_->promptTemplateCount("t1"), 2);
    EXPECT_EQ(store_->promptTemplateCount("t2"), 0);

    // TASK-20260605-01：ruleSetCount + listRuleSetVersions offset 翻页。
    PersistentStore::RuleSetRecord rs;
    rs.tenant_id = "t1"; rs.rules_json = "[]"; rs.created_at = "2026-06-04T00:00:00Z";
    rs.is_active = true;
    rs.version = 1; store_->insertRuleSet("t1", rs);
    rs.version = 2; store_->insertRuleSet("t1", rs);
    rs.version = 3; store_->insertRuleSet("t1", rs);
    EXPECT_EQ(store_->ruleSetCount("t1"), 3);
    EXPECT_EQ(store_->ruleSetCount("t2"), 0);
    // 版本倒序（3,2,1）；offset=1 limit=1 → 第二新版本 v2。
    auto page = store_->listRuleSetVersions("t1", 1, 1);
    ASSERT_EQ(page.size(), 1u);
    EXPECT_EQ(page[0].version, 2);
}

// --- SQL injection prevention ---

TEST_F(SqliteRbacTest, SqlInjectionPrevention) {
    Tenant t;
    t.id = "t1'; DROP TABLE tenants; --"; t.name = "Evil";
    ASSERT_TRUE(store_->insertTenant(t));
    auto got = store_->getTenant("t1'; DROP TABLE tenants; --");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "Evil");

    Tenant t2; t2.id = "safe"; t2.name = "Safe";
    ASSERT_TRUE(store_->insertTenant(t2));
}
