#include <gtest/gtest.h>
#include "storage/pg_persistent_store.h"
#include "auth/crypto_utils.h"
#include "control_plane/rollout/rollout_record.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace aegisgate;

// Unit tests for PgPersistentStore API surface — no real PG required.
// Integration tests only run when AEGISGATE_PG_URL is set.

TEST(PgPersistentStoreUnit, BackendName) {
    PgConfig cfg;
    PgPersistentStore store(cfg);
    EXPECT_EQ(store.backendName(), "postgres");
}

TEST(PgPersistentStoreUnit, NotHealthyBeforeInit) {
    PgConfig cfg;
    PgPersistentStore store(cfg);
    EXPECT_FALSE(store.isHealthy());
}

TEST(PgPersistentStoreUnit, OperationsReturnDefaultsBeforeInit) {
    PgConfig cfg;
    PgPersistentStore store(cfg);

    AuditEntry entry;
    entry.request_id = "req-1";
    entry.timestamp = "2026-03-19T00:00:00Z";
    EXPECT_FALSE(store.insertAudit(entry));
    EXPECT_TRUE(store.queryAudits().empty());
    EXPECT_EQ(store.auditCount(), 0);

    CostRecord record;
    record.request_id = "req-1";
    record.timestamp = "2026-03-19T00:00:00Z";
    EXPECT_FALSE(store.insertCostRecord(record));
    EXPECT_TRUE(store.queryCosts().empty());
    EXPECT_EQ(store.costRecordCount(), 0);
}

// TASK-20260702-01 P1-5 — savings events surface exists (override, not base
// no-op) and behaves safely before init (no real PG required).
TEST(PgPersistentStoreUnit, SavingsSurfaceReturnsDefaultsBeforeInit) {
    PgConfig cfg;
    PgPersistentStore store(cfg);

    PersistentStore::SavingsEventRecord ev;
    ev.type = 0; ev.model = "gpt-4"; ev.tenant_id = "t1";
    ev.tokens_saved = 10; ev.cost_saved = 0.5; ev.timestamp = "2026-03-19T00:00:00Z";
    EXPECT_FALSE(store.insertSavingsEvent(ev));
    EXPECT_TRUE(store.querySavingsEventsByDateRange("", "").empty());
    EXPECT_EQ(store.savingsEventCount(), 0);
    EXPECT_EQ(store.pruneSavingsEvents(30), 0);
}

// TASK-20260604-01 P0-A/B/E — new API surface (no real PG required).
TEST(PgPersistentStoreUnit, NewSurfaceReturnsDefaultsBeforeInit) {
    PgConfig cfg;
    PgPersistentStore store(cfg);

    // P0-E：tenant-filtered cost query + counts.
    EXPECT_TRUE(store.queryCosts("gpt-4", 10, 0, "t1").empty());
    EXPECT_EQ(store.costRecordCount("gpt-4", "t1"), 0);
    EXPECT_EQ(store.tenantCount(), 0);
    EXPECT_EQ(store.userCount("t1"), 0);
    EXPECT_EQ(store.apiKeyCount("t1"), 0);
    EXPECT_EQ(store.promptTemplateCount("t1"), 0);

    // P0-B：date-range cost query.
    EXPECT_TRUE(store.queryCostsByDateRange("t1", "2026-01-01", "2026-12-31").empty());

    // P0-A：prompt template surface.
    PersistentStore::PromptTemplateRecord tpl;
    tpl.id = "tpl1"; tpl.tenant_id = "t1"; tpl.name = "n"; tpl.content = "c";
    EXPECT_FALSE(store.insertPromptTemplate(tpl));
    EXPECT_FALSE(store.getPromptTemplate("tpl1").has_value());
    EXPECT_FALSE(store.updatePromptTemplate(tpl));
    EXPECT_FALSE(store.deletePromptTemplate("tpl1"));
    EXPECT_TRUE(store.listPromptTemplates("t1").empty());
    EXPECT_TRUE(store.listPromptTemplatesByName("t1", "n").empty());

    // P0-A：rule set surface.
    PersistentStore::RuleSetRecord rs;
    rs.tenant_id = "t1"; rs.version = 1; rs.rules_json = "[]"; rs.is_active = true;
    EXPECT_FALSE(store.insertRuleSet("t1", rs));
    EXPECT_FALSE(store.getActiveRuleSet("t1").has_value());
    EXPECT_TRUE(store.listRuleSetVersions("t1").empty());
    EXPECT_FALSE(store.activateRuleSetVersion("t1", 1));
}

// Phase 11.5 TASK-20260518-02 Epic 1.0 — ApprovalProposal API surface checks
// that do not require a real PG instance. Integration tests covering insert
// roundtrip live in PgRolloutIntegrationTest companion (added in Epic 1.5).
TEST(PgPersistentStoreUnit, ApprovalProposalDefaultsBeforeInit) {
    PgConfig cfg;
    PgPersistentStore store(cfg);

    ApprovalProposalRecord rec;
    rec.id = "01HNPGUNIT00000000000000A1";
    rec.source = "CostOptimizer";
    rec.state = "PROPOSED";
    rec.proposed_at_ms = 1716030000000LL;

    EXPECT_FALSE(store.insertApprovalProposal(rec));
    EXPECT_FALSE(store.getApprovalProposal(rec.id).has_value());
    EXPECT_FALSE(store.updateApprovalProposal(rec));
    EXPECT_TRUE(store.listApprovalProposals({}).empty());
    EXPECT_EQ(store.pruneApprovalProposals(90), 0);
    EXPECT_EQ(store.pruneApprovalProposals(0), 0);
}

TEST(PgPersistentStoreUnit, ApprovalProposalEmptyIdAndArgGuards) {
    PgConfig cfg;
    PgPersistentStore store(cfg);

    ApprovalProposalRecord rec;
    rec.id = "";
    EXPECT_FALSE(store.insertApprovalProposal(rec));
    EXPECT_FALSE(store.getApprovalProposal("").has_value());
}

TEST(PgPersistentStoreUnit, PruneReturnsZeroBeforeInit) {
    PgConfig cfg;
    PgPersistentStore store(cfg);
    EXPECT_EQ(store.pruneAudits(90), 0);
    EXPECT_EQ(store.pruneCostRecords(90), 0);
}

TEST(PgPersistentStoreUnit, PruneZeroDaysReturnsZero) {
    PgConfig cfg;
    PgPersistentStore store(cfg);
    EXPECT_EQ(store.pruneAudits(0), 0);
    EXPECT_EQ(store.pruneCostRecords(0), 0);
}

TEST(PgPersistentStoreUnit, CloseIsIdempotent) {
    PgConfig cfg;
    PgPersistentStore store(cfg);
    store.close();
    store.close();
    EXPECT_FALSE(store.isHealthy());
}

// Phase 9.3 control plane — surface-level "not initialized" guards.
// Real behavior is covered by PgIntegrationTest when AEGISGATE_PG_URL is set.
TEST(PgPersistentStoreUnit, ConfigVersionOpsReturnDefaultsBeforeInit) {
    PgConfig cfg;
    PgPersistentStore store(cfg);

    ConfigVersionRecord rec{};
    rec.version_id = "01J8A00000000000000000001";
    rec.yaml_content = "x: 1\n";
    rec.size_bytes = 5;
    rec.status = ConfigStatus::PENDING;

    EXPECT_FALSE(store.insertConfigVersion(rec));
    EXPECT_FALSE(store.updateConfigStatus(
        "01J8A00000000000000000001", ConfigStatus::APPROVED,
        "bob", "LGTM", 100));
    EXPECT_FALSE(store.getConfigVersion("01J8A00000000000000000001").has_value());
    EXPECT_TRUE(store.listConfigVersions({}).empty());
    EXPECT_FALSE(store.getActiveConfig().has_value());
    EXPECT_FALSE(store.activateConfig(
        "01J8A00000000000000000001", "carol", 200));
}

