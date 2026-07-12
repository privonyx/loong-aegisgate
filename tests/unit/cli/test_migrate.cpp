#include <gtest/gtest.h>
#include "cli/migrate.h"
#include "storage/memory_persistent_store.h"

using namespace aegisgate;

class MigrateTest : public ::testing::Test {
protected:
    void SetUp() override {
        source_.initialize();
        target_.initialize();
    }

    MemoryPersistentStore source_;
    MemoryPersistentStore target_;
    MigrationTool tool_;
};

TEST_F(MigrateTest, MigrateEmptyStores) {
    auto result = tool_.migrate(source_, target_);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.audits_migrated, 0);
    EXPECT_EQ(result.costs_migrated, 0);
}

TEST_F(MigrateTest, MigrateAudits) {
    for (int i = 0; i < 5; ++i) {
        AuditEntry e;
        e.request_id = "req-" + std::to_string(i);
        e.timestamp = "2026-03-19T12:00:00Z";
        e.tenant_id = "t1";
        source_.insertAudit(e);
    }

    auto result = tool_.migrate(source_, target_);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.audits_migrated, 5);
    EXPECT_EQ(target_.auditCount(), 5);
}

TEST_F(MigrateTest, MigrateCostRecords) {
    for (int i = 0; i < 3; ++i) {
        CostRecord r;
        r.request_id = "cost-" + std::to_string(i);
        r.timestamp = "2026-03-19T12:00:00Z";
        r.model = "gpt-4";
        r.input_tokens = 100;
        r.output_tokens = 50;
        r.total_cost = 0.01;
        source_.insertCostRecord(r);
    }

    auto result = tool_.migrate(source_, target_);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.costs_migrated, 3);
    EXPECT_EQ(target_.costRecordCount(), 3);
}

TEST_F(MigrateTest, IdempotentSkipsDuplicates) {
    AuditEntry e;
    e.request_id = "dup-1";
    e.timestamp = "2026-03-19T12:00:00Z";
    source_.insertAudit(e);
    target_.insertAudit(e);

    auto result = tool_.migrate(source_, target_);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.audits_migrated, 0);
    EXPECT_EQ(result.audits_skipped, 1);
    EXPECT_EQ(target_.auditCount(), 1);
}

TEST_F(MigrateTest, ProgressCallbackInvoked) {
    for (int i = 0; i < 3; ++i) {
        AuditEntry e;
        e.request_id = "progress-" + std::to_string(i);
        e.timestamp = "2026-03-19T12:00:00Z";
        source_.insertAudit(e);
    }

    int callback_count = 0;
    auto result = tool_.migrate(source_, target_,
        [&](const std::string&, int64_t, int64_t) { ++callback_count; });
    EXPECT_TRUE(result.success);
    EXPECT_GT(callback_count, 0);
}

TEST_F(MigrateTest, UnhealthySourceFails) {
    MemoryPersistentStore dead;
    auto result = tool_.migrate(dead, target_);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
}

TEST_F(MigrateTest, UnhealthyTargetFails) {
    MemoryPersistentStore dead;
    auto result = tool_.migrate(source_, dead);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
}

TEST_F(MigrateTest, MixedAuditsAndCosts) {
    for (int i = 0; i < 2; ++i) {
        AuditEntry e;
        e.request_id = "mixed-a-" + std::to_string(i);
        e.timestamp = "2026-03-19T12:00:00Z";
        source_.insertAudit(e);
    }
    for (int i = 0; i < 3; ++i) {
        CostRecord r;
        r.request_id = "mixed-c-" + std::to_string(i);
        r.timestamp = "2026-03-19T12:00:00Z";
        r.model = "gpt-4";
        source_.insertCostRecord(r);
    }

    auto result = tool_.migrate(source_, target_);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.audits_migrated, 2);
    EXPECT_EQ(result.costs_migrated, 3);
}
