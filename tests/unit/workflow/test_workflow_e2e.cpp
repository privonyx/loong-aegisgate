// Phase 11.3 TASK-20260523-02 — Epic 4.4 + 5.2: E2E lifecycle.
//
// DSL parse -> Engine execute -> HumanApproval pause -> AutonomyApprovalWorkflow
// propose -> approve -> apply (via WorkflowApprovalApplier) -> resume ->
// downstream Tool node fires -> run reaches Succeeded.
//
// This is the v1 walk-through artefact (P1 #1 first-time field validation).

#include "agent/tool_registry.h"
#include "agent/tool_sandbox.h"
#include "guardrail/audit.h"
#include "guardrail/inbound/pii_filter.h"
#include "observe/autonomy/approval_queue.h"
#include "observe/autonomy/approval_workflow.h"
#include "storage/memory_persistent_store.h"
#include "workflow/human_approval_node_handler.h"
#include "workflow/memory_workflow_state_store.h"
#include "workflow/workflow_approval_applier.h"
#include "workflow/workflow_dsl_parser.h"
#include "workflow/workflow_engine.h"

#include <atomic>
#include <gtest/gtest.h>

namespace aw = aegisgate::workflow;
namespace ay = aegisgate::autonomy;

namespace {

const char* kE2EYaml = R"YAML(
id: wf_e2e
version: v1
nodes:
  - id: enrich
    type: tool
    tool_id: enrich_request
  - id: review
    type: human_approval
    tool_id: security_admin
    depends_on:
      - enrich
  - id: dispatch
    type: tool
    tool_id: dispatch_response
    depends_on:
      - review
)YAML";

aegisgate::ToolDefinition tool(const std::string& id) {
    aegisgate::ToolDefinition t;
    t.id = id; t.name = id; t.enabled = true;
    return t;
}

} // namespace

TEST(WorkflowEndToEndTest, FullLifecycleParseExecuteApproveResume) {
    // ----- 1. Parse DSL ------------------------------------------------
    auto parsed = aw::parseWorkflowDslYaml(kE2EYaml);
    ASSERT_TRUE(parsed.ok) << (parsed.errors.empty() ? "" : parsed.errors[0]);
    const auto& dsl = *parsed.dsl;

    // ----- 2. Wire everything up (mirror what GatewayRuntime will do) --
    aegisgate::ToolRegistry registry;
    registry.registerTool(tool("enrich_request"));
    registry.registerTool(tool("dispatch_response"));
    aegisgate::ToolSandbox sandbox(&registry);
    std::atomic<int> tool_calls{0};
    sandbox.setExecutor(
        [&](const std::string&, const nlohmann::json&) -> std::string {
            ++tool_calls;
            return "ok";
        });

    aw::MemoryWorkflowStateStore store;
    aw::WorkflowEngineConfig cfg; cfg.worker_count = 2;
    aw::WorkflowEngine engine(cfg, &sandbox, &store);

    auto pstore = std::make_shared<aegisgate::MemoryPersistentStore>();
    pstore->initialize();
    auto queue = std::make_shared<ay::ApprovalQueue>(pstore.get());
    queue->initialize();
    auto audit = std::make_shared<aegisgate::AuditLogger>();
    auto pii   = std::make_shared<aegisgate::PIIFilter>();
    auto wf    = std::make_shared<ay::AutonomyApprovalWorkflow>(queue, audit, pii);
    wf->setAutonomyEnabledOverride(true);

    auto applier = std::make_shared<aw::WorkflowApprovalApplier>(&engine, &store);
    wf->registerApplier(ay::AutonomySource::Workflow, applier);

    aw::HumanApprovalNodeHandler handler(wf, pii);

    // The engine callback delegates HumanApproval handling to the handler
    // which enqueues the proposal; we capture the assigned id for step 4.
    std::string proposal_id;
    engine.setHumanApprovalCallback(
        [&](const std::string& run_id, const aw::NodeSpec& node,
            const nlohmann::json& ctx) {
            proposal_id = handler.enqueue(run_id, dsl.id,
                                             aw::canonicalHash(dsl),
                                             node, ctx);
            return true;  // pause
        });

    // ----- 3. Execute --------------------------------------------------
    auto first = engine.execute(dsl, "run-E2E",
                                  nlohmann::json{{"trace_id", "trace-1"}});
    EXPECT_FALSE(first.ok);
    EXPECT_EQ(first.final_status, aw::WorkflowRunStatus::WaitingForApproval);
    EXPECT_EQ(tool_calls.load(), 1);   // only "enrich" fired
    ASSERT_FALSE(proposal_id.empty());

    // ----- 4. Approve --------------------------------------------------
    EXPECT_TRUE(wf->approve(proposal_id, "alice"));

    // ----- 5. Apply (which calls engine.resume internally) -------------
    EXPECT_TRUE(wf->apply(proposal_id));
    EXPECT_EQ(tool_calls.load(), 2);   // "dispatch" fired post-resume

    auto run = store.getRun("run-E2E");
    ASSERT_TRUE(run.has_value());
    EXPECT_EQ(run->status, aw::WorkflowRunStatus::Succeeded);

    // SR3 / SR-NEW3 / SR2 / SR17 layer 1+2 all touched in this single run.
    auto nodes = store.listNodeRuns("run-E2E");
    EXPECT_EQ(nodes.size(), 3u);
    int succ = 0;
    for (const auto& n : nodes) {
        if (n.status == aw::WorkflowNodeStatus::Succeeded) ++succ;
    }
    EXPECT_EQ(succ, 3);
}