TEST(PgPersistentStoreUnit, ActivateConfigRejectsEmptyVersionId) {
    PgConfig cfg;
    PgPersistentStore store(cfg);
    EXPECT_FALSE(store.activateConfig("", "carol", 100));
}

TEST(PgPersistentStoreUnit, InsertConfigVersionRejectsEmptyVersionId) {
    PgConfig cfg;
    PgPersistentStore store(cfg);
    ConfigVersionRecord rec{};
    EXPECT_FALSE(store.insertConfigVersion(rec));
}

// Integration tests — only run when AEGISGATE_PG_URL is set
class PgIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* url = std::getenv("AEGISGATE_PG_URL");
        if (!url) {
            GTEST_SKIP() << "AEGISGATE_PG_URL not set, skipping integration tests";
        }
        PgConfig cfg;
        cfg.url = url;
        cfg.pool_size = 2;
        store_ = std::make_unique<PgPersistentStore>(cfg);
        ASSERT_TRUE(store_->initialize());
    }

    void TearDown() override {
        if (store_) {
            store_->close();
        }
    }

    std::unique_ptr<PgPersistentStore> store_;
};

TEST_F(PgIntegrationTest, InsertAndQueryAudits) {
    AuditEntry entry;
    entry.request_id = "pg-test-req-1";
    entry.timestamp = "2026-03-19T12:00:00Z";
    entry.tenant_id = "tenant-pg";
    entry.action = "test";
    entry.stage_name = "PgTest";
    entry.detail = "integration test";
    entry.input_hash = "abc";
    entry.output_hash = "def";

    ASSERT_TRUE(store_->insertAudit(entry));
    auto results = store_->queryAudits("tenant-pg", 10, 0);
    ASSERT_GE(results.size(), 1u);
    EXPECT_EQ(results[0].request_id, "pg-test-req-1");
}

TEST_F(PgIntegrationTest, InsertAndQueryCosts) {
    CostRecord record;
    record.request_id = "pg-cost-1";
    record.timestamp = "2026-03-19T12:00:00Z";
    record.model = "gpt-4-pg";
    record.input_tokens = 100;
    record.output_tokens = 50;
    record.total_cost = 0.005;
    // P1-6: exercise the 3 previously-dropped columns end-to-end.
    record.modality = "embedding";
    record.baseline_cost = 0.0123;
    record.routing_decision_reason = "router_economy";

    ASSERT_TRUE(store_->insertCostRecord(record));
    auto results = store_->queryCosts("gpt-4-pg", 10, 0);
    ASSERT_GE(results.size(), 1u);
    EXPECT_EQ(results[0].model, "gpt-4-pg");
    // P1-6: roundtrip parity with the in-memory / SQLite backends.
    EXPECT_EQ(results[0].modality, "embedding");
    EXPECT_DOUBLE_EQ(results[0].baseline_cost, 0.0123);
    EXPECT_EQ(results[0].routing_decision_reason, "router_economy");
}

// TASK-20260702-01 P1-5 — savings events persist on PG (parity w/ SQLite).
TEST_F(PgIntegrationTest, SavingsEventsPersistAndQuery) {
    // A shared/persistent dev cluster accumulates this test's rows across
    // runs; the absolute range-size assertion below only holds on a clean
    // slate. Clear this test's tenant rows first (mirrors the raw-libpq
    // cleanup used by the prompt-template / rule-set integration tests).
    // CI runs against an ephemeral postgres container, so this is a no-op
    // there but keeps the test deterministic on a long-lived cluster.
    {
        PGconn* c = PQconnectdb(std::getenv("AEGISGATE_PG_URL"));
        PQclear(PQexec(c,
            "DELETE FROM savings_events WHERE tenant_id = 'tenant-pg'"));
        PQfinish(c);
    }
    int64_t before = store_->savingsEventCount();

    auto mk = [](const std::string& model, double cost, const std::string& ts) {
        PersistentStore::SavingsEventRecord ev;
        ev.type = 1; ev.model = model; ev.tenant_id = "tenant-pg";
        ev.tokens_saved = 42; ev.cost_saved = cost; ev.fallback_pricing = true;
        ev.timestamp = ts;
        return ev;
    };
    ASSERT_TRUE(store_->insertSavingsEvent(mk("gpt-4-pg", 0.11, "2026-06-01T10:00:00Z")));
    ASSERT_TRUE(store_->insertSavingsEvent(mk("gpt-4-pg", 0.22, "2026-06-15T10:00:00Z")));

    EXPECT_EQ(store_->savingsEventCount(), before + 2);

    // Date-range query: only the second event falls in [06-10, 06-20].
    auto ranged = store_->querySavingsEventsByDateRange(
        "2026-06-10T00:00:00Z", "2026-06-20T00:00:00Z");
    ASSERT_EQ(ranged.size(), 1u);
    EXPECT_DOUBLE_EQ(ranged[0].cost_saved, 0.22);
    EXPECT_EQ(ranged[0].tokens_saved, 42);
    EXPECT_TRUE(ranged[0].fallback_pricing);
    EXPECT_EQ(ranged[0].type, 1);

    // Open-ended query returns both (ordered ascending by timestamp).
    auto all = store_->querySavingsEventsByDateRange("2026-06-01T00:00:00Z", "");
    ASSERT_GE(all.size(), 2u);
    EXPECT_LE(all.front().timestamp, all.back().timestamp);
}

TEST_F(PgIntegrationTest, SavingsEventsPrune) {
    // 极旧事件应被 retention 裁剪；近事件保留。
    PersistentStore::SavingsEventRecord old_ev;
    old_ev.type = 0; old_ev.model = "old"; old_ev.tenant_id = "tenant-pg";
    old_ev.cost_saved = 1.0; old_ev.timestamp = "2000-01-01T00:00:00Z";
    ASSERT_TRUE(store_->insertSavingsEvent(old_ev));

    int64_t deleted = store_->pruneSavingsEvents(30);
    EXPECT_GE(deleted, 1);
}

TEST_F(PgIntegrationTest, AuditCount) {
    int64_t before = store_->auditCount();
    AuditEntry entry;
    entry.request_id = "pg-count-test";
    entry.timestamp = "2026-03-19T12:00:00Z";
    store_->insertAudit(entry);
    EXPECT_EQ(store_->auditCount(), before + 1);
}

TEST_F(PgIntegrationTest, IsHealthy) {
    EXPECT_TRUE(store_->isHealthy());
}

// TASK-20260702-02 P2-2（SR-2）：MFA 失败锁定持久化（PG 后端）。
TEST_F(PgIntegrationTest, MfaFailureLockoutPersistsAndClears) {
    const std::string uid = "pg-mfa-lock-" + std::to_string(::time(nullptr));
    int64_t now = static_cast<int64_t>(::time(nullptr));
    // 阈值 3：前 2 次不锁，第 3 次锁定。
    EXPECT_FALSE(store_->recordMfaFailure(uid, 3, 900));
    EXPECT_FALSE(store_->recordMfaFailure(uid, 3, 900));
    EXPECT_TRUE(store_->recordMfaFailure(uid, 3, 900));
    EXPECT_GT(store_->getMfaLockedUntil(uid), now);
    // 清零后不再锁定。
    EXPECT_TRUE(store_->clearMfaFailures(uid));
    EXPECT_EQ(store_->getMfaLockedUntil(uid), 0);
}

// --- PG RBAC integration tests ---

class PgRbacIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* url = std::getenv("AEGISGATE_PG_URL");
        if (!url) {
            GTEST_SKIP() << "AEGISGATE_PG_URL not set, skipping PG RBAC integration tests";
        }
        PgConfig cfg;
        cfg.url = url;
        cfg.pool_size = 2;
        store_ = std::make_unique<PgPersistentStore>(cfg);
        ASSERT_TRUE(store_->initialize());
        cleanupTestData();
    }

    void TearDown() override {
        if (store_) {
            cleanupTestData();
            store_->close();
        }
    }

    void cleanupTestData() {
        // deleteTenant() does not cascade to child rows (users / api_keys /
        // cost_records) or to per-tenant prompt_templates / rule_sets, so on
        // a shared/persistent cluster those would leak across runs and break
        // the fixed-id inserts + absolute cost assertions below. Scrub the
        // child tables via raw libpq first (parent last for FK order). CI's
        // ephemeral container makes this a no-op there.
        const char* url = std::getenv("AEGISGATE_PG_URL");
        if (url) {
            PGconn* c = PQconnectdb(url);
            if (PQstatus(c) == CONNECTION_OK) {
                PQclear(PQexec(c, "DELETE FROM api_keys WHERE tenant_id IN ('pg-t1','pg-t2')"));
                PQclear(PQexec(c, "DELETE FROM users WHERE tenant_id IN ('pg-t1','pg-t2')"));
                PQclear(PQexec(c, "DELETE FROM cost_records WHERE tenant_id IN ('pg-t1','pg-t2')"));
                PQclear(PQexec(c, "DELETE FROM prompt_templates WHERE tenant_id IN ('pg-t1','pg-t2')"));
                PQclear(PQexec(c, "DELETE FROM rule_sets WHERE tenant_id IN ('pg-t1','pg-t2')"));
            }
            PQfinish(c);
        }
        store_->deleteTenant("pg-t1");
        store_->deleteTenant("pg-t2");
    }

    std::unique_ptr<PgPersistentStore> store_;
};

TEST_F(PgRbacIntegrationTest, TenantCrud) {
    Tenant t;
    t.id = "pg-t1"; t.name = "PG Tenant"; t.status = "active";
    t.model_whitelist = {"gpt-4", "claude-3"};
    t.daily_cost_limit = 10.0; t.monthly_cost_limit = 100.0;
    t.rate_limit_tokens = 1000; t.rate_limit_refill = 10.0;
    t.created_at = "2026-03-23T00:00:00Z"; t.updated_at = "2026-03-23T00:00:00Z";
    ASSERT_TRUE(store_->insertTenant(t));

    auto got = store_->getTenant("pg-t1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "PG Tenant");
    EXPECT_EQ(got->model_whitelist.size(), 2u);
    EXPECT_EQ(got->model_whitelist[0], "gpt-4");
    EXPECT_DOUBLE_EQ(got->daily_cost_limit, 10.0);

    t.name = "Updated";
    t.updated_at = "2026-03-23T01:00:00Z";
    ASSERT_TRUE(store_->updateTenant(t));
    got = store_->getTenant("pg-t1");
    EXPECT_EQ(got->name, "Updated");

    auto all = store_->listTenants();
    EXPECT_GE(all.size(), 1u);

    ASSERT_TRUE(store_->deleteTenant("pg-t1"));
    EXPECT_FALSE(store_->getTenant("pg-t1").has_value());
}

TEST_F(PgRbacIntegrationTest, UserCrud) {
    Tenant t; t.id = "pg-t1"; t.name = "T"; t.status = "active";
    t.created_at = "2026-03-23T00:00:00Z"; t.updated_at = "2026-03-23T00:00:00Z";
    store_->insertTenant(t);

    User u;
    u.id = "pg-u1"; u.tenant_id = "pg-t1"; u.username = "alice";
    u.display_name = "Alice"; u.role = Role::Developer; u.status = "active";
    u.created_at = "2026-03-23T00:00:00Z"; u.updated_at = "2026-03-23T00:00:00Z";
    ASSERT_TRUE(store_->insertUser(u));

    auto got = store_->getUser("pg-u1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->username, "alice");
    EXPECT_EQ(got->role, Role::Developer);

    auto by_name = store_->getUserByUsername("pg-t1", "alice");
    ASSERT_TRUE(by_name.has_value());
    EXPECT_EQ(by_name->id, "pg-u1");

    auto users = store_->listUsers("pg-t1");
    EXPECT_GE(users.size(), 1u);
}

TEST_F(PgRbacIntegrationTest, ApiKeyCrud) {
    Tenant t; t.id = "pg-t1"; t.name = "T"; t.status = "active";
    t.created_at = "2026-03-23T00:00:00Z"; t.updated_at = "2026-03-23T00:00:00Z";
    store_->insertTenant(t);

    User u; u.id = "pg-u1"; u.tenant_id = "pg-t1"; u.username = "alice";
    u.role = Role::Developer; u.status = "active";
    u.created_at = "2026-03-23T00:00:00Z"; u.updated_at = "2026-03-23T00:00:00Z";
    store_->insertUser(u);

    auto raw = auth::generateApiKey();
    ApiKeyRecord k;
    k.id = "pg-k1"; k.user_id = "pg-u1"; k.tenant_id = "pg-t1";
    k.key_prefix = auth::extractKeyPrefix(raw);
    k.key_hash = auth::hashApiKey(raw);
    k.role = Role::Developer; k.status = "active";
    k.created_at = "2026-03-23T00:00:00Z"; k.updated_at = "2026-03-23T00:00:00Z";
    ASSERT_TRUE(store_->insertApiKey(k));

    auto got = store_->getApiKeyByHash(k.key_hash);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id, "pg-k1");

    auto by_prefix = store_->getApiKeysByPrefix(k.key_prefix);
    EXPECT_GE(by_prefix.size(), 1u);

    ASSERT_TRUE(store_->revokeApiKey("pg-k1"));
    got = store_->getApiKeyByHash(k.key_hash);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, "revoked");
}

TEST_F(PgRbacIntegrationTest, WhitelistSpecialChars) {
    Tenant t;
    t.id = "pg-t1"; t.name = "Special"; t.status = "active";
    t.model_whitelist = {"gpt-4", "model\"quoted", "模型中文"};
    t.created_at = "2026-03-23T00:00:00Z"; t.updated_at = "2026-03-23T00:00:00Z";
    ASSERT_TRUE(store_->insertTenant(t));

    auto got = store_->getTenant("pg-t1");
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(got->model_whitelist.size(), 3u);
    EXPECT_EQ(got->model_whitelist[1], "model\"quoted");
    EXPECT_EQ(got->model_whitelist[2], "模型中文");
}

TEST_F(PgRbacIntegrationTest, TenantCostInPeriod) {
    Tenant t; t.id = "pg-t1"; t.name = "T"; t.status = "active";
    t.created_at = "2026-03-23T00:00:00Z"; t.updated_at = "2026-03-23T00:00:00Z";
    store_->insertTenant(t);

    CostRecord c1;
    c1.request_id = "pg-r1"; c1.tenant_id = "pg-t1"; c1.model = "gpt-4";
    c1.total_cost = 0.05; c1.timestamp = "2026-03-23T10:00:00Z";
    CostRecord c2;
    c2.request_id = "pg-r2"; c2.tenant_id = "pg-t1"; c2.model = "gpt-4";
    c2.total_cost = 0.10; c2.timestamp = "2026-03-23T12:00:00Z";
    store_->insertCostRecord(c1);
    store_->insertCostRecord(c2);

    double cost = store_->getTenantCostInPeriod("pg-t1",
        "2026-03-23T00:00:00Z", "2026-03-24T00:00:00Z");
    EXPECT_NEAR(cost, 0.15, 0.001);
}

