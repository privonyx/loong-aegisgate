// Phase 11.3 TASK-20260523-02 — Epic 3.2 + 3.3: WorkflowEngine.
//
// Covers:
//   - topological dispatch + dependency ordering (SR-NEW3 anchor)
//   - ToolSandbox usage on every executable node (SR3 layer 1)
//   - retry policy and timeout per node
//   - dead-letter routing for terminal failures
//   - SR17 layer 1 (engine refuses execute when autonomy disabled)

#include "agent/tool_registry.h"
#include "agent/tool_sandbox.h"
#include "workflow/workflow_engine.h"
#include "workflow/memory_workflow_state_store.h"

#include <atomic>
#include <gtest/gtest.h>
#include <thread>

namespace aw = aegisgate::workflow;

namespace {

aw::WorkflowDsl makeLinearDsl(int n_nodes) {
    aw::WorkflowDsl dsl;
    dsl.id      = "wf_linear";
    dsl.version = "v1";
    for (int i = 0; i < n_nodes; ++i) {
        aw::NodeSpec ns;
        ns.id      = "n" + std::to_string(i);
        ns.type    = aw::NodeType::Tool;
        ns.tool_id = "echo";
        if (i > 0) ns.depends_on.push_back("n" + std::to_string(i - 1));
        dsl.nodes.push_back(ns);
    }
    return dsl;
}

aegisgate::ToolDefinition echoTool() {
    aegisgate::ToolDefinition t;
    t.id          = "echo";
    t.name        = "echo";
    t.description = "echoes argument";
    t.enabled     = true;
    return t;
}

// Convenience: build a minimal engine for the tests with a dependency
// injected ToolSandbox the test owns and configures per-case.
struct TestRig {
    aegisgate::ToolRegistry registry;
    aegisgate::ToolSandbox  sandbox{&registry};
    aw::MemoryWorkflowStateStore store;
    std::unique_ptr<aw::WorkflowEngine> engine;

    TestRig() {
        registry.registerTool(echoTool());
        aw::WorkflowEngineConfig cfg;
        cfg.worker_count = 2;
        engine = std::make_unique<aw::WorkflowEngine>(cfg, &sandbox, &store);
    }
};

} // namespace

// P0-E (TASK-20260701-01): the production runtime (gateway_runtime.cpp) wires
// the WorkflowEngine with sandbox=nullptr. Dispatching a Tool node previously
// dereferenced the null sandbox and SIGSEGV'd the whole gateway process. The
// engine must instead fail the tool node gracefully (-> DLQ) without crashing.
TEST(WorkflowEngineTest, NullSandboxFailsToolNodeWithoutCrash) {
    aw::MemoryWorkflowStateStore store;
    aw::WorkflowEngineConfig cfg;
    cfg.worker_count = 1;
    aw::WorkflowEngine engine(cfg, /*sandbox=*/nullptr, &store);
    engine.setAutonomyEnabledOverride(true);

    auto dsl = makeLinearDsl(1);  // single Tool node "echo"
    auto res = engine.execute(dsl, "run_null_sandbox", nlohmann::json::object());

    EXPECT_FALSE(res.ok);
    EXPECT_NE(res.final_status, aw::WorkflowRunStatus::Succeeded);
    EXPECT_GE(res.failed_nodes, 1);
    EXPECT_FALSE(engine.hasSandbox());  // C8：null 装配 → 无 sandbox
}

// TASK-20260703-02 Epic 4 / C8 — 装配契约：引擎须报告 sandbox 装配状态。生产
// gateway_runtime 此前误传 nullptr → Tool 节点全进 DLQ；修复后从
// pipeline_->tool_sandbox.get() 装配 → hasSandbox()==true 且 Tool 节点真正执行。
TEST(WorkflowEngineTest, WiredSandboxReportsHasSandboxAndExecutesToolNode) {
    TestRig rig;  // 与生产同构：ToolRegistry 拥有 + WorkflowEngine 持 sandbox 指针
    rig.engine->setAutonomyEnabledOverride(true);
    EXPECT_TRUE(rig.engine->hasSandbox());  // C8：真实装配 → 有 sandbox

    bool executed = false;
    rig.sandbox.setExecutor(
        [&](const std::string& tool_id, const nlohmann::json&) -> std::string {
            executed = true;
            return tool_id;
        });

    auto dsl = makeLinearDsl(1);  // 单 Tool 节点
    auto res = rig.engine->execute(dsl, "run_wired_sandbox", nlohmann::json::object());
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.final_status, aw::WorkflowRunStatus::Succeeded);
    EXPECT_TRUE(executed);  // Tool 节点经 sandbox 执行（非 DLQ）
}

