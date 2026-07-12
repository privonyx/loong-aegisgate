#include "server/admin_controller.h"
#include "auth/auth_service.h"
#include "guardrail/audit.h"
#include "storage/memory_persistent_store.h"
#include <gtest/gtest.h>

using namespace aegisgate;
using json = nlohmann::json;

class AdminControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_.initialize();
        gate_ = std::make_unique<FeatureGate>(FeatureGate::createUnlocked(Edition::Enterprise));
        auth_svc_ = std::make_unique<AuthService>(&store_, nullptr, gate_.get());
        audit_ = std::make_unique<AuditLogger>();
        ctrl_ = std::make_unique<AdminController>(&store_, auth_svc_.get(), audit_.get());

        super_ctx_.role = Role::SuperAdmin;
        super_ctx_.tenant_id = "t-super";
        super_ctx_.is_rbac_enabled = true;

        admin_ctx_.role = Role::TenantAdmin;
        admin_ctx_.tenant_id = "t1";
        admin_ctx_.is_rbac_enabled = true;

        dev_ctx_.role = Role::Developer;
        dev_ctx_.tenant_id = "t1";
        dev_ctx_.is_rbac_enabled = true;

        viewer_ctx_.role = Role::Viewer;
        viewer_ctx_.tenant_id = "t1";
        viewer_ctx_.is_rbac_enabled = true;
    }

    void TearDown() override {
        if (audit_) audit_->shutdown();
    }

    MemoryPersistentStore store_;
    std::unique_ptr<FeatureGate> gate_;
    std::unique_ptr<AuthService> auth_svc_;
    std::unique_ptr<AuditLogger> audit_;
    std::unique_ptr<AdminController> ctrl_;
    AuthContext super_ctx_, admin_ctx_, dev_ctx_, viewer_ctx_;
};

// === Tenant CRUD (Task 7) ===

TEST_F(AdminControllerTest, CreateTenantAsSuperAdmin) {
    auto r = ctrl_->createTenant(super_ctx_, {{"name", "Acme"}});
    EXPECT_EQ(r.status, 201);
    EXPECT_FALSE(r.is_error);
    EXPECT_EQ(r.body["name"], "Acme");
    EXPECT_FALSE(r.body["id"].get<std::string>().empty());
}

TEST_F(AdminControllerTest, CreateTenantMissingName) {
    auto r = ctrl_->createTenant(super_ctx_, json::object());
    EXPECT_EQ(r.status, 400);
    EXPECT_TRUE(r.is_error);
}

TEST_F(AdminControllerTest, CreateTenantDuplicateName) {
    ctrl_->createTenant(super_ctx_, {{"name", "Dup"}});
    auto r = ctrl_->createTenant(super_ctx_, {{"name", "Dup"}});
    EXPECT_EQ(r.status, 409);
}

TEST_F(AdminControllerTest, CreateTenantAsDeveloper) {
    auto r = ctrl_->createTenant(dev_ctx_, {{"name", "Nope"}});
    EXPECT_EQ(r.status, 403);
}

TEST_F(AdminControllerTest, ListTenants) {
    ctrl_->createTenant(super_ctx_, {{"name", "A"}});
    ctrl_->createTenant(super_ctx_, {{"name", "B"}});
    auto r = ctrl_->listTenants(super_ctx_, 100, 0);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["data"].size(), 2u);
}

TEST_F(AdminControllerTest, UpdateTenant) {
    auto cr = ctrl_->createTenant(super_ctx_, {{"name", "Old"}});
    auto id = cr.body["id"].get<std::string>();
    auto r = ctrl_->updateTenant(super_ctx_, id, {{"name", "New"}, {"model_whitelist", {"gpt-4"}}});
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["name"], "New");
    EXPECT_EQ(r.body["model_whitelist"].size(), 1u);
}

TEST_F(AdminControllerTest, DeleteTenant) {
    auto cr = ctrl_->createTenant(super_ctx_, {{"name", "Del"}});
    auto id = cr.body["id"].get<std::string>();
    auto r = ctrl_->deleteTenant(super_ctx_, id);
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(r.body["deleted"].get<bool>());
}

// === User CRUD (Task 8) ===

