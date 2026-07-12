#include "storage/memory_persistent_store.h"
#include <gtest/gtest.h>

using namespace aegisgate;

class MemoryPersistentStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryPersistentStore>();
        store_->initialize();
    }
    std::unique_ptr<MemoryPersistentStore> store_;
};

// TASK-20260702-02 P2-2（SR-2）：MFA 失败锁定（memory 后端，三后端对称）。
TEST_F(MemoryPersistentStoreTest, MfaFailureLockoutAndClear) {
    int64_t now = static_cast<int64_t>(::time(nullptr));
    EXPECT_FALSE(store_->recordMfaFailure("u1", 3, 900));
    EXPECT_FALSE(store_->recordMfaFailure("u1", 3, 900));
    EXPECT_TRUE(store_->recordMfaFailure("u1", 3, 900));
    EXPECT_GT(store_->getMfaLockedUntil("u1"), now);
    EXPECT_TRUE(store_->clearMfaFailures("u1"));
    EXPECT_EQ(store_->getMfaLockedUntil("u1"), 0);
}

TEST_F(MemoryPersistentStoreTest, MfaFailureIsolatedPerUser) {
    store_->recordMfaFailure("a", 2, 900);
    store_->recordMfaFailure("a", 2, 900);  // a locked
    EXPECT_GT(store_->getMfaLockedUntil("a"), 0);
    EXPECT_EQ(store_->getMfaLockedUntil("b"), 0);  // b unaffected
}

static AuditEntry makeAudit(const std::string& req_id,
                             const std::string& tenant = "t1") {
    AuditEntry e;
    e.request_id = req_id;
    e.timestamp = "2026-03-19T00:00:00Z";
    e.tenant_id = tenant;
    e.action = "request_received";
    e.stage_name = "pipeline";
    e.detail = "test detail";
    return e;
}

static CostRecord makeCost(const std::string& req_id,
                            const std::string& model = "gpt-4") {
    CostRecord r;
    r.request_id = req_id;
    r.tenant_id = "t1";
    r.app_id = "app1";
    r.model = model;
    r.input_tokens = 100;
    r.output_tokens = 50;
    r.input_cost = 0.003;
    r.output_cost = 0.006;
    r.total_cost = 0.009;
    r.timestamp = "2026-03-19T00:00:00Z";
    return r;
}

TEST_F(MemoryPersistentStoreTest, BackendName) {
    EXPECT_EQ(store_->backendName(), "memory");
}

TEST_F(MemoryPersistentStoreTest, InitializeAndHealth) {
    MemoryPersistentStore fresh;
    EXPECT_FALSE(fresh.isHealthy());
    EXPECT_TRUE(fresh.initialize());
    EXPECT_TRUE(fresh.isHealthy());
}

TEST_F(MemoryPersistentStoreTest, InsertAndQueryAudit) {
    EXPECT_TRUE(store_->insertAudit(makeAudit("r1")));
    auto results = store_->queryAudits();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].request_id, "r1");
    EXPECT_EQ(results[0].tenant_id, "t1");
}

TEST_F(MemoryPersistentStoreTest, AuditFilterByTenant) {
    store_->insertAudit(makeAudit("r1", "tenant-a"));
    store_->insertAudit(makeAudit("r2", "tenant-b"));
    store_->insertAudit(makeAudit("r3", "tenant-a"));

    auto a = store_->queryAudits("tenant-a");
    EXPECT_EQ(a.size(), 2u);
    auto b = store_->queryAudits("tenant-b");
    EXPECT_EQ(b.size(), 1u);
}

// TASK-20260703-02 Epic 6.2 / C11 ② — 排序一致性（三后端统一 newest-first / DESC）。
// 根因：PG queryAudits/queryCosts 均 ORDER BY id DESC；SQLite audits DESC 但 costs ASC；
// Memory 两者均按插入序（ASC）。同一查询跨后端返回顺序不一致 → 分页/展示错乱。
// 目标：统一为 DESC（最新在前，对齐 PG 两者 + SQLite audits）。
TEST_F(MemoryPersistentStoreTest, AuditPagination) {
    for (int i = 0; i < 10; ++i)
        store_->insertAudit(makeAudit("r" + std::to_string(i)));

    // C11 ②：DESC → 最新（r9）在首页首位。
    auto page1 = store_->queryAudits("", 3, 0);
    EXPECT_EQ(page1.size(), 3u);
    EXPECT_EQ(page1[0].request_id, "r9");

    auto page2 = store_->queryAudits("", 3, 3);
    EXPECT_EQ(page2.size(), 3u);
    EXPECT_EQ(page2[0].request_id, "r6");
}

