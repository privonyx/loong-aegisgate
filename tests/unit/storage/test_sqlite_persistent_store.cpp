#include "storage/sqlite_persistent_store.h"
#include <gtest/gtest.h>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <unistd.h>

using namespace aegisgate;

class SQLitePersistentStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<SQLitePersistentStore>(":memory:");
        ASSERT_TRUE(store_->initialize());
    }
    void TearDown() override {
        store_->close();
    }
    std::unique_ptr<SQLitePersistentStore> store_;
};

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

// TASK-20260703-02 Epic 6.1 / C11 ①：生成 (now - seconds_ago) 的 ISO8601 时间戳，
// 与 currentTimestamp()/nowIso() 的 "%Y-%m-%dT%H:%M:%SZ" 写入格式一致。
static std::string isoAgo(int64_t seconds_ago) {
    auto tp = std::chrono::system_clock::now() -
              std::chrono::seconds(seconds_ago);
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

TEST_F(SQLitePersistentStoreTest, BackendName) {
    EXPECT_EQ(store_->backendName(), "sqlite");
}

// ---------------------------------------------------------------------------
// TASK-20260703-02 Epic 6.1 / C11 ① — prune 时间格式一致性（合规删除）。
// 根因：timestamp 写入为 ISO8601 "...THH:MM:SSZ"（含 'T'/'Z'），而 prune 比较用
// datetime('now','-N days')（空格分隔无 T/Z）→ 同一 UTC 日历日边界处 'T'(0x54)
// > ' '(0x20) 字符串错位 → 应删的记录被保留约 1 天（超期留存 = 合规 bug）。
// 说明：若当前 UTC 时刻处于 00:00–01:00（now-7d-1h 跨午夜到前一日），该边界不触发；
// 非该窗口时可靠复现。
// ---------------------------------------------------------------------------

TEST_F(SQLitePersistentStoreTest, PruneAuditsRespectsIsoBoundary) {
    auto old_a = makeAudit("old");
    old_a.timestamp = isoAgo(7 * 86400 + 3600);  // 7 天 1 小时前 → 应删
    ASSERT_TRUE(store_->insertAudit(old_a));
    auto fresh_a = makeAudit("fresh");
    fresh_a.timestamp = isoAgo(3600);            // 1 小时前 → 保留
    ASSERT_TRUE(store_->insertAudit(fresh_a));

    auto deleted = store_->pruneAudits(7);
    EXPECT_EQ(deleted, 1) << "ISO 边界记录未被正确删除（datetime vs ISO 错位）";
}

TEST_F(SQLitePersistentStoreTest, PruneCostRecordsRespectsIsoBoundary) {
    auto old_c = makeCost("old");
    old_c.timestamp = isoAgo(7 * 86400 + 3600);
    ASSERT_TRUE(store_->insertCostRecord(old_c));
    auto fresh_c = makeCost("fresh");
    fresh_c.timestamp = isoAgo(3600);
    ASSERT_TRUE(store_->insertCostRecord(fresh_c));

    auto deleted = store_->pruneCostRecords(7);
    EXPECT_EQ(deleted, 1) << "ISO 边界记录未被正确删除（datetime vs ISO 错位）";
}

TEST_F(SQLitePersistentStoreTest, IsHealthy) {
    EXPECT_TRUE(store_->isHealthy());
}

TEST_F(SQLitePersistentStoreTest, InsertAndQueryAudit) {
    EXPECT_TRUE(store_->insertAudit(makeAudit("r1")));
    auto results = store_->queryAudits();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].request_id, "r1");
    EXPECT_EQ(results[0].tenant_id, "t1");
    EXPECT_EQ(results[0].action, "request_received");
}

TEST_F(SQLitePersistentStoreTest, AuditFilterByTenant) {
    store_->insertAudit(makeAudit("r1", "tenant-a"));
    store_->insertAudit(makeAudit("r2", "tenant-b"));
    store_->insertAudit(makeAudit("r3", "tenant-a"));

    auto a = store_->queryAudits("tenant-a");
    EXPECT_EQ(a.size(), 2u);
    auto b = store_->queryAudits("tenant-b");
    EXPECT_EQ(b.size(), 1u);
}

TEST_F(SQLitePersistentStoreTest, AuditPagination) {
    for (int i = 0; i < 10; ++i)
        store_->insertAudit(makeAudit("r" + std::to_string(i)));

    auto page1 = store_->queryAudits("", 3, 0);
    EXPECT_EQ(page1.size(), 3u);
    EXPECT_EQ(page1[0].request_id, "r9");

    auto page2 = store_->queryAudits("", 3, 3);
    EXPECT_EQ(page2.size(), 3u);
    EXPECT_EQ(page2[0].request_id, "r6");
}

