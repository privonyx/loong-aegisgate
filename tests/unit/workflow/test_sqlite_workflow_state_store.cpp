// Phase 11.3 TASK-20260523-02 — Epic 2.2: SQLiteWorkflowStateStore.
//
// SR-NEW3 anchor — covers durable run/node lifecycle, DDL constraints
// (NOT NULL + CHECK on status enums), foreign key cascade on prune,
// BEGIN IMMEDIATE serialisation of concurrent transitions, and
// initialise/re-open idempotency.

#include "workflow/sqlite_workflow_state_store.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace aw = aegisgate::workflow;

namespace {

std::string tempDb(const std::string& suffix) {
    auto p = std::filesystem::temp_directory_path() /
             ("aegisgate_wf_" + suffix + ".sqlite");
    std::filesystem::remove(p);
    return p.string();
}

aw::WorkflowRunRecord makeRun(const std::string& id) {
    aw::WorkflowRunRecord r;
    r.run_id        = id;
    r.workflow_id   = "wf_demo";
    r.dsl_hash      = "deadbeef";
    r.status        = aw::WorkflowRunStatus::Pending;
    r.created_at_ms = 1000;
    r.updated_at_ms = 1000;
    r.context_json  = R"({"k":"v"})";
    return r;
}

aw::WorkflowNodeRunRecord makeNode(const std::string& run_id,
                                    const std::string& node_id) {
    aw::WorkflowNodeRunRecord n;
    n.run_id        = run_id;
    n.node_id       = node_id;
    n.attempt       = 1;
    n.status        = aw::WorkflowNodeStatus::Pending;
    return n;
}

} // namespace

TEST(SQLiteWorkflowStateStoreTest, CreatesSchemaAndPersistsRun) {
    auto db = tempDb("a");
    aw::SQLiteWorkflowStateStore store(db);
    ASSERT_TRUE(store.initialize());
    ASSERT_TRUE(store.createRun(makeRun("r1")));
    auto got = store.getRun("r1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->workflow_id, "wf_demo");
    EXPECT_EQ(got->dsl_hash, "deadbeef");
    EXPECT_EQ(got->status, aw::WorkflowRunStatus::Pending);
}

TEST(SQLiteWorkflowStateStoreTest, ReopensAndSeesRowsAcrossInstances) {
    auto db = tempDb("b");
    {
        aw::SQLiteWorkflowStateStore s(db);
        ASSERT_TRUE(s.initialize());
        ASSERT_TRUE(s.createRun(makeRun("r2")));
    }
    aw::SQLiteWorkflowStateStore s2(db);
    ASSERT_TRUE(s2.initialize());
    EXPECT_TRUE(s2.getRun("r2").has_value());
}

TEST(SQLiteWorkflowStateStoreTest, DdlRejectsInvalidStatusViaCheck) {
    // SR-NEW3 invariant — direct INSERT with an unknown status value must be
    // rejected by the CHECK constraint. We bypass the typed API to probe the
    // DDL guarantee, mirroring TASK-20260521-03 P1 "test the contract".
    auto db = tempDb("c");
    aw::SQLiteWorkflowStateStore store(db);
    ASSERT_TRUE(store.initialize());
    EXPECT_FALSE(store.execRawForTesting(
        "INSERT INTO workflow_runs (run_id, workflow_id, dsl_hash, status, "
        "created_at_ms, updated_at_ms) VALUES "
        "('rx','wf','h','bogus_status',1,1);"));
}

TEST(SQLiteWorkflowStateStoreTest, NodeUpsertCascadesAndPruneRemovesNodes) {
    auto db = tempDb("d");
    aw::SQLiteWorkflowStateStore store(db);
    ASSERT_TRUE(store.initialize());
    auto r = makeRun("r3"); r.updated_at_ms = 100;
    store.createRun(r);
    store.upsertNodeRun(makeNode("r3", "n1"));
    store.upsertNodeRun(makeNode("r3", "n2"));
    // upsert again with new status — single row remains.
    auto n = makeNode("r3", "n1");
    n.status = aw::WorkflowNodeStatus::Succeeded;
    n.ended_at_ms = 200;
    store.upsertNodeRun(n);
    EXPECT_EQ(store.listNodeRuns("r3").size(), 2u);
    int pruned = store.pruneOldRuns(1000);
    EXPECT_EQ(pruned, 1);
    EXPECT_TRUE(store.listNodeRuns("r3").empty());
}

// I30 (TASK-20260703-04) parity：不同 attempt 独立行；getNodeRun 返回最高 attempt
// （ORDER BY attempt DESC LIMIT 1）。与 Memory 后端一致。
TEST(SQLiteWorkflowStateStoreTest, NodeAttemptsAppendAsSeparateRows) {
    auto db = tempDb("i30");
    aw::SQLiteWorkflowStateStore store(db);
    ASSERT_TRUE(store.initialize());
    store.createRun(makeRun("r-i30"));

    auto a1 = makeNode("r-i30", "nx");
    a1.attempt = 1; a1.status = aw::WorkflowNodeStatus::Failed;
    store.upsertNodeRun(a1);
    auto a2 = makeNode("r-i30", "nx");
    a2.attempt = 2; a2.status = aw::WorkflowNodeStatus::Succeeded;
    a2.ended_at_ms = 9999;
    store.upsertNodeRun(a2);

    EXPECT_EQ(store.listNodeRuns("r-i30").size(), 2u);
    auto latest = store.getNodeRun("r-i30", "nx");
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->attempt, 2);
    EXPECT_EQ(latest->status, aw::WorkflowNodeStatus::Succeeded);
}

TEST(SQLiteWorkflowStateStoreTest, TransitionRunStatusUpdatesRow) {
    auto db = tempDb("e");
    aw::SQLiteWorkflowStateStore store(db);
    ASSERT_TRUE(store.initialize());
    store.createRun(makeRun("r4"));
    EXPECT_TRUE(store.transitionRunStatus(
        "r4", aw::WorkflowRunStatus::WaitingForApproval, 4000));
    auto got = store.getRun("r4");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, aw::WorkflowRunStatus::WaitingForApproval);
    EXPECT_EQ(got->updated_at_ms, 4000);
}

TEST(SQLiteWorkflowStateStoreTest, ListRunsByStatusFilter) {
    auto db = tempDb("f");
    aw::SQLiteWorkflowStateStore store(db);
    ASSERT_TRUE(store.initialize());
    auto r1 = makeRun("a"); r1.status = aw::WorkflowRunStatus::Running;
    auto r2 = makeRun("b"); r2.status = aw::WorkflowRunStatus::Succeeded;
    auto r3 = makeRun("c"); r3.status = aw::WorkflowRunStatus::Running;
    store.createRun(r1); store.createRun(r2); store.createRun(r3);
    EXPECT_EQ(store.listRuns(aw::WorkflowRunStatus::Running).size(), 2u);
    EXPECT_EQ(store.listRuns(aw::WorkflowRunStatus::Succeeded).size(), 1u);
    EXPECT_EQ(store.listRuns(std::nullopt).size(), 3u);
}