// TASK-20260604-01 P0-A — PG prompt template + rule set roundtrip (real PG).
TEST_F(PgRbacIntegrationTest, PromptTemplateRoundtrip) {
    Tenant t; t.id = "pg-t1"; t.name = "T"; t.status = "active";
    t.created_at = "2026-06-04T00:00:00Z"; t.updated_at = "2026-06-04T00:00:00Z";
    store_->insertTenant(t);

    PersistentStore::PromptTemplateRecord tpl;
    tpl.id = "pg-tpl1"; tpl.tenant_id = "pg-t1"; tpl.name = "greet";
    tpl.content = "hello"; tpl.version = 1; tpl.weight = 100; tpl.is_active = true;
    tpl.created_at = "2026-06-04T00:00:00Z"; tpl.updated_at = "2026-06-04T00:00:00Z";
    ASSERT_TRUE(store_->insertPromptTemplate(tpl));

    auto got = store_->getPromptTemplate("pg-tpl1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "greet");
    EXPECT_EQ(got->content, "hello");
    EXPECT_TRUE(got->is_active);

    tpl.content = "hi there"; tpl.version = 2;
    ASSERT_TRUE(store_->updatePromptTemplate(tpl));
    got = store_->getPromptTemplate("pg-tpl1");
    EXPECT_EQ(got->content, "hi there");
    EXPECT_EQ(got->version, 2);

    EXPECT_EQ(store_->listPromptTemplates("pg-t1").size(), 1u);
    EXPECT_EQ(store_->listPromptTemplatesByName("pg-t1", "greet").size(), 1u);
    EXPECT_EQ(store_->promptTemplateCount("pg-t1"), 1);

    ASSERT_TRUE(store_->deletePromptTemplate("pg-tpl1"));
    EXPECT_FALSE(store_->getPromptTemplate("pg-tpl1").has_value());

    // cleanup
    PGconn* conn = PQconnectdb(std::getenv("AEGISGATE_PG_URL"));
    PQclear(PQexec(conn, "DELETE FROM prompt_templates WHERE tenant_id = 'pg-t1'"));
    PQfinish(conn);
}

TEST_F(PgRbacIntegrationTest, RuleSetRoundtripAndActivation) {
    Tenant t; t.id = "pg-t1"; t.name = "T"; t.status = "active";
    t.created_at = "2026-06-04T00:00:00Z"; t.updated_at = "2026-06-04T00:00:00Z";
    store_->insertTenant(t);

    PGconn* conn = PQconnectdb(std::getenv("AEGISGATE_PG_URL"));
    PQclear(PQexec(conn, "DELETE FROM rule_sets WHERE tenant_id = 'pg-t1'"));

    PersistentStore::RuleSetRecord v1;
    v1.tenant_id = "pg-t1"; v1.version = 1; v1.rules_json = "[\"a\"]";
    v1.created_at = "2026-06-04T00:00:00Z"; v1.is_active = true;
    ASSERT_TRUE(store_->insertRuleSet("pg-t1", v1));

    PersistentStore::RuleSetRecord v2;
    v2.tenant_id = "pg-t1"; v2.version = 2; v2.rules_json = "[\"b\"]";
    v2.created_at = "2026-06-04T01:00:00Z"; v2.is_active = true;
    ASSERT_TRUE(store_->insertRuleSet("pg-t1", v2));

    // 插入 v2(active) 应使 v1 失活 → 当前 active = v2。
    auto active = store_->getActiveRuleSet("pg-t1");
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->version, 2);

    EXPECT_EQ(store_->listRuleSetVersions("pg-t1").size(), 2u);

    // TASK-20260605-01：ruleSetCount + offset 翻页（版本倒序 2,1 → offset=1 取 v1）。
    EXPECT_EQ(store_->ruleSetCount("pg-t1"), 2);
    auto page = store_->listRuleSetVersions("pg-t1", 1, 1);
    ASSERT_EQ(page.size(), 1u);
    EXPECT_EQ(page[0].version, 1);

    // 切回 v1。
    ASSERT_TRUE(store_->activateRuleSetVersion("pg-t1", 1));
    active = store_->getActiveRuleSet("pg-t1");
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->version, 1);

    // 不存在的版本激活失败。
    EXPECT_FALSE(store_->activateRuleSetVersion("pg-t1", 99));

    PQclear(PQexec(conn, "DELETE FROM rule_sets WHERE tenant_id = 'pg-t1'"));
    PQfinish(conn);
}

TEST_F(PgRbacIntegrationTest, PaginationTotalCounts) {
    Tenant t1; t1.id = "pg-t1"; t1.name = "T1"; t1.status = "active";
    t1.created_at = "2026-06-04T00:00:00Z"; t1.updated_at = "2026-06-04T00:00:00Z";
    Tenant t2; t2.id = "pg-t2"; t2.name = "T2"; t2.status = "active";
    t2.created_at = "2026-06-04T00:00:00Z"; t2.updated_at = "2026-06-04T00:00:00Z";
    store_->insertTenant(t1); store_->insertTenant(t2);
    EXPECT_GE(store_->tenantCount(), 2);

    User u1; u1.id = "pg-u1"; u1.tenant_id = "pg-t1"; u1.username = "a";
    u1.role = Role::Developer; u1.status = "active";
    u1.created_at = "2026-06-04T00:00:00Z"; u1.updated_at = "2026-06-04T00:00:00Z";
    store_->insertUser(u1);
    EXPECT_EQ(store_->userCount("pg-t1"), 1);
    EXPECT_EQ(store_->userCount("pg-t2"), 0);
}

// --- Phase 9.3 control plane — PG integration tests ---
//
// Exercises the real backend end-to-end; only runs when AEGISGATE_PG_URL is
// set. CI enables this via the `pg` compose service in
// .github/workflows/ci.yml.

class PgConfigVersionIntegrationTest : public PgIntegrationTest {
protected:
    void SetUp() override {
        PgIntegrationTest::SetUp();
        if (!store_) return;
        // These tests seed fixed version_ids (pg-cp-01..03) and there is no
        // delete() surface, so on a persistent cluster a re-run would hit
        // duplicate-key / stale-active-config state. Scrub the seed rows via
        // raw libpq first (no-op on CI's ephemeral container).
        const char* url = std::getenv("AEGISGATE_PG_URL");
        if (url) {
            PGconn* c = PQconnectdb(url);
            if (PQstatus(c) == CONNECTION_OK) {
                PQclear(PQexec(c,
                    "DELETE FROM config_versions "
                    "WHERE version_id IN ('pg-cp-01','pg-cp-02','pg-cp-03')"));
            }
            PQfinish(c);
        }
    }
};

TEST_F(PgConfigVersionIntegrationTest, InsertGetActivateRoundtrip) {
    ConfigVersionRecord rec{};
    rec.version_id      = "pg-cp-01";
    rec.content_sha256  = "sha-01";
    {
        const char raw[] = {'b','i','n','\0',static_cast<char>(0xff),
                             'd','a','t','a'};
        rec.yaml_content.assign(raw, raw + sizeof(raw));
    }
    rec.size_bytes      = 9;
    rec.status          = ConfigStatus::APPROVED;
    rec.submitter       = "alice";
    rec.submitted_at    = 1000;
    ASSERT_TRUE(store_->insertConfigVersion(rec));

    auto got = store_->getConfigVersion("pg-cp-01");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->yaml_content.size(), 9u);
    EXPECT_EQ(got->yaml_content, rec.yaml_content);

    ASSERT_TRUE(store_->activateConfig("pg-cp-01", "carol", 2000));
    auto active = store_->getActiveConfig();
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->version_id, "pg-cp-01");
    EXPECT_EQ(active->activator, "carol");
    EXPECT_EQ(active->status, ConfigStatus::ACTIVE);
}

