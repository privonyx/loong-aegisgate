// Phase 11.3 TASK-20260523-02 — Epic 2.1: MemoryWorkflowStateStore.
//
// Validates the IWorkflowStateStore contract that the SQLite backend will
// re-implement (Epic 2.2). Covers: createRun, getRun, listRuns, nodeRun
// upsert, run status transitions, simple resume after persisted snapshot,
// and pruneOldRuns retention.

#include "workflow/memory_workflow_state_store.h"

#include <gtest/gtest.h>

namespace aw = aegisgate::workflow;

namespace {

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
    n.started_at_ms = 0;
    n.ended_at_ms   = 0;
    return n;
}

} // namespace

TEST(MemoryWorkflowStateStoreTest, CreateAndGetRunRoundTrip) {
    aw::MemoryWorkflowStateStore store;
    auto r = makeRun("run-1");
    EXPECT_TRUE(store.createRun(r));
    auto got = store.getRun("run-1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->workflow_id, "wf_demo");
    EXPECT_EQ(got->dsl_hash, "deadbeef");
    EXPECT_EQ(got->status, aw::WorkflowRunStatus::Pending);
    EXPECT_EQ(got->context_json, R"({"k":"v"})");
}

TEST(MemoryWorkflowStateStoreTest, CreateRunRejectsDuplicateId) {
    aw::MemoryWorkflowStateStore store;
    auto r = makeRun("run-dup");
    EXPECT_TRUE(store.createRun(r));
    EXPECT_FALSE(store.createRun(r));
}

TEST(MemoryWorkflowStateStoreTest, TransitionRunStatusUpdatesTimestamp) {
    aw::MemoryWorkflowStateStore store;
    auto r = makeRun("run-2");
    store.createRun(r);
    EXPECT_TRUE(store.transitionRunStatus("run-2",
                                          aw::WorkflowRunStatus::Running,
                                          2000));
    auto got = store.getRun("run-2");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, aw::WorkflowRunStatus::Running);
    EXPECT_EQ(got->updated_at_ms, 2000);
}

TEST(MemoryWorkflowStateStoreTest, TransitionRunStatusRejectsUnknownRun) {
    aw::MemoryWorkflowStateStore store;
    EXPECT_FALSE(store.transitionRunStatus("nope",
                                           aw::WorkflowRunStatus::Failed,
                                           1234));
}

TEST(MemoryWorkflowStateStoreTest, UpsertNodeRunIsIdempotent) {
    aw::MemoryWorkflowStateStore store;
    store.createRun(makeRun("run-n"));
    auto n = makeNode("run-n", "node-1");
    EXPECT_TRUE(store.upsertNodeRun(n));
    // Update — bump status + ended_at_ms.
    n.status      = aw::WorkflowNodeStatus::Succeeded;
    n.ended_at_ms = 7777;
    EXPECT_TRUE(store.upsertNodeRun(n));
    auto nodes = store.listNodeRuns("run-n");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].status, aw::WorkflowNodeStatus::Succeeded);
    EXPECT_EQ(nodes[0].ended_at_ms, 7777);
    EXPECT_EQ(nodes[0].attempt, 1);
}

// I30 (TASK-20260703-04)：不同 attempt 是独立行（审计链），getNodeRun 返回最高
// attempt。修复前 (run,node) 键覆盖 → 只留 1 行。
TEST(MemoryWorkflowStateStoreTest, NodeAttemptsAppendAsSeparateRows) {
    aw::MemoryWorkflowStateStore store;
    store.createRun(makeRun("run-i30"));

    auto a1 = makeNode("run-i30", "node-x");
    a1.attempt = 1;
    a1.status  = aw::WorkflowNodeStatus::Failed;
    EXPECT_TRUE(store.upsertNodeRun(a1));

    auto a2 = makeNode("run-i30", "node-x");
    a2.attempt = 2;
    a2.status  = aw::WorkflowNodeStatus::Succeeded;
    a2.ended_at_ms = 9999;
    EXPECT_TRUE(store.upsertNodeRun(a2));

    auto nodes = store.listNodeRuns("run-i30");
    ASSERT_EQ(nodes.size(), 2u);  // 两次尝试各留一行

    auto latest = store.getNodeRun("run-i30", "node-x");
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->attempt, 2);
    EXPECT_EQ(latest->status, aw::WorkflowNodeStatus::Succeeded);
}

TEST(MemoryWorkflowStateStoreTest, ListRunsFiltersByStatus) {
    aw::MemoryWorkflowStateStore store;
    auto a = makeRun("a"); a.status = aw::WorkflowRunStatus::Running;
    auto b = makeRun("b"); b.status = aw::WorkflowRunStatus::Succeeded;
    auto c = makeRun("c"); c.status = aw::WorkflowRunStatus::Running;
    store.createRun(a); store.createRun(b); store.createRun(c);
    auto running = store.listRuns(aw::WorkflowRunStatus::Running);
    EXPECT_EQ(running.size(), 2u);
    auto all = store.listRuns(std::nullopt);
    EXPECT_EQ(all.size(), 3u);
}

TEST(MemoryWorkflowStateStoreTest, PruneOldRunsRespectsThreshold) {
    aw::MemoryWorkflowStateStore store;
    auto old_run  = makeRun("old");
    old_run.created_at_ms = 100;
    old_run.updated_at_ms = 100;
    auto fresh    = makeRun("new");
    fresh.created_at_ms   = 5000;
    fresh.updated_at_ms   = 5000;
    store.createRun(old_run);
    store.createRun(fresh);
    int n = store.pruneOldRuns(1000);
    EXPECT_EQ(n, 1);
    EXPECT_FALSE(store.getRun("old").has_value());
    EXPECT_TRUE(store.getRun("new").has_value());
}

TEST(MemoryWorkflowStateStoreTest, ResumeReadsBackPersistedContext) {
    aw::MemoryWorkflowStateStore store;
    auto r = makeRun("resume");
    r.context_json = R"({"trace_id":"abc"})";
    store.createRun(r);
    store.upsertNodeRun(makeNode("resume", "node-A"));
    store.upsertNodeRun(makeNode("resume", "node-B"));
    // simulate engine restart: state store survives because it's memory in
    // this process; the assertion is that we can re-read prior to status
    // transitions. (SQLite backend tested separately in Epic 2.2.)
    auto got    = store.getRun("resume");
    auto nodes  = store.listNodeRuns("resume");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->context_json, R"({"trace_id":"abc"})");
    EXPECT_EQ(nodes.size(), 2u);
}