TEST_F(AdminControllerTest, CreateUserAsTenantAdmin) {
    Tenant t; t.id = "t1"; t.name = "T1"; store_.insertTenant(t);

    auto r = ctrl_->createUser(admin_ctx_, {{"username", "alice"}, {"role", "developer"}});
    EXPECT_EQ(r.status, 201);
    EXPECT_EQ(r.body["username"], "alice");
    EXPECT_EQ(r.body["role"], "developer");
    EXPECT_EQ(r.body["tenant_id"], "t1");
}

TEST_F(AdminControllerTest, CreateUserAsDeveloperDenied) {
    Tenant t; t.id = "t1"; t.name = "T1"; store_.insertTenant(t);

    auto r = ctrl_->createUser(dev_ctx_, {{"username", "bob"}});
    EXPECT_EQ(r.status, 403);
}

TEST_F(AdminControllerTest, CreateUserCrossTenantDenied) {
    Tenant t; t.id = "t2"; t.name = "T2"; store_.insertTenant(t);

    auto r = ctrl_->createUser(admin_ctx_, {{"username", "alice"}, {"tenant_id", "t2"}});
    EXPECT_EQ(r.status, 403);
}

TEST_F(AdminControllerTest, CreateUserDuplicateUsername) {
    Tenant t; t.id = "t1"; t.name = "T1"; store_.insertTenant(t);

    ctrl_->createUser(admin_ctx_, {{"username", "alice"}});
    auto r = ctrl_->createUser(admin_ctx_, {{"username", "alice"}});
    EXPECT_EQ(r.status, 409);
}

TEST_F(AdminControllerTest, ListUsersFilterByTenant) {
    Tenant t1; t1.id = "t1"; t1.name = "T1"; store_.insertTenant(t1);
    Tenant t2; t2.id = "t2"; t2.name = "T2"; store_.insertTenant(t2);

    ctrl_->createUser(admin_ctx_, {{"username", "a"}});

    AuthContext admin2 = admin_ctx_;
    admin2.tenant_id = "t2";
    ctrl_->createUser(admin2, {{"username", "b"}});

    auto r1 = ctrl_->listUsers(admin_ctx_, "t1", 100, 0);
    EXPECT_EQ(r1.body["count"], 1);

    auto r_all = ctrl_->listUsers(super_ctx_, "", 100, 0);
    EXPECT_EQ(r_all.body["count"], 2);
}

// === API Key management (Task 9) ===

TEST_F(AdminControllerTest, CreateApiKey) {
    Tenant t; t.id = "t1"; t.name = "T1"; store_.insertTenant(t);
    User u; u.id = "u1"; u.tenant_id = "t1"; u.username = "alice"; u.role = Role::Developer;
    store_.insertUser(u);

    auto r = ctrl_->createApiKey(admin_ctx_, {{"user_id", "u1"}, {"name", "dev-key"}});
    EXPECT_EQ(r.status, 201);
    EXPECT_TRUE(r.body.contains("key"));
    auto key = r.body["key"].get<std::string>();
    EXPECT_EQ(key.substr(0, 3), "sk-");
    EXPECT_EQ(r.body["key_prefix"].get<std::string>().size(), 8u);
}

TEST_F(AdminControllerTest, ListApiKeysNoHash) {
    Tenant t; t.id = "t1"; t.name = "T1"; store_.insertTenant(t);
    User u; u.id = "u1"; u.tenant_id = "t1"; u.username = "alice"; store_.insertUser(u);

    ctrl_->createApiKey(admin_ctx_, {{"user_id", "u1"}});
    auto r = ctrl_->listApiKeys(admin_ctx_, "t1", 100, 0);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["data"].size(), 1u);
    EXPECT_FALSE(r.body["data"][0].contains("key_hash"));
}

TEST_F(AdminControllerTest, RevokeApiKey) {
    Tenant t; t.id = "t1"; t.name = "T1"; store_.insertTenant(t);
    User u; u.id = "u1"; u.tenant_id = "t1"; u.username = "alice"; store_.insertUser(u);

    auto cr = ctrl_->createApiKey(admin_ctx_, {{"user_id", "u1"}});
    auto key_id = cr.body["id"].get<std::string>();
    auto raw_key = cr.body["key"].get<std::string>();

    auto r = ctrl_->revokeApiKey(admin_ctx_, key_id);
    EXPECT_EQ(r.status, 200);

    auto resolved = auth_svc_->resolve(raw_key);
    EXPECT_FALSE(resolved.has_value());
}

