#include <gtest/gtest.h>
#include "observe/compliance_reporter.h"
#include "storage/memory_persistent_store.h"

using namespace aegisgate;

class ComplianceReporterTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_.initialize();
    }

    MemoryPersistentStore store_;
};

TEST_F(ComplianceReporterTest, ExportAuditsCsvHeader) {
    ComplianceReporter reporter(&store_);
    auto csv = reporter.exportAuditsCsv("", "", "");
    EXPECT_NE(csv.find("request_id,timestamp,tenant_id,action,stage,detail"), std::string::npos);
}

TEST_F(ComplianceReporterTest, ExportAuditsCsvContainsEntries) {
    AuditEntry e;
    e.request_id = "req1";
    e.timestamp = "2026-03-25T10:00:00Z";
    e.tenant_id = "t1";
    e.action = "chat";
    e.stage_name = "pipeline";
    e.detail = "model=gpt-4";
    store_.insertAudit(e);

    e.request_id = "req2";
    e.timestamp = "2026-03-25T11:00:00Z";
    store_.insertAudit(e);

    ComplianceReporter reporter(&store_);
    auto csv = reporter.exportAuditsCsv("", "", "t1");
    EXPECT_NE(csv.find("req1,"), std::string::npos);
    EXPECT_NE(csv.find("req2,"), std::string::npos);
}

TEST_F(ComplianceReporterTest, ExportAuditsCsvTimeFilter) {
    AuditEntry e1, e2;
    e1.request_id = "early";
    e1.timestamp = "2026-03-24T10:00:00Z";
    e1.tenant_id = "t1";
    e1.action = "chat";
    store_.insertAudit(e1);

    e2.request_id = "late";
    e2.timestamp = "2026-03-25T10:00:00Z";
    e2.tenant_id = "t1";
    e2.action = "chat";
    store_.insertAudit(e2);

    ComplianceReporter reporter(&store_);
    auto csv = reporter.exportAuditsCsv("2026-03-25T00:00:00Z", "2026-03-26T00:00:00Z", "t1");
    EXPECT_EQ(csv.find("early"), std::string::npos);
    EXPECT_NE(csv.find("late"), std::string::npos);
}

TEST_F(ComplianceReporterTest, ExportAuditsJson) {
    AuditEntry e;
    e.request_id = "jsonreq1";
    e.timestamp = "2026-03-25T10:00:00Z";
    e.tenant_id = "t1";
    e.action = "chat";
    e.stage_name = "GatewayStage";
    store_.insertAudit(e);

    ComplianceReporter reporter(&store_);
    auto json_str = reporter.exportAuditsJson("", "", "t1");
    auto json = nlohmann::json::parse(json_str);
    ASSERT_TRUE(json.is_array());
    ASSERT_GE(json.size(), 1u);
    EXPECT_EQ(json[0]["request_id"], "jsonreq1");
}

TEST_F(ComplianceReporterTest, ExportCostsCsv) {
    CostRecord c;
    c.request_id = "costreq1";
    c.model = "gpt-4";
    c.timestamp = "2026-03-25T10:00:00Z";
    c.tenant_id = "t1";
    c.input_tokens = 500;
    c.output_tokens = 200;
    c.total_cost = 0.03;
    store_.insertCostRecord(c);

    ComplianceReporter reporter(&store_);
    auto csv = reporter.exportCostsCsv("", "", "t1");
    EXPECT_NE(csv.find("costreq1"), std::string::npos);
    EXPECT_NE(csv.find("gpt-4"), std::string::npos);
}

// === Cost JSON export (TASK-20260605-02 P0 / SR-1) ===

TEST_F(ComplianceReporterTest, ExportCostsJson) {
    CostRecord c;
    c.request_id = "costjson1";
    c.model = "gpt-4";
    c.timestamp = "2026-03-25T10:00:00Z";
    c.tenant_id = "t1";
    c.input_tokens = 500;
    c.output_tokens = 200;
    c.total_cost = 0.03;
    store_.insertCostRecord(c);

    ComplianceReporter reporter(&store_);
    auto json_str = reporter.exportCostsJson("", "", "t1");
    auto json = nlohmann::json::parse(json_str);
    ASSERT_TRUE(json.is_array());
    ASSERT_GE(json.size(), 1u);
    EXPECT_EQ(json[0]["request_id"], "costjson1");
    EXPECT_EQ(json[0]["model"], "gpt-4");
    EXPECT_EQ(json[0]["tenant_id"], "t1");
    EXPECT_EQ(json[0]["input_tokens"], 500);
    EXPECT_EQ(json[0]["output_tokens"], 200);
}