TEST_F(PgConfigVersionIntegrationTest, ActivationSupersedesPrevious) {
    ConfigVersionRecord a{}, b{};
    a.version_id = "pg-cp-02"; a.status = ConfigStatus::APPROVED;
    a.yaml_content = "a"; a.size_bytes = 1; a.submitted_at = 1000;
    b.version_id = "pg-cp-03"; b.status = ConfigStatus::APPROVED;
    b.yaml_content = "b"; b.size_bytes = 1; b.submitted_at = 2000;
    ASSERT_TRUE(store_->insertConfigVersion(a));
    ASSERT_TRUE(store_->insertConfigVersion(b));
    ASSERT_TRUE(store_->activateConfig("pg-cp-02", "x", 1500));
    ASSERT_TRUE(store_->activateConfig("pg-cp-03", "y", 2500));

    auto old_ = store_->getConfigVersion("pg-cp-02");
    ASSERT_TRUE(old_.has_value());
    EXPECT_EQ(old_->status, ConfigStatus::SUPERSEDED);
    EXPECT_EQ(old_->deactivated_at, 2500);
}

// ---------------------------------------------------------------------------
// Phase 9.3.4 RolloutController PG backend (TASK-20260507-01)
//
// Schema migration + 7 virtual methods + SR14 partial UNIQUE INDEX defense in
// depth. All tests below GTEST_SKIP when AEGISGATE_PG_URL is unset; CI runs
// them through the `build-pg` job's docker-compose postgres service.
// ---------------------------------------------------------------------------

namespace {

// Best-effort cleanup helper: deletes any leftover rows whose ids match the
// given LIKE pattern. Used in SetUp/TearDown of rollout tests to keep them
// isolated when sharing a long-lived dev PG.
[[maybe_unused]] void pgCleanRollout(const char* url,
                                     const std::string& like_pattern) {
    PGconn* conn = PQconnectdb(url);
    if (PQstatus(conn) != CONNECTION_OK) { PQfinish(conn); return; }
    std::string ev_sql =
        "DELETE FROM rollout_stage_events WHERE rollout_id LIKE '" +
        like_pattern + "'";
    std::string r_sql =
        "DELETE FROM rollouts WHERE rollout_id LIKE '" + like_pattern + "'";
    PQclear(PQexec(conn, ev_sql.c_str()));
    PQclear(PQexec(conn, r_sql.c_str()));
    PQfinish(conn);
}

}  // namespace

// Epic 1.1 — schema migration creates two tables and the partial UNIQUE
// INDEX needed for the SR14 defense-in-depth invariant.
TEST(PgRolloutStore, SchemaTablesCreated) {
    const char* url = std::getenv("AEGISGATE_PG_URL");
    if (!url) GTEST_SKIP() << "AEGISGATE_PG_URL not set";
    PgConfig cfg; cfg.url = url; cfg.pool_size = 1;
    PgPersistentStore store(cfg);
    ASSERT_TRUE(store.initialize());

    PGconn* conn = PQconnectdb(url);
    ASSERT_EQ(PQstatus(conn), CONNECTION_OK);

    PGresult* res = PQexec(conn,
        "SELECT count(*) FROM information_schema.tables "
        "WHERE table_name IN ('rollouts', 'rollout_stage_events')");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "2");
    PQclear(res);

    res = PQexec(conn,
        "SELECT count(*) FROM pg_indexes "
        "WHERE indexname = 'rollouts_one_active_per_target'");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "1");
    PQclear(res);

    res = PQexec(conn,
        "SELECT count(*) FROM pg_indexes "
        "WHERE indexname = 'rollouts_started_at_idx'");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "1");
    PQclear(res);

    res = PQexec(conn,
        "SELECT count(*) FROM pg_indexes "
        "WHERE indexname = 'rollout_stage_events_by_rollout'");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "1");
    PQclear(res);

    PQfinish(conn);
}

// Epic 1.3 — IF NOT EXISTS makes initialize() safe to call repeatedly;
// existing rows must survive a re-init.
TEST(PgRolloutStore, SchemaMigrationIsIdempotent) {
    const char* url = std::getenv("AEGISGATE_PG_URL");
    if (!url) GTEST_SKIP() << "AEGISGATE_PG_URL not set";

    pgCleanRollout(url, "pg-idem-%");

    {
        PgConfig cfg; cfg.url = url; cfg.pool_size = 1;
        PgPersistentStore s1(cfg);
        ASSERT_TRUE(s1.initialize());
        // Insert directly — Epic 2.1 insertRollout API isn't required for
        // this test (it only verifies schema idempotency).
        PGconn* conn = PQconnectdb(url);
        ASSERT_EQ(PQstatus(conn), CONNECTION_OK);
        PGresult* r = PQexec(conn,
            "INSERT INTO rollouts (rollout_id, target_version_id, "
            "previous_active_version_id, spec_json, status, "
            "current_stage_index, started_at, stage_started_at, "
            "paused_at, pause_reason, pause_detail, creator, last_actor, "
            "completed_at, chain_hash) VALUES "
            "('pg-idem-R1', 'V1', '', E'\\\\x7b7d', 1, 0, 1000, 0, 0, 0, "
            "'', 'alice', '', 0, '')");
        EXPECT_EQ(PQresultStatus(r), PGRES_COMMAND_OK)
            << PQresultErrorMessage(r);
        PQclear(r);
        PQfinish(conn);
    }

    {
        PgConfig cfg; cfg.url = url; cfg.pool_size = 1;
        PgPersistentStore s2(cfg);
        ASSERT_TRUE(s2.initialize()) << "schema migration must be idempotent";

        PGconn* conn = PQconnectdb(url);
        ASSERT_EQ(PQstatus(conn), CONNECTION_OK);
        PGresult* res = PQexec(conn,
            "SELECT target_version_id FROM rollouts "
            "WHERE rollout_id = 'pg-idem-R1'");
        ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
        ASSERT_EQ(PQntuples(res), 1);
        EXPECT_STREQ(PQgetvalue(res, 0, 0), "V1");
        PQclear(res);
        PQfinish(conn);
    }

    pgCleanRollout(url, "pg-idem-%");
}