TEST_F(AdminControllerTest, RotateApiKey) {
    Tenant t; t.id = "t1"; t.name = "T1"; store_.insertTenant(t);
    User u; u.id = "u1"; u.tenant_id = "t1"; u.username = "alice"; store_.insertUser(u);

    auto cr = ctrl_->createApiKey(admin_ctx_, {{"user_id", "u1"}});
    auto old_id = cr.body["id"].get<std::string>();
    auto old_key = cr.body["key"].get<std::string>();

    auto r = ctrl_->rotateApiKey(admin_ctx_, old_id);
    EXPECT_EQ(r.status, 201);
    EXPECT_TRUE(r.body.contains("key"));
    EXPECT_EQ(r.body["rotated_from"], old_id);

    EXPECT_FALSE(auth_svc_->resolve(old_key).has_value());
    auto new_key = r.body["key"].get<std::string>();
    EXPECT_TRUE(auth_svc_->resolve(new_key).has_value());
}

TEST_F(AdminControllerTest, CreateApiKeyAsDeveloperDenied) {
    Tenant t; t.id = "t1"; t.name = "T1"; store_.insertTenant(t);
    User u; u.id = "u1"; u.tenant_id = "t1"; u.username = "alice"; store_.insertUser(u);

    auto r = ctrl_->createApiKey(dev_ctx_, {{"user_id", "u1"}});
    EXPECT_EQ(r.status, 403);
}

// === Audit & Cost queries (Task 10) ===

TEST_F(AdminControllerTest, QueryAuditsAsViewer) {
    AuditEntry a;
    a.request_id = "r1"; a.tenant_id = "t1"; a.timestamp = "2026-03-22T10:00:00Z";
    a.action = "chat"; a.stage_name = "inbound";
    store_.insertAudit(a);

    auto r = ctrl_->queryAudits(viewer_ctx_, "t1", 100, 0);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["count"], 1);
}

TEST_F(AdminControllerTest, QueryAuditsCrossTenantDenied) {
    AuditEntry a; a.request_id = "r1"; a.tenant_id = "t2"; a.timestamp = "2026-03-22T10:00:00Z";
    store_.insertAudit(a);

    auto r = ctrl_->queryAudits(viewer_ctx_, "t2", 100, 0);
    EXPECT_EQ(r.status, 403);
}

TEST_F(AdminControllerTest, QueryAuditsCrossTenantSuperAdminOk) {
    AuditEntry a; a.request_id = "r1"; a.tenant_id = "t2"; a.timestamp = "2026-03-22T10:00:00Z";
    store_.insertAudit(a);

    auto r = ctrl_->queryAudits(super_ctx_, "t2", 100, 0);
    EXPECT_EQ(r.status, 200);
}

TEST_F(AdminControllerTest, QueryCosts) {
    CostRecord c;
    c.request_id = "r1"; c.tenant_id = "t1"; c.model = "gpt-4";
    c.total_cost = 0.05; c.timestamp = "2026-03-22T10:00:00Z";
    store_.insertCostRecord(c);

    auto r = ctrl_->queryCosts(viewer_ctx_, "t1", "", 100, 0);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["count"], 1);
}

TEST_F(AdminControllerTest, CrudOperationsWriteAuditLog) {
    auto before = audit_->entries().size();
    json body = {{"name", "audit-test-tenant"}};
    auto r = ctrl_->createTenant(super_ctx_, body);
    ASSERT_EQ(r.status, 201);

    auto entries = audit_->entries();
    ASSERT_GT(entries.size(), before);
    bool found = false;
    for (const auto& e : entries) {
        if (e.action == "cross_tenant:admin.create_tenant") { found = true; break; }
    }
    EXPECT_TRUE(found) << "Expected cross_tenant:admin.create_tenant audit entry";
}