TEST_F(MemoryPersistentStoreTest, QueryAuditsNewestFirst) {
    store_->insertAudit(makeAudit("r0"));
    store_->insertAudit(makeAudit("r1"));
    store_->insertAudit(makeAudit("r2"));
    auto results = store_->queryAudits();
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].request_id, "r2");  // 最新在前（DESC）
    EXPECT_EQ(results[2].request_id, "r0");
}

TEST_F(MemoryPersistentStoreTest, QueryCostsNewestFirst) {
    store_->insertCostRecord(makeCost("r0"));
    store_->insertCostRecord(makeCost("r1"));
    store_->insertCostRecord(makeCost("r2"));
    auto results = store_->queryCosts();
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].request_id, "r2");  // 最新在前（DESC）
    EXPECT_EQ(results[2].request_id, "r0");
}

TEST_F(MemoryPersistentStoreTest, AuditCount) {
    store_->insertAudit(makeAudit("r1", "tenant-a"));
    store_->insertAudit(makeAudit("r2", "tenant-b"));
    store_->insertAudit(makeAudit("r3", "tenant-a"));

    EXPECT_EQ(store_->auditCount(), 3);
    EXPECT_EQ(store_->auditCount("tenant-a"), 2);
    EXPECT_EQ(store_->auditCount("tenant-c"), 0);
}

TEST_F(MemoryPersistentStoreTest, InsertAndQueryCost) {
    EXPECT_TRUE(store_->insertCostRecord(makeCost("r1")));
    auto results = store_->queryCosts();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].request_id, "r1");
    EXPECT_EQ(results[0].model, "gpt-4");
    EXPECT_DOUBLE_EQ(results[0].total_cost, 0.009);
}

TEST_F(MemoryPersistentStoreTest, CostFilterByModel) {
    store_->insertCostRecord(makeCost("r1", "gpt-4"));
    store_->insertCostRecord(makeCost("r2", "claude-3"));
    store_->insertCostRecord(makeCost("r3", "gpt-4"));

    auto gpt = store_->queryCosts("gpt-4");
    EXPECT_EQ(gpt.size(), 2u);
    auto claude = store_->queryCosts("claude-3");
    EXPECT_EQ(claude.size(), 1u);
}

TEST_F(MemoryPersistentStoreTest, CostPagination) {
    for (int i = 0; i < 10; ++i)
        store_->insertCostRecord(makeCost("r" + std::to_string(i)));

    auto page1 = store_->queryCosts("", 3, 0);
    EXPECT_EQ(page1.size(), 3u);
    auto page2 = store_->queryCosts("", 3, 7);
    EXPECT_EQ(page2.size(), 3u);
}

TEST_F(MemoryPersistentStoreTest, CostRecordCount) {
    store_->insertCostRecord(makeCost("r1", "gpt-4"));
    store_->insertCostRecord(makeCost("r2", "claude-3"));

    EXPECT_EQ(store_->costRecordCount(), 2);
    EXPECT_EQ(store_->costRecordCount("gpt-4"), 1);
    EXPECT_EQ(store_->costRecordCount("nonexistent"), 0);
}

// TASK-20260604-01 P0-E/D1=A — cost tenant 过滤 + 分页总数（Memory 后端）。
TEST_F(MemoryPersistentStoreTest, CostFilterByTenantAndCounts) {
    auto c = [](const std::string& req, const std::string& tenant,
                const std::string& model) {
        CostRecord r;
        r.request_id = req; r.tenant_id = tenant; r.model = model;
        r.total_cost = 0.009; r.timestamp = "2026-03-19T00:00:00Z";
        return r;
    };
    store_->insertCostRecord(c("r1", "ta", "gpt-4"));
    store_->insertCostRecord(c("r2", "tb", "gpt-4"));
    store_->insertCostRecord(c("r3", "ta", "claude-3"));

    EXPECT_EQ(store_->queryCosts("", 100, 0, "ta").size(), 2u);
    EXPECT_EQ(store_->queryCosts("", 100, 0, "tb").size(), 1u);
    EXPECT_EQ(store_->queryCosts("gpt-4", 100, 0, "ta").size(), 1u);
    EXPECT_EQ(store_->queryCosts("", 100, 0, "").size(), 3u);
    EXPECT_EQ(store_->costRecordCount("", "ta"), 2);
    EXPECT_EQ(store_->costRecordCount("gpt-4", "ta"), 1);
    EXPECT_EQ(store_->costRecordCount(), 3);
}