TEST(WorkflowEngineTest, ExecutesLinearDagInDependencyOrder) {
    TestRig rig;
    std::vector<std::string> order;
    std::mutex order_mu;
    rig.sandbox.setExecutor(
        [&](const std::string& tool_id, const nlohmann::json& args) -> std::string {
            std::lock_guard<std::mutex> g(order_mu);
            order.push_back(args.value("node_id", std::string()));
            return tool_id;
        });

    auto dsl = makeLinearDsl(3);
    auto res = rig.engine->execute(dsl, "run-A", /*context=*/nlohmann::json::object());
    ASSERT_TRUE(res.ok);
    EXPECT_EQ(res.final_status, aw::WorkflowRunStatus::Succeeded);
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], "n0");
    EXPECT_EQ(order[1], "n1");
    EXPECT_EQ(order[2], "n2");
}

TEST(WorkflowEngineTest, EveryToolNodeGoesThroughSandbox) {
    TestRig rig;
    std::atomic<int> calls{0};
    rig.sandbox.setExecutor(
        [&](const std::string& /*tool_id*/, const nlohmann::json&) -> std::string {
            calls++;
            return "ok";
        });
    auto dsl = makeLinearDsl(4);
    auto res = rig.engine->execute(dsl, "run-B", nlohmann::json::object());
    ASSERT_TRUE(res.ok);
    EXPECT_EQ(calls.load(), 4);
    // SR3: total ToolSandbox executions reflects every Tool node dispatched.
    EXPECT_EQ(rig.sandbox.totalExecutions(), 4u);
}

TEST(WorkflowEngineTest, RetriesAreApplied) {
    TestRig rig;
    std::atomic<int> attempts{0};
    rig.sandbox.setExecutor(
        [&](const std::string&, const nlohmann::json&) -> std::string {
            int n = ++attempts;
            if (n < 3) throw std::runtime_error("transient");
            return "ok";
        });
    auto dsl = makeLinearDsl(1);
    dsl.nodes[0].retry.max_attempts = 3;
    auto res = rig.engine->execute(dsl, "run-R", nlohmann::json::object());
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(attempts.load(), 3);
    // I30 (TASK-20260703-04)：每次 attempt 现为独立行（审计链），不再单行覆盖。
    auto nodes = rig.store.listNodeRuns("run-R");
    ASSERT_EQ(nodes.size(), 3u);
    auto latest = rig.store.getNodeRun("run-R", "n0");
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->status, aw::WorkflowNodeStatus::Succeeded);
    EXPECT_EQ(latest->attempt, 3);
}

TEST(WorkflowEngineTest, FailedNodeRoutesToDeadLetter) {
    TestRig rig;
    rig.sandbox.setExecutor(
        [&](const std::string&, const nlohmann::json&) -> std::string {
            throw std::runtime_error("permanent");
        });
    auto dsl = makeLinearDsl(1);
    dsl.nodes[0].retry.max_attempts = 2;
    auto res = rig.engine->execute(dsl, "run-D", nlohmann::json::object());
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.final_status, aw::WorkflowRunStatus::DeadLetter);
    // I30：attempt 1 Failed 独立留存；attempt 2 终态 DeadLetter（审计链 2 行）。
    auto nodes = rig.store.listNodeRuns("run-D");
    ASSERT_EQ(nodes.size(), 2u);
    auto latest = rig.store.getNodeRun("run-D", "n0");
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->status, aw::WorkflowNodeStatus::DeadLetter);
    EXPECT_EQ(latest->attempt, 2);
}

TEST(WorkflowEngineTest, RefusesExecuteWhenAutonomyDisabled) {
    // SR17 anchor — engine layer. Force disable via override.
    TestRig rig;
    rig.engine->setAutonomyEnabledOverride(false);
    rig.sandbox.setExecutor(
        [](const std::string&, const nlohmann::json&) -> std::string {
            return "should-not-be-called";
        });
    auto dsl = makeLinearDsl(1);
    auto res = rig.engine->execute(dsl, "run-K", nlohmann::json::object());
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.final_status, aw::WorkflowRunStatus::Cancelled);
    EXPECT_FALSE(res.error_message.empty());
    EXPECT_NE(res.error_message.find("autonomy_disabled"), std::string::npos);
}

TEST(WorkflowEngineTest, CycleDetectionPreventsExecute) {
    TestRig rig;
    rig.sandbox.setExecutor(
        [](const std::string&, const nlohmann::json&) -> std::string {
            return "ok";
        });
    aw::WorkflowDsl dsl;
    dsl.id      = "wf_cycle";
    dsl.version = "v1";
    aw::NodeSpec a, b;
    a.id = "a"; a.type = aw::NodeType::Tool; a.tool_id = "echo";
    a.depends_on = {"b"};
    b.id = "b"; b.type = aw::NodeType::Tool; b.tool_id = "echo";
    b.depends_on = {"a"};
    dsl.nodes = {a, b};
    auto res = rig.engine->execute(dsl, "run-C", nlohmann::json::object());
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.final_status, aw::WorkflowRunStatus::Failed);
    EXPECT_NE(res.error_message.find("cycle"), std::string::npos);
}