TEST_F(AdminControllerTest, WriteOperationsGenerateAuditEntries) {
    auto before = audit_->entries().size();

    json tenant_body = {{"name", "audit-write-test"}};
    auto r1 = ctrl_->createTenant(super_ctx_, tenant_body);
    ASSERT_EQ(r1.status, 201);
    auto tid = r1.body.value("id", "");

    json user_body = {{"username", "audit_user"}, {"display_name", "Audit User"},
                      {"tenant_id", tid}};
    auto r2 = ctrl_->createUser(super_ctx_, user_body);
    ASSERT_EQ(r2.status, 201);

    auto entries = audit_->entries();
    ASSERT_GT(entries.size(), before + 1);

    bool found_tenant = false, found_user = false;
    for (size_t i = before; i < entries.size(); ++i) {
        if (entries[i].action == "cross_tenant:admin.create_tenant") found_tenant = true;
        if (entries[i].action == "admin.create_user") found_user = true;
    }
    EXPECT_TRUE(found_tenant) << "Expected cross_tenant:admin.create_tenant audit entry";
    EXPECT_TRUE(found_user) << "Expected admin.create_user audit entry";
}

TEST_F(AdminControllerTest, SsoProviderCrudGeneratesAuditEntries) {
    json provider_body = {
        {"name", "test-oidc"},
        {"issuer_url", "https://sso.example.com"},
        {"client_id", "test-client"},
        {"client_secret", "test-secret"},
        {"redirect_uri", "https://app.example.com/callback"},
        {"tenant_id", "t-super"}
    };

    auto before = audit_->entries().size();
    auto r = ctrl_->createSsoProvider(super_ctx_, provider_body);

    if (r.status >= 200 && r.status < 300) {
        auto entries = audit_->entries();
        bool found_create = false;
        for (size_t i = before; i < entries.size(); ++i) {
            if (entries[i].action == "admin.create_sso_provider") {
                found_create = true;
                break;
            }
        }
        EXPECT_TRUE(found_create) << "Expected admin.create_sso_provider audit entry";
    }
}

TEST_F(AdminControllerTest, DeleteTenantGeneratesAuditEntry) {
    json body = {{"name", "delete-audit-test"}};
    auto r = ctrl_->createTenant(super_ctx_, body);
    ASSERT_EQ(r.status, 201);
    auto tid = r.body.value("id", "");

    auto before = audit_->entries().size();
    auto r2 = ctrl_->deleteTenant(super_ctx_, tid);
    ASSERT_EQ(r2.status, 200);

    auto entries = audit_->entries();
    bool found = false;
    for (size_t i = before; i < entries.size(); ++i) {
        if (entries[i].action == "cross_tenant:admin.delete_tenant") { found = true; break; }
    }
    EXPECT_TRUE(found) << "Expected cross_tenant:admin.delete_tenant audit entry";
}

// === Pagination total (TASK-20260604-01 P0-E / SR-3) ===

TEST_F(AdminControllerTest, ListTenantsReturnsTotal) {
    ctrl_->createTenant(super_ctx_, {{"name", "T-A"}});
    ctrl_->createTenant(super_ctx_, {{"name", "T-B"}});
    ctrl_->createTenant(super_ctx_, {{"name", "T-C"}});

    auto r = ctrl_->listTenants(super_ctx_, 2, 0);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["count"], 2);            // 当前页
    EXPECT_GE(r.body["total"].get<int>(), 3); // 全量
}

TEST_F(AdminControllerTest, QueryCostsTotalIsTenantScopedForNonSuper) {
    // t1（viewer 所属）2 条，t2 1 条。
    auto mk = [&](const std::string& req, const std::string& tenant) {
        CostRecord c; c.request_id = req; c.tenant_id = tenant; c.model = "gpt-4";
        c.total_cost = 0.01; c.timestamp = "2026-06-04T10:00:00Z";
        store_.insertCostRecord(c);
    };
    mk("c1", "t1"); mk("c2", "t1"); mk("c3", "t2");

    // SR-3：非 super 的 total 仅本租户（不泄漏 t2）。
    auto r = ctrl_->queryCosts(viewer_ctx_, "", "", 100, 0);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["total"].get<int>(), 2);
    EXPECT_EQ(r.body["count"].get<int>(), 2);

    // SuperAdmin 不指定租户 → 全局 total。
    auto rs = ctrl_->queryCosts(super_ctx_, "", "", 100, 0);
    EXPECT_EQ(rs.body["total"].get<int>(), 3);
}