TEST_F(MemoryPersistentStoreTest, PaginationTotalCounts) {
    Tenant t1; t1.id = "t1"; t1.name = "T1"; store_->insertTenant(t1);
    Tenant t2; t2.id = "t2"; t2.name = "T2"; store_->insertTenant(t2);
    EXPECT_EQ(store_->tenantCount(), 2);

    User u1; u1.id = "u1"; u1.tenant_id = "t1"; u1.username = "a"; store_->insertUser(u1);
    User u2; u2.id = "u2"; u2.tenant_id = "t2"; u2.username = "b"; store_->insertUser(u2);
    EXPECT_EQ(store_->userCount("t1"), 1);
    EXPECT_EQ(store_->userCount(""), 2);

    ApiKeyRecord k1; k1.id = "k1"; k1.user_id = "u1"; k1.tenant_id = "t1";
    k1.key_prefix = "p1"; k1.key_hash = "h1"; store_->insertApiKey(k1);
    EXPECT_EQ(store_->apiKeyCount("t1"), 1);
    EXPECT_EQ(store_->apiKeyCount("t2"), 0);

    PersistentStore::PromptTemplateRecord tpl;
    tpl.id = "tpl1"; tpl.tenant_id = "t1"; tpl.name = "greet"; tpl.content = "hi";
    store_->insertPromptTemplate(tpl);
    EXPECT_EQ(store_->promptTemplateCount("t1"), 1);
    EXPECT_EQ(store_->promptTemplateCount("t2"), 0);
}

// ===========================================================================
// TASK-20260703-02 Epic 6.3 / C11 ③ — Memory 后端补全 4 类持久化实现（D3=A）。
// 根因：SSO Provider / Identity Mapping / Session / SCIM Token 在 Memory 后端未
// override → 落入 PersistentStore 基类 no-op 默认（返回 false/nullopt/空），
// 与 SQLite/PG 语义不对齐。以 memory 后端跑 SSO 登录/会话/SCIM 时静默失效。
// 语义严格镜像 SQLite（getIdentityMapping 按 subject+issuer / listSessionsByUser
// 按 created_at DESC / deleteExpiredSessions 按 ISO expires_at / getScimTokenByHash
// 按 token_hash）。
// ===========================================================================