TEST(WorkflowEngineTest, SandboxBypassAttemptIsRejected) {
    TestRig rig;
    rig.sandbox.setExecutor(
        [](const std::string&, const nlohmann::json&) -> std::string {
            return "should-not-be-called";
        });
    aw::WorkflowDsl dsl;
    dsl.id      = "wf_bypass";
    dsl.version = "v1";
    aw::NodeSpec n;
    n.id      = "exec";
    n.type    = aw::NodeType::Tool;
    n.tool_id = "";  // bypass attempt
    dsl.nodes.push_back(n);
    auto res = rig.engine->execute(dsl, "run-S", nlohmann::json::object());
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.final_status, aw::WorkflowRunStatus::Failed);
    EXPECT_NE(res.error_message.find("sandbox_bypass"), std::string::npos);
}

// ---------------------------------------------------------------------------
// TASK-20260703-04 Epic 2 / D2=C — 连续调度 + 节点超时 + 背压。
// ---------------------------------------------------------------------------

namespace {
aw::WorkflowDsl makeParallelDsl(int n) {
    aw::WorkflowDsl dsl;
    dsl.id      = "wf_par";
    dsl.version = "v1";
    for (int i = 0; i < n; ++i) {
        aw::NodeSpec ns;
        ns.id      = "p" + std::to_string(i);
        ns.type    = aw::NodeType::Tool;
        ns.tool_id = "echo";
        dsl.nodes.push_back(ns);
    }
    return dsl;
}
} // namespace

// SR-4/D2=C：无依赖节点应并发执行（观测峰值并发 > 1）。
TEST(WorkflowEngineTest, IndependentNodesRunConcurrently) {
    aegisgate::ToolRegistry registry;
    registry.registerTool(echoTool());
    aegisgate::ToolSandbox sandbox{&registry};
    aw::MemoryWorkflowStateStore store;
    aw::WorkflowEngineConfig cfg;
    cfg.worker_count = 4;
    aw::WorkflowEngine engine(cfg, &sandbox, &store);

    std::atomic<int> inflight{0};
    std::atomic<int> peak{0};
    sandbox.setExecutor(
        [&](const std::string& tid, const nlohmann::json&) -> std::string {
            int cur = ++inflight;
            int prev = peak.load();
            while (cur > prev && !peak.compare_exchange_weak(prev, cur)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            --inflight;
            return tid;
        });

    auto dsl = makeParallelDsl(4);
    auto res = engine.execute(dsl, "run-par", nlohmann::json::object());
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.completed_nodes, 4);
    EXPECT_GE(peak.load(), 2);  // 至少两节点同时在执行 → 证明并发
}

// SR-5：挂起节点须被超时守卫踢入 DLQ，调度不永久阻塞。
TEST(WorkflowEngineTest, HangingNodeTimesOutToDeadLetter) {
    aegisgate::ToolRegistry registry;
    registry.registerTool(echoTool());
    aegisgate::ToolSandbox sandbox{&registry};
    aw::MemoryWorkflowStateStore store;
    aw::WorkflowEngineConfig cfg;
    cfg.worker_count = 2;
    aw::WorkflowEngine engine(cfg, &sandbox, &store);

    sandbox.setExecutor(
        [&](const std::string& tid, const nlohmann::json&) -> std::string {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            return tid;
        });

    auto dsl = makeLinearDsl(1);
    dsl.nodes[0].timeout_ms = 50;  // 远小于 400ms 执行 → 超时
    auto res = engine.execute(dsl, "run-timeout", nlohmann::json::object());
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.final_status, aw::WorkflowRunStatus::DeadLetter);
    EXPECT_GE(res.failed_nodes, 1);
}

// SR-4：max_concurrent_runs 背压 → 超额并发 run fail-fast 429（backpressure_rejected）。
TEST(WorkflowEngineTest, BackpressureRejectsExcessConcurrentRuns) {
    aegisgate::ToolRegistry registry;
    registry.registerTool(echoTool());
    aegisgate::ToolSandbox sandbox{&registry};
    aw::MemoryWorkflowStateStore store;
    aw::WorkflowEngineConfig cfg;
    cfg.worker_count       = 2;
    cfg.max_concurrent_runs = 1;  // 仅允许 1 个并发 run
    aw::WorkflowEngine engine(cfg, &sandbox, &store);

    std::atomic<bool> release{false};
    sandbox.setExecutor(
        [&](const std::string& tid, const nlohmann::json&) -> std::string {
            // run A 的节点在此阻塞，占住 active_runs_ 直到测试放行。
            for (int i = 0; i < 3000 && !release.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return tid;
        });

    aw::WorkflowExecutionResult res_a;
    std::thread ta([&] {
        res_a = engine.execute(makeLinearDsl(1), "run-A",
                               nlohmann::json::object());
    });
    // 确保 run A 已进入并阻塞在节点执行（active_runs_==1）。
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    auto res_b = engine.execute(makeLinearDsl(1), "run-B",
                                nlohmann::json::object());
    EXPECT_FALSE(res_b.ok);
    EXPECT_EQ(res_b.error_message, "backpressure_rejected");

    release.store(true);
    ta.join();
    EXPECT_TRUE(res_a.ok);
}