TEST_F(AdminControllerTest, QueryCostsPaginationWithTenantFilter) {
    for (int i = 0; i < 5; ++i) {
        CostRecord c; c.request_id = "r" + std::to_string(i);
        c.tenant_id = "t1"; c.model = "gpt-4"; c.total_cost = 0.01;
        c.timestamp = "2026-06-04T10:00:00Z"; store_.insertCostRecord(c);
    }
    CostRecord other; other.request_id = "x"; other.tenant_id = "t2";
    other.model = "gpt-4"; other.timestamp = "2026-06-04T10:00:00Z";
    store_.insertCostRecord(other);

    // 第二页（offset=3 limit=3）应只剩 t1 的 2 条，total 仍为 5。
    auto r = ctrl_->queryCosts(viewer_ctx_, "t1", "", 3, 3);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["count"].get<int>(), 2);
    EXPECT_EQ(r.body["total"].get<int>(), 5);
}

// === Rule set pagination total + offset (TASK-20260605-01 P0-E residual / SR-2) ===

TEST_F(AdminControllerTest, ListRuleSetsReturnsTotalAndOffset) {
    // 3 个版本（每次 createRuleSet 递增版本号）。
    for (int i = 0; i < 3; ++i)
        ctrl_->createRuleSet(admin_ctx_, {{"rules", json::array()}, {"tenant_id", "t1"}});

    auto r = ctrl_->listRuleSets(admin_ctx_, "t1", 2, 0);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["count"].get<int>(), 2);   // 当前页
    EXPECT_EQ(r.body["total"].get<int>(), 3);   // 全量

    // 第二页（offset=2 limit=2）应只剩 1 条，total 不变。
    auto r2 = ctrl_->listRuleSets(admin_ctx_, "t1", 2, 2);
    EXPECT_EQ(r2.body["count"].get<int>(), 1);
    EXPECT_EQ(r2.body["total"].get<int>(), 3);
}

TEST_F(AdminControllerTest, ListRuleSetsTotalTenantScopedForNonSuper) {
    // SR-2：非 super 的 total 仅本租户（不泄漏其它租户规则集计数）。
    ctrl_->createRuleSet(admin_ctx_, {{"rules", json::array()}, {"tenant_id", "t1"}});
    ctrl_->createRuleSet(admin_ctx_, {{"rules", json::array()}, {"tenant_id", "t1"}});
    ctrl_->createRuleSet(super_ctx_, {{"rules", json::array()}, {"tenant_id", "t2"}});

    auto r = ctrl_->listRuleSets(viewer_ctx_, "", 100, 0);  // viewer 属 t1，eff 强制 t1
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["total"].get<int>(), 2);   // 仅 t1，不含 t2
}

// === Compliance export (TASK-20260604-01 P0-C / SR-4) ===

TEST_F(AdminControllerTest, ExportAuditReportRequiresTenantAdmin) {
    // SR-4：Viewer / Developer 不得导出（最低 TenantAdmin）。
    EXPECT_EQ(ctrl_->exportAuditReport(viewer_ctx_, "2026-01-01", "2026-12-31", "t1", "csv").status, 403);
    EXPECT_EQ(ctrl_->exportAuditReport(dev_ctx_, "2026-01-01", "2026-12-31", "t1", "csv").status, 403);
}

TEST_F(AdminControllerTest, ExportCostReportRequiresTenantAdmin) {
    EXPECT_EQ(ctrl_->exportCostReport(viewer_ctx_, "2026-01-01", "2026-12-31", "t1", "csv").status, 403);
    EXPECT_EQ(ctrl_->exportCostReport(dev_ctx_, "2026-01-01", "2026-12-31", "t1", "csv").status, 403);
}

TEST_F(AdminControllerTest, ExportAuditReportTenantAdminCsvAndJson) {
    AuditEntry a; a.request_id = "r1"; a.tenant_id = "t1";
    a.timestamp = "2026-06-04T10:00:00Z"; a.action = "x"; a.stage_name = "s";
    store_.insertAudit(a);

    auto csv = ctrl_->exportAuditReport(admin_ctx_, "2026-01-01", "2026-12-31", "t1", "csv");
    EXPECT_EQ(csv.status, 200);
    EXPECT_EQ(csv.body["format"], "csv");

    auto js = ctrl_->exportAuditReport(admin_ctx_, "2026-01-01", "2026-12-31", "t1", "json");
    EXPECT_EQ(js.status, 200);
    EXPECT_EQ(js.body["format"], "json");
}