TEST_F(SQLitePersistentStoreTest, AuditCount) {
    store_->insertAudit(makeAudit("r1", "tenant-a"));
    store_->insertAudit(makeAudit("r2", "tenant-b"));
    store_->insertAudit(makeAudit("r3", "tenant-a"));

    EXPECT_EQ(store_->auditCount(), 3);
    EXPECT_EQ(store_->auditCount("tenant-a"), 2);
    EXPECT_EQ(store_->auditCount("tenant-c"), 0);
}

TEST_F(SQLitePersistentStoreTest, InsertAndQueryCost) {
    EXPECT_TRUE(store_->insertCostRecord(makeCost("r1")));
    auto results = store_->queryCosts();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].model, "gpt-4");
    EXPECT_DOUBLE_EQ(results[0].total_cost, 0.009);
}

// P1-6: SQLite must persist modality / baseline_cost / routing_decision_reason
// (previously only 10 columns -> these 3 were silently dropped, diverging from
// the in-memory backend which kept them).
TEST_F(SQLitePersistentStoreTest, CostRecordPersistsP1_6Fields) {
    CostRecord r = makeCost("r-p16", "gpt-4o-mini");
    r.modality = "embedding";
    r.baseline_cost = 0.042;
    r.routing_decision_reason = "router_economy";
    ASSERT_TRUE(store_->insertCostRecord(r));

    auto results = store_->queryCosts();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].modality, "embedding");
    EXPECT_DOUBLE_EQ(results[0].baseline_cost, 0.042);
    EXPECT_EQ(results[0].routing_decision_reason, "router_economy");
}

// P1-6: a record written without touching the new fields must read back with
// the schema defaults (modality="chat"), not empty/garbage.
TEST_F(SQLitePersistentStoreTest, CostRecordP1_6Defaults) {
    ASSERT_TRUE(store_->insertCostRecord(makeCost("r-def")));
    auto results = store_->queryCosts();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].modality, "chat");
    EXPECT_DOUBLE_EQ(results[0].baseline_cost, 0.0);
    EXPECT_EQ(results[0].routing_decision_reason, "");
}

TEST_F(SQLitePersistentStoreTest, CostFilterByModel) {
    store_->insertCostRecord(makeCost("r1", "gpt-4"));
    store_->insertCostRecord(makeCost("r2", "claude-3"));
    store_->insertCostRecord(makeCost("r3", "gpt-4"));

    auto gpt = store_->queryCosts("gpt-4");
    EXPECT_EQ(gpt.size(), 2u);
    auto claude = store_->queryCosts("claude-3");
    EXPECT_EQ(claude.size(), 1u);
}

TEST_F(SQLitePersistentStoreTest, CostPagination) {
    for (int i = 0; i < 10; ++i)
        store_->insertCostRecord(makeCost("r" + std::to_string(i)));

    auto page1 = store_->queryCosts("", 3, 0);
    EXPECT_EQ(page1.size(), 3u);
    auto page2 = store_->queryCosts("", 3, 7);
    EXPECT_EQ(page2.size(), 3u);
}

// TASK-20260703-02 Epic 6.2 / C11 ② — queryCosts 排序一致性。SQLite 此前用
// ORDER BY id ASC（最旧在前），与 PG（DESC）+ audits（DESC）不一致 → 统一 DESC。
TEST_F(SQLitePersistentStoreTest, QueryCostsNewestFirst) {
    store_->insertCostRecord(makeCost("r0"));
    store_->insertCostRecord(makeCost("r1"));
    store_->insertCostRecord(makeCost("r2"));
    auto results = store_->queryCosts();
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].request_id, "r2");  // 最新在前（DESC）
    EXPECT_EQ(results[2].request_id, "r0");
}

TEST_F(SQLitePersistentStoreTest, CostRecordCount) {
    store_->insertCostRecord(makeCost("r1", "gpt-4"));
    store_->insertCostRecord(makeCost("r2", "claude-3"));

    EXPECT_EQ(store_->costRecordCount(), 2);
    EXPECT_EQ(store_->costRecordCount("gpt-4"), 1);
    EXPECT_EQ(store_->costRecordCount("nonexistent"), 0);
}