// Epic 2.1+2.3 — insert + get round-trip with binary spec_json. Also covers
// rejection of empty rollout_id.
TEST(PgRolloutStore, InsertAndGetRolloutRoundtrip) {
    const char* url = std::getenv("AEGISGATE_PG_URL");
    if (!url) GTEST_SKIP() << "AEGISGATE_PG_URL not set";
    PgConfig cfg; cfg.url = url; cfg.pool_size = 2;
    PgPersistentStore store(cfg);
    ASSERT_TRUE(store.initialize());
    pgCleanRollout(url, "pg-ins-%");

    RolloutRecord r{};
    r.rollout_id = "pg-ins-R1";
    r.target_version_id = "V1";
    r.spec.target_version_id = "V1";
    r.spec.sticky_key = "tenant_id";
    r.spec.auto_rollback_on_pause = true;
    r.spec.auto_rollback_grace_seconds = 1800;
    RolloutStageRecord s; s.name = "1pct"; s.scope.percentage = 1;
    s.scope.tenant_globs = {"internal-*"};
    r.spec.stages.push_back(std::move(s));
    r.status = RolloutStatus::PENDING;
    r.creator = "alice";
    ASSERT_TRUE(store.insertRollout(r));

    auto got = store.getRollout("pg-ins-R1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->target_version_id, "V1");
    EXPECT_EQ(got->spec.sticky_key, "tenant_id");
    EXPECT_TRUE(got->spec.auto_rollback_on_pause);
    EXPECT_EQ(got->spec.auto_rollback_grace_seconds, 1800);
    ASSERT_EQ(got->spec.stages.size(), 1u);
    EXPECT_EQ(got->spec.stages[0].name, "1pct");
    EXPECT_EQ(got->spec.stages[0].scope.percentage, 1);
    ASSERT_EQ(got->spec.stages[0].scope.tenant_globs.size(), 1u);
    EXPECT_EQ(got->spec.stages[0].scope.tenant_globs[0], "internal-*");
    EXPECT_EQ(got->status, RolloutStatus::PENDING);
    EXPECT_EQ(got->creator, "alice");

    // getRollout for missing id returns nullopt
    EXPECT_FALSE(store.getRollout("pg-ins-nope").has_value());
    // empty id rejected at API level
    EXPECT_FALSE(store.getRollout("").has_value());

    // Empty rollout_id rejected by insertRollout
    RolloutRecord empty{}; empty.target_version_id = "V1";
    EXPECT_FALSE(store.insertRollout(empty));

    pgCleanRollout(url, "pg-ins-%");
}

// SR14 + SR18 — schema-level partial UNIQUE INDEX rejects a second
// non-terminal rollout on the same target_version_id even when
// application-level guards have been bypassed.
TEST(PgRolloutStore, InsertSecondActiveOnSameTargetRejected) {
    const char* url = std::getenv("AEGISGATE_PG_URL");
    if (!url) GTEST_SKIP() << "AEGISGATE_PG_URL not set";
    PgConfig cfg; cfg.url = url; cfg.pool_size = 2;
    PgPersistentStore store(cfg);
    ASSERT_TRUE(store.initialize());
    pgCleanRollout(url, "pg-uniq-%");

    auto mk = [](const std::string& id, RolloutStatus st) {
        RolloutRecord r{};
        r.rollout_id = id; r.target_version_id = "V1";
        r.spec.target_version_id = "V1";
        RolloutStageRecord s; s.name = "x"; s.scope.percentage = 1;
        r.spec.stages.push_back(s);
        r.status = st; r.creator = "alice";
        return r;
    };

    ASSERT_TRUE(store.insertRollout(mk("pg-uniq-R1", RolloutStatus::PROGRESSING)));
    EXPECT_FALSE(store.insertRollout(mk("pg-uniq-R2", RolloutStatus::PENDING)));
    EXPECT_FALSE(store.insertRollout(mk("pg-uniq-R3", RolloutStatus::PAUSED)));

    // Terminal-state row for the same target is allowed (partial index does
    // not cover COMPLETED/FAILED/ABORTED).
    EXPECT_TRUE(store.insertRollout(mk("pg-uniq-T1", RolloutStatus::COMPLETED)));

    pgCleanRollout(url, "pg-uniq-%");
}

// Epic 2.2 — UPDATE ... RETURNING rollout_id makes "row exists?" check
// atomic with the write. PQntuples()==1 ⇔ row was found and updated.
TEST(PgRolloutStore, UpdateRolloutPersistsAndReturnsCorrectly) {
    const char* url = std::getenv("AEGISGATE_PG_URL");
    if (!url) GTEST_SKIP() << "AEGISGATE_PG_URL not set";
    PgConfig cfg; cfg.url = url; cfg.pool_size = 2;
    PgPersistentStore store(cfg);
    ASSERT_TRUE(store.initialize());
    pgCleanRollout(url, "pg-upd-%");

    RolloutRecord r{};
    r.rollout_id = "pg-upd-R1"; r.target_version_id = "V1";
    r.spec.target_version_id = "V1";
    RolloutStageRecord s; s.name = "x"; s.scope.percentage = 1;
    r.spec.stages.push_back(s);
    r.status = RolloutStatus::PENDING;
    r.creator = "alice";
    ASSERT_TRUE(store.insertRollout(r));

    // Mutate -> persist -> verify
    r.status = RolloutStatus::PROGRESSING;
    r.last_actor = "bob";
    r.started_at = 12345;
    r.stage_started_at = 12345;
    EXPECT_TRUE(store.updateRollout(r));

    auto got = store.getRollout("pg-upd-R1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, RolloutStatus::PROGRESSING);
    EXPECT_EQ(got->last_actor, "bob");
    EXPECT_EQ(got->started_at, 12345);

    // Update on a non-existent id returns false (RETURNING rollout_id
    // produces 0 rows on miss).
    RolloutRecord nope = r;
    nope.rollout_id = "pg-upd-nonexistent";
    EXPECT_FALSE(store.updateRollout(nope));

    // Empty id rejected at API level
    RolloutRecord empty = r; empty.rollout_id = "";
    EXPECT_FALSE(store.updateRollout(empty));

    pgCleanRollout(url, "pg-upd-%");
}

// Epic 2.4 — find by target with status ∈ (PENDING, PROGRESSING, PAUSED).
// The IN-list mirrors the partial UNIQUE INDEX so the planner can use it for
// both lookup and the SR14 invariant.
TEST(PgRolloutStore, FindActiveRolloutByTargetSemantics) {
    const char* url = std::getenv("AEGISGATE_PG_URL");
    if (!url) GTEST_SKIP() << "AEGISGATE_PG_URL not set";
    PgConfig cfg; cfg.url = url; cfg.pool_size = 2;
    PgPersistentStore store(cfg);
    ASSERT_TRUE(store.initialize());
    pgCleanRollout(url, "pg-fnd-%");

    RolloutRecord r1{};
    r1.rollout_id = "pg-fnd-R1"; r1.target_version_id = "V1";
    r1.spec.target_version_id = "V1";
    RolloutStageRecord s; s.name = "x"; s.scope.percentage = 1;
    r1.spec.stages.push_back(s);
    r1.status = RolloutStatus::PROGRESSING;
    r1.creator = "alice"; r1.started_at = 1000;
    ASSERT_TRUE(store.insertRollout(r1));

    auto found = store.findActiveRolloutByTarget("V1");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->rollout_id, "pg-fnd-R1");

    // Different target ⇒ nullopt
    EXPECT_FALSE(store.findActiveRolloutByTarget("V_other").has_value());
    // Empty target rejected at API layer
    EXPECT_FALSE(store.findActiveRolloutByTarget("").has_value());

    // Promote to terminal -> active slot freed
    r1.status = RolloutStatus::COMPLETED; r1.completed_at = 2000;
    ASSERT_TRUE(store.updateRollout(r1));
    EXPECT_FALSE(store.findActiveRolloutByTarget("V1").has_value());

    // After freeing, a new active row on V1 is allowed and findable
    RolloutRecord r2 = r1;
    r2.rollout_id = "pg-fnd-R2"; r2.status = RolloutStatus::PAUSED;
    r2.completed_at = 0;
    ASSERT_TRUE(store.insertRollout(r2));
    auto found2 = store.findActiveRolloutByTarget("V1");
    ASSERT_TRUE(found2.has_value());
    EXPECT_EQ(found2->rollout_id, "pg-fnd-R2");

    pgCleanRollout(url, "pg-fnd-%");
}

// Epic 2.5 — ordering, status filter, and cursor pagination. The pagination
// pattern is two round-trips: first SELECT started_at FROM rollouts WHERE
// rollout_id = $page_token; then SELECT rows where (started_at, rollout_id)
// is strictly less than the token tuple under DESC ordering.
TEST(PgRolloutStore, ListRolloutsOrderingAndPagination) {
    const char* url = std::getenv("AEGISGATE_PG_URL");
    if (!url) GTEST_SKIP() << "AEGISGATE_PG_URL not set";
    PgConfig cfg; cfg.url = url; cfg.pool_size = 2;
    PgPersistentStore store(cfg);
    ASSERT_TRUE(store.initialize());
    pgCleanRollout(url, "pg-lst-%");

    for (int i = 1; i <= 5; ++i) {
        RolloutRecord r{};
        r.rollout_id = std::string("pg-lst-R") + std::to_string(i);
        r.target_version_id = std::string("V") + std::to_string(i);
        r.spec.target_version_id = r.target_version_id;
        RolloutStageRecord s; s.name = "x"; s.scope.percentage = 1;
        r.spec.stages.push_back(s);
        r.status = RolloutStatus::COMPLETED;
        r.creator = "alice";
        r.started_at = i * 1000;
        ASSERT_TRUE(store.insertRollout(r));
    }

    auto out = store.listRollouts({});
    std::vector<RolloutRecord> ours;
    for (auto& r : out) {
        if (r.rollout_id.rfind("pg-lst-", 0) == 0) ours.push_back(r);
    }
    ASSERT_EQ(ours.size(), 5u);
    EXPECT_EQ(ours[0].rollout_id, "pg-lst-R5");
    EXPECT_EQ(ours[4].rollout_id, "pg-lst-R1");

    RolloutQuery q; q.limit = 2;
    auto p1 = store.listRollouts(q);
    std::vector<RolloutRecord> p1ours;
    for (auto& r : p1) {
        if (r.rollout_id.rfind("pg-lst-", 0) == 0) p1ours.push_back(r);
    }
    ASSERT_GE(p1ours.size(), 2u);
    EXPECT_EQ(p1ours[0].rollout_id, "pg-lst-R5");
    EXPECT_EQ(p1ours[1].rollout_id, "pg-lst-R4");

    q.page_token = p1ours[1].rollout_id;
    auto p2 = store.listRollouts(q);
    std::vector<RolloutRecord> p2ours;
    for (auto& r : p2) {
        if (r.rollout_id.rfind("pg-lst-", 0) == 0) p2ours.push_back(r);
    }
    ASSERT_GE(p2ours.size(), 2u);
    EXPECT_EQ(p2ours[0].rollout_id, "pg-lst-R3");
    EXPECT_EQ(p2ours[1].rollout_id, "pg-lst-R2");

    // Status filter
    RolloutQuery sq; sq.statuses = {RolloutStatus::COMPLETED};
    auto sout = store.listRollouts(sq);
    bool all_completed = true;
    for (auto& r : sout) {
        if (r.status != RolloutStatus::COMPLETED) { all_completed = false; break; }
    }
    EXPECT_TRUE(all_completed);

    pgCleanRollout(url, "pg-lst-%");
}

// Epic 2.6 — append events with both empty (→ NULL) and binary (NUL byte
// inside) metrics_json payloads. SR18 binding via paramFormats[5]=1
// preserves byte-exact contents through the BYTEA column.
TEST(PgRolloutStore, AppendStageEventWithBinaryMetrics) {
    const char* url = std::getenv("AEGISGATE_PG_URL");
    if (!url) GTEST_SKIP() << "AEGISGATE_PG_URL not set";
    PgConfig cfg; cfg.url = url; cfg.pool_size = 2;
    PgPersistentStore store(cfg);
    ASSERT_TRUE(store.initialize());
    pgCleanRollout(url, "pg-evt-%");

    // Empty metrics_json → NULL column
    RolloutStageEvent e1{};
    e1.event_id = "pg-evt-E1"; e1.rollout_id = "pg-evt-R1";
    e1.stage_index = 0; e1.event_type = "entered";
    e1.at_millis = 1000; e1.actor = "system";
    EXPECT_TRUE(store.appendRolloutStageEvent(e1));

    // Binary metrics_json with NUL byte
    RolloutStageEvent e2{};
    e2.event_id = "pg-evt-E2"; e2.rollout_id = "pg-evt-R1";
    e2.stage_index = 1; e2.event_type = "promoted";
    const char raw[] = {'{', '"', 'x', '"', ':', '\0', '1', '}'};
    e2.metrics_json.assign(raw, raw + sizeof(raw));
    e2.at_millis = 2000; e2.actor = "system";
    EXPECT_TRUE(store.appendRolloutStageEvent(e2));

    // Empty event_id rejected
    RolloutStageEvent bad{};
    bad.rollout_id = "pg-evt-R1"; bad.event_type = "x"; bad.actor = "x";
    EXPECT_FALSE(store.appendRolloutStageEvent(bad));

    // Empty rollout_id rejected
    RolloutStageEvent bad2{};
    bad2.event_id = "pg-evt-Ebad"; bad2.event_type = "x"; bad2.actor = "x";
    EXPECT_FALSE(store.appendRolloutStageEvent(bad2));

    pgCleanRollout(url, "pg-evt-%");
}

// Epic 2.7 — list events ascending by at_millis, then event_id; rollout
// isolation; binary metrics round-trip; missing rollout returns empty.
TEST(PgRolloutStore, ListStageEventsOrderedAscAndIsolated) {
    const char* url = std::getenv("AEGISGATE_PG_URL");
    if (!url) GTEST_SKIP() << "AEGISGATE_PG_URL not set";
    PgConfig cfg; cfg.url = url; cfg.pool_size = 2;
    PgPersistentStore store(cfg);
    ASSERT_TRUE(store.initialize());
    pgCleanRollout(url, "pg-le-%");

    auto mkE = [](const std::string& id, const std::string& rid, int idx,
                   const std::string& typ, std::int64_t at) {
        RolloutStageEvent e{};
        e.event_id = id; e.rollout_id = rid; e.stage_index = idx;
        e.event_type = typ; e.at_millis = at; e.actor = "x";
        return e;
    };
    ASSERT_TRUE(store.appendRolloutStageEvent(mkE("pg-le-E1", "pg-le-R1", 0, "entered",  1000)));
    ASSERT_TRUE(store.appendRolloutStageEvent(mkE("pg-le-E2", "pg-le-R1", 1, "promoted", 3000)));
    ASSERT_TRUE(store.appendRolloutStageEvent(mkE("pg-le-E3", "pg-le-R1", 1, "paused",   2000)));
    ASSERT_TRUE(store.appendRolloutStageEvent(mkE("pg-le-E4", "pg-le-R2", 0, "entered",  1000)));

    auto r1 = store.listRolloutStageEvents("pg-le-R1");
    ASSERT_EQ(r1.size(), 3u);
    EXPECT_EQ(r1[0].event_id, "pg-le-E1");
    EXPECT_EQ(r1[1].event_id, "pg-le-E3");
    EXPECT_EQ(r1[2].event_id, "pg-le-E2");

    auto r2 = store.listRolloutStageEvents("pg-le-R2");
    ASSERT_EQ(r2.size(), 1u);
    EXPECT_EQ(r2[0].event_id, "pg-le-E4");

    EXPECT_TRUE(store.listRolloutStageEvents("pg-le-RX").empty());
    EXPECT_TRUE(store.listRolloutStageEvents("").empty());

    pgCleanRollout(url, "pg-le-%");
}

// Epic 3.2 — SR14 schema-level UNIQUE INDEX defense-in-depth.
//
// Bypasses the application layer (PgPersistentStore::insertRollout) and
// goes through a raw libpq connection to INSERT a second active rollout on
// the same target_version_id. The partial UNIQUE INDEX
// rollouts_one_active_per_target must reject the write at the schema level
// with SQLSTATE 23505 (unique_violation), proving that a tampering attacker
// who skirts the application path is still caught by the database.
TEST(PgRolloutStore, UniqueActiveRolloutIndexEnforcedAtSchemaLevel) {
    const char* url = std::getenv("AEGISGATE_PG_URL");
    if (!url) GTEST_SKIP() << "AEGISGATE_PG_URL not set";

    PgConfig cfg; cfg.url = url; cfg.pool_size = 2;
    PgPersistentStore store(cfg);
    ASSERT_TRUE(store.initialize());
    pgCleanRollout(url, "pg-uniqraw-%");

    // Seed: insert the first active rollout via the application API.
    RolloutRecord r1{};
    r1.rollout_id = "pg-uniqraw-R1"; r1.target_version_id = "V_RAW";
    r1.spec.target_version_id = "V_RAW";
    RolloutStageRecord s; s.name = "x"; s.scope.percentage = 1;
    r1.spec.stages.push_back(s);
    r1.status = RolloutStatus::PROGRESSING;
    r1.creator = "alice"; r1.started_at = 1000;
    ASSERT_TRUE(store.insertRollout(r1));

    // Adversary path: raw libpq connection, bypassing the application
    // layer's findActiveRolloutByTarget guard. status=2 (PROGRESSING) is
    // covered by the partial UNIQUE INDEX → must be rejected.
    PGconn* conn = PQconnectdb(url);
    ASSERT_EQ(PQstatus(conn), CONNECTION_OK);
    const char* raw_sql =
        "INSERT INTO rollouts (rollout_id, target_version_id, "
        "previous_active_version_id, spec_json, status, current_stage_index, "
        "started_at, stage_started_at, paused_at, pause_reason, pause_detail, "
        "creator, last_actor, completed_at, chain_hash) "
        "VALUES ('pg-uniqraw-R2', 'V_RAW', '', E'\\\\x7b7d', 2, 0, 2000, "
        "0, 0, 0, '', 'mallory', '', 0, '')";
    PGresult* res = PQexec(conn, raw_sql);
    EXPECT_NE(PQresultStatus(res), PGRES_COMMAND_OK)
        << "schema-level UNIQUE INDEX must reject second active rollout";
    const char* sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
    ASSERT_NE(sqlstate, nullptr);
    EXPECT_STREQ(sqlstate, "23505")
        << "expected SQLSTATE 23505 (unique_violation) but got "
        << sqlstate;
    PQclear(res);
    PQfinish(conn);

    pgCleanRollout(url, "pg-uniqraw-%");
}

// ---------------------------------------------------------------------------
// Epic 3.3 — PG rollout integration fixture (4 cases)
//
// End-to-end exercises against the real PG backend, covering:
//   1. CRUD round-trip with binary spec_json (NUL byte preserved)
//   2. at-most-one ACTIVE invariant across PROGRESSING → COMPLETED
//   3. Concurrent insert race — exactly one of N threads wins
//   4. ~900 KiB spec_json round-trip without truncation
// ---------------------------------------------------------------------------
class PgRolloutIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* url = std::getenv("AEGISGATE_PG_URL");
        if (!url) GTEST_SKIP() << "AEGISGATE_PG_URL not set";
        PgConfig cfg; cfg.url = url; cfg.pool_size = 4;
        store_ = std::make_unique<PgPersistentStore>(cfg);
        ASSERT_TRUE(store_->initialize());
        cleanup();
    }

    void TearDown() override {
        if (store_) {
            cleanup();
            store_->close();
        }
    }

    void cleanup() {
        const char* url = std::getenv("AEGISGATE_PG_URL");
        if (!url) return;
        pgCleanRollout(url, "pg-itr-%");
    }

    std::unique_ptr<PgPersistentStore> store_;
};