TEST_F(MemoryPersistentStoreTest, SsoProviderCrud) {
    SsoProvider p;
    p.id = "sso1"; p.tenant_id = "t1"; p.name = "okta"; p.enabled = true;
    EXPECT_TRUE(store_->insertSsoProvider(p));
    EXPECT_FALSE(store_->insertSsoProvider(p));  // 重复 id → false（镜像 PK 冲突）

    auto got = store_->getSsoProvider("sso1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "okta");

    auto by_tenant = store_->getSsoProviderByTenant("t1");
    ASSERT_TRUE(by_tenant.has_value());
    EXPECT_EQ(by_tenant->id, "sso1");

    EXPECT_EQ(store_->ssoProviderCount(), 1);
    EXPECT_EQ(store_->listSsoProviders(100, 0).size(), 1u);

    p.name = "okta2";
    EXPECT_TRUE(store_->updateSsoProvider(p));
    EXPECT_EQ(store_->getSsoProvider("sso1")->name, "okta2");

    // 禁用后 getSsoProviderByTenant 不再返回（enabled=1 语义）。
    p.enabled = false;
    EXPECT_TRUE(store_->updateSsoProvider(p));
    EXPECT_FALSE(store_->getSsoProviderByTenant("t1").has_value());

    EXPECT_TRUE(store_->deleteSsoProvider("sso1"));
    EXPECT_FALSE(store_->getSsoProvider("sso1").has_value());
    EXPECT_FALSE(store_->deleteSsoProvider("sso1"));
}

TEST_F(MemoryPersistentStoreTest, IdentityMappingCrud) {
    IdentityMapping m;
    m.id = "im1"; m.tenant_id = "t1"; m.external_subject = "sub1";
    m.external_issuer = "iss1"; m.user_id = "u1"; m.email = "a@x.io";
    m.last_login_at = "2026-03-19T00:00:00Z";
    EXPECT_TRUE(store_->insertIdentityMapping(m));

    auto got = store_->getIdentityMapping("sub1", "iss1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->user_id, "u1");
    EXPECT_FALSE(store_->getIdentityMapping("sub1", "other").has_value());

    EXPECT_EQ(store_->listIdentityMappings("t1", 100, 0).size(), 1u);
    EXPECT_EQ(store_->listIdentityMappings("t2", 100, 0).size(), 0u);

    EXPECT_TRUE(store_->updateIdentityMappingLastLogin("im1", "2026-04-01T00:00:00Z"));
    EXPECT_EQ(store_->getIdentityMapping("sub1", "iss1")->last_login_at,
              "2026-04-01T00:00:00Z");
    EXPECT_FALSE(store_->updateIdentityMappingLastLogin("nope", "x"));

    EXPECT_TRUE(store_->deleteIdentityMapping("im1"));
    EXPECT_FALSE(store_->getIdentityMapping("sub1", "iss1").has_value());
}

TEST_F(MemoryPersistentStoreTest, SessionCrud) {
    auto mk = [](const std::string& id, const std::string& user,
                 const std::string& created, const std::string& expires) {
        Session s;
        s.id = id; s.user_id = user; s.tenant_id = "t1"; s.auth_method = "sso";
        s.created_at = created; s.last_active_at = created; s.expires_at = expires;
        return s;
    };
    EXPECT_TRUE(store_->insertSession(mk("s1", "u1", "2026-03-24T00:00:00Z",
                                          "2099-01-01T00:00:00Z")));
    EXPECT_TRUE(store_->insertSession(mk("s2", "u1", "2026-03-25T00:00:00Z",
                                          "2099-01-01T00:00:00Z")));

    auto got = store_->getSession("s1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->user_id, "u1");

    // listSessionsByUser 按 created_at DESC（最新在前，镜像 SQLite）。
    auto list = store_->listSessionsByUser("u1");
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].id, "s2");
    EXPECT_EQ(store_->countSessionsByUser("u1"), 2);

    EXPECT_TRUE(store_->updateSessionActivity("s1", "2026-03-26T00:00:00Z"));
    EXPECT_EQ(store_->getSession("s1")->last_active_at, "2026-03-26T00:00:00Z");

    EXPECT_FALSE(store_->getSession("s1")->mfa_verified);
    EXPECT_TRUE(store_->updateSessionMfaVerified("s1", true));
    EXPECT_TRUE(store_->getSession("s1")->mfa_verified);

    EXPECT_TRUE(store_->deleteSession("s2"));
    EXPECT_EQ(store_->countSessionsByUser("u1"), 1);
}

TEST_F(MemoryPersistentStoreTest, SessionDeleteExpired) {
    auto mk = [](const std::string& id, const std::string& expires) {
        Session s;
        s.id = id; s.user_id = "u1"; s.tenant_id = "t1"; s.auth_method = "sso";
        s.created_at = "2026-03-24T00:00:00Z"; s.last_active_at = "2026-03-24T00:00:00Z";
        s.expires_at = expires;
        return s;
    };
    EXPECT_TRUE(store_->insertSession(mk("expired", "2020-01-01T00:00:00Z")));
    EXPECT_TRUE(store_->insertSession(mk("valid", "2099-01-01T00:00:00Z")));

    EXPECT_EQ(store_->deleteExpiredSessions(), 1);
    EXPECT_FALSE(store_->getSession("expired").has_value());
    EXPECT_TRUE(store_->getSession("valid").has_value());
}

TEST_F(MemoryPersistentStoreTest, ScimTokenCrud) {
    ScimToken t;
    t.id = "tok1"; t.tenant_id = "t1"; t.token_hash = "h-abc";
    t.description = "ci"; t.created_at = "2026-03-19T00:00:00Z";
    t.expires_at = "2099-01-01T00:00:00Z";
    EXPECT_TRUE(store_->insertScimToken(t));

    auto got = store_->getScimTokenByHash("h-abc");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id, "tok1");
    EXPECT_FALSE(store_->getScimTokenByHash("nope").has_value());

    EXPECT_EQ(store_->listScimTokens("t1").size(), 1u);
    EXPECT_EQ(store_->listScimTokens("t2").size(), 0u);

    EXPECT_TRUE(store_->deleteScimToken("tok1"));
    EXPECT_FALSE(store_->getScimTokenByHash("h-abc").has_value());
    EXPECT_FALSE(store_->deleteScimToken("tok1"));
}