// TASK-20260604-01 P0-E/D1=A — cost tenant 过滤下沉 DB（折叠 P1-11）。
TEST_F(SQLitePersistentStoreTest, CostFilterByTenant) {
    auto c = [](const std::string& req, const std::string& tenant,
                const std::string& model) {
        CostRecord r;
        r.request_id = req; r.tenant_id = tenant; r.app_id = "app1";
        r.model = model; r.input_tokens = 100; r.output_tokens = 50;
        r.input_cost = 0.003; r.output_cost = 0.006; r.total_cost = 0.009;
        r.timestamp = "2026-03-19T00:00:00Z";
        return r;
    };
    store_->insertCostRecord(c("r1", "ta", "gpt-4"));
    store_->insertCostRecord(c("r2", "tb", "gpt-4"));
    store_->insertCostRecord(c("r3", "ta", "claude-3"));

    // tenant 过滤
    auto ta = store_->queryCosts("", 100, 0, "ta");
    EXPECT_EQ(ta.size(), 2u);
    auto tb = store_->queryCosts("", 100, 0, "tb");
    EXPECT_EQ(tb.size(), 1u);
    // model + tenant 联合过滤
    auto ta_gpt = store_->queryCosts("gpt-4", 100, 0, "ta");
    EXPECT_EQ(ta_gpt.size(), 1u);
    // 空 tenant = 不过滤（向后兼容）
    EXPECT_EQ(store_->queryCosts("", 100, 0, "").size(), 3u);

    // costRecordCount tenant 过滤
    EXPECT_EQ(store_->costRecordCount("", "ta"), 2);
    EXPECT_EQ(store_->costRecordCount("", "tb"), 1);
    EXPECT_EQ(store_->costRecordCount("gpt-4", "ta"), 1);
    EXPECT_EQ(store_->costRecordCount(), 3);

    // TASK-20260702-01 P1-2：costTotal DB 级 SUM + tenant 过滤
    EXPECT_NEAR(store_->costTotal("ta"), 0.018, 1e-9);
    EXPECT_NEAR(store_->costTotal("tb"), 0.009, 1e-9);
    EXPECT_NEAR(store_->costTotal(), 0.027, 1e-9);
}

TEST_F(SQLitePersistentStoreTest, SQLInjectionSafe) {
    AuditEntry injected;
    injected.request_id = "r1";
    injected.timestamp = "2026-01-01";
    injected.tenant_id = "'; DROP TABLE audits; --";
    injected.action = "test";
    injected.stage_name = "test";

    EXPECT_TRUE(store_->insertAudit(injected));
    EXPECT_EQ(store_->auditCount(), 1);

    auto results = store_->queryAudits("'; DROP TABLE audits; --");
    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].tenant_id, "'; DROP TABLE audits; --");

    EXPECT_EQ(store_->auditCount(), 1);
}

TEST_F(SQLitePersistentStoreTest, ReopenRestoresData) {
    using namespace std::chrono;
    auto ts = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    auto tmp_path = std::filesystem::temp_directory_path() /
        ("aegis_test_" + std::to_string(getpid()) + "_" + std::to_string(ts) + ".db");
    std::string path = tmp_path.string();

    {
        SQLitePersistentStore s(path);
        ASSERT_TRUE(s.initialize());
        s.insertAudit(makeAudit("r1"));
        s.insertCostRecord(makeCost("r1"));
        s.close();
    }

    {
        SQLitePersistentStore s(path);
        ASSERT_TRUE(s.initialize());
        EXPECT_EQ(s.auditCount(), 1);
        EXPECT_EQ(s.costRecordCount(), 1);
        s.close();
    }

    std::filesystem::remove(path);
    std::filesystem::remove(std::string(path) + "-wal");
    std::filesystem::remove(std::string(path) + "-shm");
}

// C3: initialize() 持锁调用 close() 导致递归死锁
// 修复后 close() 内部不再死锁
TEST_F(SQLitePersistentStoreTest, DoubleInitialize_NoDeadlock) {
    // 第一次已在 SetUp 中 initialize
    // close + 重新 initialize 不应死锁
    store_->close();
    EXPECT_TRUE(store_->initialize());
    EXPECT_TRUE(store_->isHealthy());
}

TEST_F(SQLitePersistentStoreTest, InitCloseInit_DataPersistsInMemory) {
    store_->insertAudit(makeAudit("r-before"));
    // close 后 db 为空
    store_->close();
    EXPECT_FALSE(store_->isHealthy());
    // 重新 initialize（内存数据库会重建空表）
    EXPECT_TRUE(store_->initialize());
    EXPECT_TRUE(store_->isHealthy());
    // 内存数据库在 close 后数据丢失，所以 count=0
    EXPECT_EQ(store_->auditCount(), 0);
}