TEST_F(PgRolloutIntegrationTest, FullCrudRoundtripWithBinaryPayload) {
    RolloutRecord r{};
    r.rollout_id = "pg-itr-R1";
    r.target_version_id = "V_ITR_1";
    r.spec.target_version_id = "V_ITR_1";
    // Force NUL byte into the JSON BLOB via creator_comment so we exercise
    // the full BYTEA path: serializeRolloutSpec → BYTEA wire → parse →
    // POCO and confirm the byte survives.
    r.spec.creator_comment = std::string("a\0b", 3);
    RolloutStageRecord s; s.name = "1pct"; s.scope.percentage = 1;
    r.spec.stages.push_back(s);
    r.status = RolloutStatus::PENDING;
    r.creator = "alice";
    ASSERT_TRUE(store_->insertRollout(r));

    auto got = store_->getRollout("pg-itr-R1");
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(got->spec.creator_comment.size(), 3u);
    EXPECT_EQ(got->spec.creator_comment[0], 'a');
    EXPECT_EQ(got->spec.creator_comment[1], '\0');
    EXPECT_EQ(got->spec.creator_comment[2], 'b');

    // Update + read again
    r.status = RolloutStatus::PROGRESSING;
    r.last_actor = "bob";
    EXPECT_TRUE(store_->updateRollout(r));

    got = store_->getRollout("pg-itr-R1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, RolloutStatus::PROGRESSING);
    EXPECT_EQ(got->last_actor, "bob");
}

TEST_F(PgRolloutIntegrationTest, ActiveInvariantAcrossLifecycle) {
    auto mk = [](const std::string& id, RolloutStatus st) {
        RolloutRecord r{};
        r.rollout_id = id; r.target_version_id = "V_ITR_2";
        r.spec.target_version_id = "V_ITR_2";
        RolloutStageRecord s; s.name = "x"; s.scope.percentage = 1;
        r.spec.stages.push_back(s);
        r.status = st; r.creator = "alice";
        return r;
    };
    ASSERT_TRUE(store_->insertRollout(mk("pg-itr-A1", RolloutStatus::PROGRESSING)));
    EXPECT_FALSE(store_->insertRollout(mk("pg-itr-A2", RolloutStatus::PENDING)));

    // Promote A1 to terminal -> active slot freed
    auto a1 = store_->getRollout("pg-itr-A1");
    ASSERT_TRUE(a1.has_value());
    a1->status = RolloutStatus::COMPLETED;
    a1->completed_at = 9999;
    ASSERT_TRUE(store_->updateRollout(*a1));

    EXPECT_TRUE(store_->insertRollout(mk("pg-itr-A2", RolloutStatus::PENDING)));

    auto found = store_->findActiveRolloutByTarget("V_ITR_2");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->rollout_id, "pg-itr-A2");
}