TEST_F(ComplianceReporterTest, ExportCostsJsonTenantFilter) {
    // SR-1：JSON 路径必须与 CSV 路径同样按 tenant_id 过滤，不泄漏跨租户成本。
    CostRecord a; a.request_id = "mine"; a.model = "m"; a.tenant_id = "t1";
    a.timestamp = "2026-03-25T10:00:00Z"; a.total_cost = 0.01;
    store_.insertCostRecord(a);
    CostRecord b; b.request_id = "theirs"; b.model = "m"; b.tenant_id = "t2";
    b.timestamp = "2026-03-25T10:00:00Z"; b.total_cost = 0.02;
    store_.insertCostRecord(b);

    ComplianceReporter reporter(&store_);
    auto json = nlohmann::json::parse(reporter.exportCostsJson("", "", "t1"));
    bool sawMine = false, sawTheirs = false;
    for (const auto& r : json) {
        if (r["request_id"] == "mine") sawMine = true;
        if (r["request_id"] == "theirs") sawTheirs = true;
    }
    EXPECT_TRUE(sawMine);
    EXPECT_FALSE(sawTheirs) << "SR-1: cost JSON export must not leak other tenant rows";
}

// === TASK-20260702-02 P2-3 / SR-3：成本导出 tenant 过滤下推到查询层 ===
// 此前 exportCosts* 用 queryCosts("", kMaxExportRows, 0) 全租户取 10w 行再内存
// 过滤 → 他租户数据占满前 10w 时目标租户行被截断漏计（金额低估），与审计导出
// （queryAudits(tenant,...) 下推）不对称。下推 tenant 到 queryCosts 消除截断。
namespace {
class TenantSpyStore : public MemoryPersistentStore {
public:
    std::string last_cost_tenant = "<unset>";
    std::vector<CostRecord> queryCosts(const std::string& model, int limit,
                                       int offset,
                                       const std::string& tenant_id) override {
        last_cost_tenant = tenant_id;
        return MemoryPersistentStore::queryCosts(model, limit, offset, tenant_id);
    }
};
}  // namespace

TEST(ComplianceReporterPushdownTest, ExportCostsCsvPushesTenantIntoQuery) {
    TenantSpyStore s;
    s.initialize();
    CostRecord c;
    c.request_id = "r";
    c.model = "m";
    c.tenant_id = "t1";
    c.timestamp = "2026-03-25T10:00:00Z";
    c.total_cost = 0.01;
    s.insertCostRecord(c);
    ComplianceReporter reporter(&s);
    reporter.exportCostsCsv("", "", "t1");
    EXPECT_EQ(s.last_cost_tenant, "t1")
        << "SR-3: cost export must push tenant filter into queryCosts (not in-memory)";
}

TEST(ComplianceReporterPushdownTest, ExportCostsJsonPushesTenantIntoQuery) {
    TenantSpyStore s;
    s.initialize();
    CostRecord c;
    c.request_id = "r";
    c.model = "m";
    c.tenant_id = "t1";
    c.timestamp = "2026-03-25T10:00:00Z";
    c.total_cost = 0.01;
    s.insertCostRecord(c);
    ComplianceReporter reporter(&s);
    reporter.exportCostsJson("", "", "t1");
    EXPECT_EQ(s.last_cost_tenant, "t1")
        << "SR-3: cost export JSON must push tenant filter into queryCosts";
}

TEST_F(ComplianceReporterTest, NullStoreReturnsEmpty) {
    ComplianceReporter reporter(nullptr);
    EXPECT_TRUE(reporter.exportAuditsCsv("", "", "").empty());
    EXPECT_TRUE(reporter.exportCostsCsv("", "", "").empty());
    EXPECT_EQ(reporter.exportAuditsJson("", "", ""), "[]");
    EXPECT_EQ(reporter.exportCostsJson("", "", ""), "[]");
}