TEST_F(AdminControllerTest, ExportRejectsCrossTenant) {
    // SR-4：TenantAdmin 不得导出其他租户数据。
    auto r = ctrl_->exportAuditReport(admin_ctx_, "2026-01-01", "2026-12-31", "t-other", "csv");
    EXPECT_EQ(r.status, 403);
    auto r2 = ctrl_->exportCostReport(admin_ctx_, "2026-01-01", "2026-12-31", "t-other", "csv");
    EXPECT_EQ(r2.status, 403);
}

// === Cost JSON export (TASK-20260605-02 P0 / SR-1) ===

TEST_F(AdminControllerTest, ExportCostReportTenantAdminJson) {
    // P0：format=json 必须返回 JSON（605-01 前 (void)format 强制 CSV）。
    CostRecord c; c.request_id = "cjson"; c.model = "gpt-4"; c.tenant_id = "t1";
    c.timestamp = "2026-06-04T10:00:00Z"; c.total_cost = 0.05;
    store_.insertCostRecord(c);

    auto js = ctrl_->exportCostReport(admin_ctx_, "2026-01-01", "2026-12-31", "t1", "json");
    EXPECT_EQ(js.status, 200);
    EXPECT_EQ(js.body["format"], "json");
    ASSERT_TRUE(js.body["data"].is_array());

    auto csv = ctrl_->exportCostReport(admin_ctx_, "2026-01-01", "2026-12-31", "t1", "csv");
    EXPECT_EQ(csv.status, 200);
    EXPECT_EQ(csv.body["format"], "csv");
}

TEST_F(AdminControllerTest, ExportCostReportJsonCrossTenantIsolation) {
    // SR-1：非 super 导出 JSON 仅见本租户成本；显式跨租户 → 403。
    CostRecord a; a.request_id = "mine"; a.model = "m"; a.tenant_id = "t1";
    a.timestamp = "2026-06-04T10:00:00Z"; a.total_cost = 0.01;
    store_.insertCostRecord(a);
    CostRecord b; b.request_id = "theirs"; b.model = "m"; b.tenant_id = "t2";
    b.timestamp = "2026-06-04T10:00:00Z"; b.total_cost = 0.02;
    store_.insertCostRecord(b);

    // tenant_id 留空 → effective 收敛到 admin 自己的 t1，不得含 t2 行。
    auto js = ctrl_->exportCostReport(admin_ctx_, "2026-01-01", "2026-12-31", "", "json");
    ASSERT_EQ(js.status, 200);
    auto data = js.body["data"];
    ASSERT_TRUE(data.is_array());
    bool sawTheirs = false;
    for (const auto& r : data) {
        if (r["request_id"] == "theirs") sawTheirs = true;
    }
    EXPECT_FALSE(sawTheirs) << "SR-1: JSON export leaked other tenant cost rows";

    // 显式跨租户 → 403。
    auto cross = ctrl_->exportCostReport(admin_ctx_, "2026-01-01", "2026-12-31", "t-other", "json");
    EXPECT_EQ(cross.status, 403);
}

// === SSO provider list total (TASK-20260605-02 P1 / SR-2) ===

TEST_F(AdminControllerTest, ListSsoProvidersReturnsTotalField) {
    // P1：响应须含 total 字段（供前端真分页）。Memory store 不存 SSO → total=0。
    auto r = ctrl_->listSsoProviders(super_ctx_, 50, 0);
    EXPECT_EQ(r.status, 200);
    ASSERT_TRUE(r.body.contains("total"));
    EXPECT_TRUE(r.body["total"].is_number());
}

TEST_F(AdminControllerTest, ListSsoProvidersRequiresSuperAdmin) {
    // SR-2：仅 SuperAdmin 可达；非 super → 403（total 不越权可达）。
    EXPECT_EQ(ctrl_->listSsoProviders(admin_ctx_, 50, 0).status, 403);
    EXPECT_EQ(ctrl_->listSsoProviders(dev_ctx_, 50, 0).status, 403);
    EXPECT_EQ(ctrl_->listSsoProviders(viewer_ctx_, 50, 0).status, 403);
}