TEST_F(PgRolloutIntegrationTest, ConcurrentInsertRaceOnSameTarget) {
    constexpr int kThreads = 8;
    std::atomic<int> wins{0};
    std::vector<std::thread> th;
    th.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        th.emplace_back([&, i]() {
            RolloutRecord r{};
            r.rollout_id = "pg-itr-race-" + std::to_string(i);
            r.target_version_id = "V_ITR_RACE";
            r.spec.target_version_id = "V_ITR_RACE";
            RolloutStageRecord s; s.name = "x"; s.scope.percentage = 1;
            r.spec.stages.push_back(s);
            r.status = RolloutStatus::PROGRESSING;
            r.creator = "alice";
            if (store_->insertRollout(r)) {
                wins.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : th) t.join();
    EXPECT_EQ(wins.load(), 1)
        << "exactly one of " << kThreads
        << " concurrent inserts should win the partial UNIQUE INDEX race";
}

TEST_F(PgRolloutIntegrationTest, LargeSpecJsonRoundtrip) {
    RolloutRecord r{};
    r.rollout_id = "pg-itr-big";
    r.target_version_id = "V_ITR_BIG";
    r.spec.target_version_id = "V_ITR_BIG";
    // Pad creator_comment to ~900 KiB (under the 1 MiB cap RolloutController
    // enforces upstream). This validates that PG BYTEA + libpq binary
    // binding handle multi-block payloads end to end.
    r.spec.creator_comment = std::string(900 * 1024, 'A');
    RolloutStageRecord s; s.name = "1pct"; s.scope.percentage = 1;
    r.spec.stages.push_back(s);
    r.status = RolloutStatus::PENDING;
    r.creator = "alice";
    ASSERT_TRUE(store_->insertRollout(r));

    auto got = store_->getRollout("pg-itr-big");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->spec.creator_comment.size(), 900u * 1024u);
    EXPECT_EQ(got->spec.creator_comment[0], 'A');
    EXPECT_EQ(got->spec.creator_comment[900 * 1024 - 1], 'A');
}