TEST(SQLitePersistentStoreStandalone, DestructorAfterClose_NoDoubleFree) {
    auto store = std::make_unique<SQLitePersistentStore>(":memory:");
    ASSERT_TRUE(store->initialize());
    store->close();
    store.reset(); // ~SQLitePersistentStore calls close() again — must not crash
}

TEST_F(SQLitePersistentStoreTest, LargeDatasetPerformance) {
    for (int i = 0; i < 1000; ++i)
        store_->insertAudit(makeAudit("r" + std::to_string(i), "perf-tenant"));

    auto start = std::chrono::steady_clock::now();
    auto results = store_->queryAudits("perf-tenant", 50, 500);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(results.size(), 50u);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 100);
}

// --- SSO Storage Tests (TASK-20260324-03) ---

static SsoProvider makeSsoProvider(const std::string& id, const std::string& tenant_id) {
    SsoProvider p;
    p.id = id;
    p.tenant_id = tenant_id;
    p.name = "okta";
    p.issuer_url = "https://test.okta.com/oauth2/default";
    p.client_id = "client-123";
    p.client_secret_enc = "encrypted-secret";
    p.redirect_uri = "https://aegisgate.test/callback";
    p.scopes = {"openid", "profile", "email"};
    p.claim_mapping_json = R"({"username":"preferred_username"})";
    p.group_role_mapping_json = R"({"Admins":"super_admin"})";
    p.jit_provisioning = true;
    p.default_role = "developer";
    p.enabled = true;
    p.created_at = "2026-03-24T00:00:00Z";
    p.updated_at = "2026-03-24T00:00:00Z";
    return p;
}