TEST_F(AdminControllerTest, ExportWritesAuditEntry) {
    auto before = audit_->entries().size();
    auto r = ctrl_->exportAuditReport(admin_ctx_, "2026-01-01", "2026-12-31", "t1", "csv");
    ASSERT_EQ(r.status, 200);
    auto entries = audit_->entries();
    bool found = false;
    for (size_t i = before; i < entries.size(); ++i) {
        if (entries[i].action.find("admin.export_audit_report") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found) << "Expected admin.export_audit_report audit entry";
}

// === P0-1 / SR-1：垂直提权防护（TASK-20260702-01） ===
// 调用者不得授予/操作高于自身角色的主体（createUser / updateUser /
// createApiKey / rotateApiKey）。否则 TenantAdmin 可在本租户造出 super_admin
// 主体，配合 requireTenantAccess(super)=恒放行 → 提权 + 跨租户。

TEST_F(AdminControllerTest, CreateUserPrivilegeEscalationDenied) {
    Tenant t; t.id = "t1"; t.name = "T1"; store_.insertTenant(t);
    auto r = ctrl_->createUser(admin_ctx_, {{"username", "evil"}, {"role", "super_admin"}});
    EXPECT_EQ(r.status, 403) << "SR-1: TenantAdmin 不得创建 super_admin 用户";
}

TEST_F(AdminControllerTest, CreateUserEqualRoleAllowed) {
    Tenant t; t.id = "t1"; t.name = "T1"; store_.insertTenant(t);
    // 边界：授予等于自身角色（tenant_admin）应允许。
    auto r = ctrl_->createUser(admin_ctx_, {{"username", "peer"}, {"role", "tenant_admin"}});
    EXPECT_EQ(r.status, 201) << "SR-1: 授予 <= 自身角色应允许";
    EXPECT_EQ(r.body["role"], "tenant_admin");
}

TEST_F(AdminControllerTest, UpdateUserPrivilegeEscalationDenied) {
    Tenant t; t.id = "t1"; t.name = "T1"; store_.insertTenant(t);
    User u; u.id = "u1"; u.tenant_id = "t1"; u.username = "alice"; u.role = Role::Developer;
    store_.insertUser(u);
    auto r = ctrl_->updateUser(admin_ctx_, "u1", {{"role", "super_admin"}});
    EXPECT_EQ(r.status, 403) << "SR-1: TenantAdmin 不得把用户提升为 super_admin";
}

TEST_F(AdminControllerTest, CreateApiKeyPrivilegeEscalationDenied) {
    Tenant t; t.id = "t1"; t.name = "T1"; store_.insertTenant(t);
    User u; u.id = "u1"; u.tenant_id = "t1"; u.username = "alice"; u.role = Role::Developer;
    store_.insertUser(u);
    auto r = ctrl_->createApiKey(admin_ctx_, {{"user_id", "u1"}, {"role", "super_admin"}});
    EXPECT_EQ(r.status, 403) << "SR-1: TenantAdmin 不得签发 super_admin 密钥";
}

// === P1-1 / SR-3：dashboard 计数租户隔离（TASK-20260702-01） ===
// dashboardSummary 的 total_requests / total_cost_records 此前走全局 count，
// 非 super 角色会看到跨租户计数（隔离泄漏）。计数须经 effectiveTenantId 过滤。

TEST_F(AdminControllerTest, DashboardSummaryCountsTenantScopedForNonSuper) {
    AuditEntry a1; a1.request_id="r1"; a1.tenant_id="t1"; a1.timestamp="2026-03-22T10:00:00Z";
    store_.insertAudit(a1);
    AuditEntry a2; a2.request_id="r2"; a2.tenant_id="t2"; a2.timestamp="2026-03-22T10:00:00Z";
    store_.insertAudit(a2);
    CostRecord c1; c1.request_id="r1"; c1.tenant_id="t1"; c1.model="gpt-4";
    c1.total_cost=0.05; c1.timestamp="2026-03-22T10:00:00Z"; store_.insertCostRecord(c1);
    CostRecord c2; c2.request_id="r2"; c2.tenant_id="t2"; c2.model="gpt-4";
    c2.total_cost=0.07; c2.timestamp="2026-03-22T10:00:00Z"; store_.insertCostRecord(c2);

    auto r = ctrl_->dashboardSummary(admin_ctx_);  // TenantAdmin of t1
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.body["total_requests"].get<int64_t>(), 1)
        << "SR-3: 非 super 只计本租户审计数";
    EXPECT_EQ(r.body["total_cost_records"].get<int64_t>(), 1)
        << "SR-3: 非 super 只计本租户成本记录数";
}