TEST_F(SQLitePersistentStoreTest, SsoProvider_InsertAndGet) {
    auto p = makeSsoProvider("sp1", "t1");
    ASSERT_TRUE(store_->insertSsoProvider(p));

    auto got = store_->getSsoProvider("sp1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id, "sp1");
    EXPECT_EQ(got->tenant_id, "t1");
    EXPECT_EQ(got->name, "okta");
    EXPECT_EQ(got->issuer_url, "https://test.okta.com/oauth2/default");
    EXPECT_EQ(got->client_id, "client-123");
    EXPECT_EQ(got->client_secret_enc, "encrypted-secret");
    EXPECT_EQ(got->scopes.size(), 3u);
    EXPECT_EQ(got->default_role, "developer");
    EXPECT_TRUE(got->jit_provisioning);
    EXPECT_TRUE(got->enabled);
}

TEST_F(SQLitePersistentStoreTest, SsoProvider_GetByTenant) {
    ASSERT_TRUE(store_->insertSsoProvider(makeSsoProvider("sp1", "t1")));
    ASSERT_TRUE(store_->insertSsoProvider(makeSsoProvider("sp2", "t2")));

    auto got = store_->getSsoProviderByTenant("t1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id, "sp1");

    EXPECT_FALSE(store_->getSsoProviderByTenant("t-nonexist").has_value());
}

TEST_F(SQLitePersistentStoreTest, SsoProvider_UpdateAndDelete) {
    auto p = makeSsoProvider("sp1", "t1");
    ASSERT_TRUE(store_->insertSsoProvider(p));

    p.enabled = false;
    p.name = "keycloak";
    ASSERT_TRUE(store_->updateSsoProvider(p));

    auto got = store_->getSsoProvider("sp1");
    ASSERT_TRUE(got.has_value());
    EXPECT_FALSE(got->enabled);
    EXPECT_EQ(got->name, "keycloak");

    ASSERT_TRUE(store_->deleteSsoProvider("sp1"));
    EXPECT_FALSE(store_->getSsoProvider("sp1").has_value());
}

TEST_F(SQLitePersistentStoreTest, SsoProvider_List) {
    ASSERT_TRUE(store_->insertSsoProvider(makeSsoProvider("sp1", "t1")));
    ASSERT_TRUE(store_->insertSsoProvider(makeSsoProvider("sp2", "t2")));

    auto all = store_->listSsoProviders();
    EXPECT_EQ(all.size(), 2u);

    auto page = store_->listSsoProviders(1, 0);
    EXPECT_EQ(page.size(), 1u);
}

// TASK-20260605-02 P1：ssoProviderCount 全量计数（供 listSsoProviders total / 翻页）。
TEST_F(SQLitePersistentStoreTest, SsoProvider_Count) {
    EXPECT_EQ(store_->ssoProviderCount(), 0);
    ASSERT_TRUE(store_->insertSsoProvider(makeSsoProvider("sp1", "t1")));
    ASSERT_TRUE(store_->insertSsoProvider(makeSsoProvider("sp2", "t2")));
    EXPECT_EQ(store_->ssoProviderCount(), 2);
}

TEST_F(SQLitePersistentStoreTest, IdentityMapping_InsertAndLookup) {
    IdentityMapping m;
    m.id = "im1";
    m.tenant_id = "t1";
    m.external_subject = "google-oauth2|123456";
    m.external_issuer = "https://accounts.google.com";
    m.user_id = "u1";
    m.email = "user@test.com";
    m.created_at = "2026-03-24T00:00:00Z";

    ASSERT_TRUE(store_->insertIdentityMapping(m));

    auto got = store_->getIdentityMapping("google-oauth2|123456", "https://accounts.google.com");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->user_id, "u1");
    EXPECT_EQ(got->email, "user@test.com");

    EXPECT_FALSE(store_->getIdentityMapping("nonexist", "https://accounts.google.com").has_value());
}

TEST_F(SQLitePersistentStoreTest, IdentityMapping_UpdateLastLogin) {
    IdentityMapping m;
    m.id = "im1"; m.tenant_id = "t1";
    m.external_subject = "sub1"; m.external_issuer = "iss1";
    m.user_id = "u1"; m.created_at = "2026-03-24T00:00:00Z";
    ASSERT_TRUE(store_->insertIdentityMapping(m));

    ASSERT_TRUE(store_->updateIdentityMappingLastLogin("im1", "2026-03-24T12:00:00Z"));

    auto got = store_->getIdentityMapping("sub1", "iss1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->last_login_at, "2026-03-24T12:00:00Z");
}

TEST_F(SQLitePersistentStoreTest, Session_InsertGetDelete) {
    Session s;
    s.id = "sess-abc123";
    s.user_id = "u1";
    s.tenant_id = "t1";
    s.ip_address = "192.168.1.1";
    s.user_agent = "Mozilla/5.0";
    s.auth_method = "sso";
    s.mfa_verified = false;
    s.created_at = "2026-03-24T00:00:00Z";
    s.last_active_at = "2026-03-24T00:00:00Z";
    s.expires_at = "2026-03-25T00:00:00Z";

    ASSERT_TRUE(store_->insertSession(s));

    auto got = store_->getSession("sess-abc123");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->user_id, "u1");
    EXPECT_EQ(got->auth_method, "sso");
    EXPECT_FALSE(got->mfa_verified);

    ASSERT_TRUE(store_->deleteSession("sess-abc123"));
    EXPECT_FALSE(store_->getSession("sess-abc123").has_value());
}

TEST_F(SQLitePersistentStoreTest, Session_ListByUserAndCount) {
    for (int i = 0; i < 3; ++i) {
        Session s;
        s.id = "sess-" + std::to_string(i);
        s.user_id = "u1";
        s.tenant_id = "t1";
        s.auth_method = "sso";
        s.created_at = "2026-03-24T00:00:00Z";
        s.last_active_at = "2026-03-24T00:00:00Z";
        s.expires_at = "2026-03-25T00:00:00Z";
        ASSERT_TRUE(store_->insertSession(s));
    }

    EXPECT_EQ(store_->countSessionsByUser("u1"), 3);
    auto sessions = store_->listSessionsByUser("u1");
    EXPECT_EQ(sessions.size(), 3u);
}

TEST_F(SQLitePersistentStoreTest, Session_UpdateActivity) {
    Session s;
    s.id = "sess-1"; s.user_id = "u1"; s.tenant_id = "t1";
    s.auth_method = "sso";
    s.created_at = "2026-03-24T00:00:00Z";
    s.last_active_at = "2026-03-24T00:00:00Z";
    s.expires_at = "2026-03-25T00:00:00Z";
    ASSERT_TRUE(store_->insertSession(s));

    ASSERT_TRUE(store_->updateSessionActivity("sess-1", "2026-03-24T06:00:00Z"));
    auto got = store_->getSession("sess-1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->last_active_at, "2026-03-24T06:00:00Z");
}

TEST_F(SQLitePersistentStoreTest, Session_UpdateMfaVerified) {
    Session s;
    s.id = "sess-1"; s.user_id = "u1"; s.tenant_id = "t1";
    s.auth_method = "sso"; s.mfa_verified = false;
    s.created_at = "2026-03-24T00:00:00Z";
    s.last_active_at = "2026-03-24T00:00:00Z";
    s.expires_at = "2026-03-25T00:00:00Z";
    ASSERT_TRUE(store_->insertSession(s));

    ASSERT_TRUE(store_->updateSessionMfaVerified("sess-1", true));
    auto got = store_->getSession("sess-1");
    ASSERT_TRUE(got.has_value());
    EXPECT_TRUE(got->mfa_verified);
}

TEST_F(SQLitePersistentStoreTest, Session_DeleteExpired) {
    auto makeSession = [&](const std::string& id, const std::string& expires) {
        Session s;
        s.id = id; s.user_id = "u1"; s.tenant_id = "t1";
        s.auth_method = "sso";
        s.created_at = "2026-03-24T00:00:00Z";
        s.last_active_at = "2026-03-24T00:00:00Z";
        s.expires_at = expires;
        return s;
    };

    ASSERT_TRUE(store_->insertSession(makeSession("expired", "2020-01-01T00:00:00Z")));
    ASSERT_TRUE(store_->insertSession(makeSession("valid", "2099-01-01T00:00:00Z")));

    auto deleted = store_->deleteExpiredSessions();
    EXPECT_EQ(deleted, 1);
    EXPECT_FALSE(store_->getSession("expired").has_value());
    EXPECT_TRUE(store_->getSession("valid").has_value());
}

TEST_F(SQLitePersistentStoreTest, MfaSecret_UpsertAndGet) {
    MfaSecret m;
    m.user_id = "u1";
    m.secret_enc = "encrypted-totp-secret";
    m.enabled = true;
    m.recovery_codes_hash = {"hash1", "hash2", "hash3"};
    m.created_at = "2026-03-24T00:00:00Z";

    ASSERT_TRUE(store_->upsertMfaSecret(m));

    auto got = store_->getMfaSecret("u1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->secret_enc, "encrypted-totp-secret");
    EXPECT_TRUE(got->enabled);
    EXPECT_EQ(got->recovery_codes_hash.size(), 3u);

    m.enabled = false;
    ASSERT_TRUE(store_->upsertMfaSecret(m));
    got = store_->getMfaSecret("u1");
    EXPECT_FALSE(got->enabled);
}

TEST_F(SQLitePersistentStoreTest, MfaSecret_Delete) {
    MfaSecret m;
    m.user_id = "u1"; m.secret_enc = "s"; m.created_at = "2026-03-24T00:00:00Z";
    ASSERT_TRUE(store_->upsertMfaSecret(m));

    ASSERT_TRUE(store_->deleteMfaSecret("u1"));
    EXPECT_FALSE(store_->getMfaSecret("u1").has_value());
}

TEST_F(SQLitePersistentStoreTest, ScimToken_InsertAndGetByHash) {
    ScimToken t;
    t.id = "st1";
    t.tenant_id = "t1";
    t.token_hash = "sha256-hash-value";
    t.description = "SCIM sync token";
    t.created_at = "2026-03-24T00:00:00Z";
    t.expires_at = "2027-03-24T00:00:00Z";

    ASSERT_TRUE(store_->insertScimToken(t));

    auto got = store_->getScimTokenByHash("sha256-hash-value");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->tenant_id, "t1");
    EXPECT_EQ(got->description, "SCIM sync token");

    EXPECT_FALSE(store_->getScimTokenByHash("nonexist").has_value());
}

TEST_F(SQLitePersistentStoreTest, ScimToken_ListAndDelete) {
    ScimToken t1;
    t1.id = "st1"; t1.tenant_id = "t1"; t1.token_hash = "h1";
    t1.created_at = "2026-03-24T00:00:00Z";
    ScimToken t2;
    t2.id = "st2"; t2.tenant_id = "t1"; t2.token_hash = "h2";
    t2.created_at = "2026-03-24T00:00:00Z";

    ASSERT_TRUE(store_->insertScimToken(t1));
    ASSERT_TRUE(store_->insertScimToken(t2));

    auto list = store_->listScimTokens("t1");
    EXPECT_EQ(list.size(), 2u);

    ASSERT_TRUE(store_->deleteScimToken("st1"));
    list = store_->listScimTokens("t1");
    EXPECT_EQ(list.size(), 1u);
}