TEST_F(AdminControllerTest, DashboardSummaryTotalCostNotTruncatedAt10k) {
    // P1-2：total_cost 此前用 queryCosts("",10000,0) 内存累加，>10000 条被截断。
    // 改 DB 聚合（costTotal）后应返回全量真实金额。
    for (int i = 0; i < 10001; ++i) {
        CostRecord c; c.request_id = "r" + std::to_string(i); c.tenant_id = "t1";
        c.model = "gpt-4"; c.total_cost = 1.0; c.timestamp = "2026-03-22T10:00:00Z";
        store_.insertCostRecord(c);
    }
    auto r = ctrl_->dashboardSummary(super_ctx_);
    ASSERT_EQ(r.status, 200);
    EXPECT_DOUBLE_EQ(r.body["total_cost"].get<double>(), 10001.0)
        << "P1-2: total_cost 不应被 10000 条截断";
}

TEST_F(AdminControllerTest, DashboardSummaryTotalCostTenantScoped) {
    CostRecord c1; c1.request_id="r1"; c1.tenant_id="t1"; c1.model="gpt-4";
    c1.total_cost=2.0; c1.timestamp="2026-03-22T10:00:00Z"; store_.insertCostRecord(c1);
    CostRecord c2; c2.request_id="r2"; c2.tenant_id="t2"; c2.model="gpt-4";
    c2.total_cost=5.0; c2.timestamp="2026-03-22T10:00:00Z"; store_.insertCostRecord(c2);

    auto r = ctrl_->dashboardSummary(admin_ctx_);  // TenantAdmin of t1
    ASSERT_EQ(r.status, 200);
    EXPECT_DOUBLE_EQ(r.body["total_cost"].get<double>(), 2.0)
        << "P1-2: 非 super total_cost 只计本租户";
}

TEST_F(AdminControllerTest, DashboardSummaryCountsGlobalForSuper) {
    AuditEntry a1; a1.request_id="r1"; a1.tenant_id="t1"; a1.timestamp="2026-03-22T10:00:00Z";
    store_.insertAudit(a1);
    AuditEntry a2; a2.request_id="r2"; a2.tenant_id="t2"; a2.timestamp="2026-03-22T10:00:00Z";
    store_.insertAudit(a2);
    CostRecord c1; c1.request_id="r1"; c1.tenant_id="t1"; c1.model="gpt-4";
    c1.total_cost=0.05; c1.timestamp="2026-03-22T10:00:00Z"; store_.insertCostRecord(c1);
    CostRecord c2; c2.request_id="r2"; c2.tenant_id="t2"; c2.model="gpt-4";
    c2.total_cost=0.07; c2.timestamp="2026-03-22T10:00:00Z"; store_.insertCostRecord(c2);

    auto r = ctrl_->dashboardSummary(super_ctx_);
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.body["total_requests"].get<int64_t>(), 2)
        << "SR-3: super 看全局审计数";
    EXPECT_EQ(r.body["total_cost_records"].get<int64_t>(), 2)
        << "SR-3: super 看全局成本记录数";
}

TEST_F(AdminControllerTest, RotateApiKeyHigherRoleDenied) {
    Tenant t; t.id = "t1"; t.name = "T1"; store_.insertTenant(t);
    User u; u.id = "u1"; u.tenant_id = "t1"; u.username = "root"; u.role = Role::SuperAdmin;
    store_.insertUser(u);
    // super 先造一把 super_admin 密钥。
    auto cr = ctrl_->createApiKey(super_ctx_, {{"user_id", "u1"}, {"role", "super_admin"}});
    ASSERT_EQ(cr.status, 201);
    auto key_id = cr.body["id"].get<std::string>();
    // TenantAdmin 不得为高于自身角色的密钥重新签发凭证。
    auto r = ctrl_->rotateApiKey(admin_ctx_, key_id);
    EXPECT_EQ(r.status, 403) << "SR-1: TenantAdmin 不得 rotate super_admin 密钥";
}
