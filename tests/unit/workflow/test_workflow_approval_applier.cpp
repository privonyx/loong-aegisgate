// Phase 11.3 TASK-20260523-02 — Epic 4.2 + 4.3: WorkflowApprovalApplier.
//
// IApprovalApplier registered for AutonomySource::Workflow. apply() drives
// the paused run forward via WorkflowEngine::resume(). isLowRisk() is the
// 4-rule predicate used when the workflow flagged auto_low_risk mode.
//
// SR anchors:
//   SR17 layer 2 — apply() short-circuits when isAutonomyEnabled() == false
//   SR2          — isLowRisk() is conservative-by-default

#include "agent/tool_registry.h"
#include "agent/tool_sandbox.h"
#include "workflow/memory_workflow_state_store.h"
#include "workflow/workflow_approval_applier.h"
#include "workflow/workflow_engine.h"

#include <cstdlib>
#include <gtest/gtest.h>

namespace aw = aegisgate::workflow;
namespace ay = aegisgate::autonomy;

namespace {

aegisgate::ToolDefinition echoTool() {
    aegisgate::ToolDefinition t;
    t.id = "echo"; t.name = "echo"; t.enabled = true;
    return t;
}

ay::ApprovalProposal makeWorkflowProposal(const std::string& run_id,
                                             const std::string& dsl_hash = "",
                                             const nlohmann::json& payload_extra = {}) {
    ay::ApprovalProposal p;
    p.source  = ay::AutonomySource::Workflow;
    p.subject = "workflow:wf_demo/approval-1";
    p.payload = nlohmann::json{
        {"run_id",      run_id},
        {"workflow_id", "wf_demo"},
        {"node_id",     "approval-1"},
        {"dsl_hash",    dsl_hash},
        {"tool_id",     "security_reviewer"},
    };
    for (auto it = payload_extra.begin(); it != payload_extra.end(); ++it) {
        p.payload[it.key()] = it.value();
    }
    p.decision_trace = nlohmann::json{
        {"source_id",         run_id},
        {"algorithm_name",    "workflow_human_approval_v1"},
        {"input_hash_sha256", std::string(64, 'a')},
        {"proposed_at_ms",    1716030000000LL},
    };
    p.state = ay::ApprovalState::APPROVED;
    return p;
}

struct ApplierRig {
    aegisgate::ToolRegistry registry;
    aegisgate::ToolSandbox  sandbox{&registry};
    aw::MemoryWorkflowStateStore store;
    std::unique_ptr<aw::WorkflowEngine> engine;
    std::shared_ptr<aw::WorkflowApprovalApplier> applier;

    ApplierRig() {
        registry.registerTool(echoTool());
        aw::WorkflowEngineConfig cfg; cfg.worker_count = 2;
        engine = std::make_unique<aw::WorkflowEngine>(cfg, &sandbox, &store);
        applier = std::make_shared<aw::WorkflowApprovalApplier>(engine.get(), &store);
        sandbox.setExecutor(
            [](const std::string&, const nlohmann::json&) -> std::string {
                return "ok";
            });
        // Seed a paused run that the applier will resume.
        aw::WorkflowDsl dsl;
        dsl.id = "wf_demo"; dsl.version = "v1";
        aw::NodeSpec n1, n2, n3;
        n1.id = "n1"; n1.type = aw::NodeType::Tool; n1.tool_id = "echo";
        n2.id = "approval-1"; n2.type = aw::NodeType::HumanApproval;
        n2.tool_id = "security_reviewer"; n2.depends_on = {"n1"};
        n3.id = "n3"; n3.type = aw::NodeType::Tool; n3.tool_id = "echo";
        n3.depends_on = {"approval-1"};
        dsl.nodes = {n1, n2, n3};
        engine->setHumanApprovalCallback(
            [](const std::string&, const aw::NodeSpec&, const nlohmann::json&) {
                return true;  // pause
            });
        engine->execute(dsl, "run-A", nlohmann::json::object());
    }
};

} // namespace

TEST(WorkflowApprovalApplierTest, ApplyResumesPausedRun) {
    ApplierRig r;
    auto run0 = r.store.getRun("run-A");
    ASSERT_TRUE(run0.has_value());
    auto p = makeWorkflowProposal("run-A", run0->dsl_hash);
    auto res = r.applier->apply(p, /*dry_run=*/false);
    EXPECT_TRUE(res.success) << res.error_code << " / " << res.error_message;
    auto run = r.store.getRun("run-A");
    ASSERT_TRUE(run.has_value());
    EXPECT_EQ(run->status, aw::WorkflowRunStatus::Succeeded);
}

TEST(WorkflowApprovalApplierTest, DryRunDoesNotMutateRun) {
    ApplierRig r;
    auto run0 = r.store.getRun("run-A");
    ASSERT_TRUE(run0.has_value());
    auto p = makeWorkflowProposal("run-A", run0->dsl_hash);
    auto res = r.applier->apply(p, /*dry_run=*/true);
    EXPECT_TRUE(res.success);
    auto run = r.store.getRun("run-A");
    ASSERT_TRUE(run.has_value());
    // Run still paused on approval node after dry-run.
    EXPECT_EQ(run->status, aw::WorkflowRunStatus::WaitingForApproval);
}

TEST(WorkflowApprovalApplierTest, RefusesWhenAutonomyDisabled) {
    // SR17 layer 2 anchor.
    ApplierRig r;
    auto run0 = r.store.getRun("run-A");
    ASSERT_TRUE(run0.has_value());
    setenv("AEGISGATE_DISABLE_AUTONOMY", "1", /*overwrite=*/1);
    auto p = makeWorkflowProposal("run-A", run0->dsl_hash);
    auto res = r.applier->apply(p, /*dry_run=*/false);
    unsetenv("AEGISGATE_DISABLE_AUTONOMY");
    EXPECT_FALSE(res.success);
    EXPECT_EQ(res.error_code, "autonomy_disabled");
}

TEST(WorkflowApprovalApplierTest, RejectsDslHashMismatch) {
    // T01 anchor: applier MUST refuse when the proposal carries a hash
    // diverging from the persisted run record.
    ApplierRig r;
    auto p = makeWorkflowProposal("run-A", "tampered_hash");
    auto res = r.applier->apply(p, /*dry_run=*/false);
    EXPECT_FALSE(res.success);
    EXPECT_EQ(res.error_code, "dsl_hash_mismatch");
}

TEST(WorkflowApprovalApplierTest, RejectsSchemaInvalidPayload) {
    ApplierRig r;
    ay::ApprovalProposal p;
    p.source = ay::AutonomySource::Workflow;
    p.payload = nlohmann::json{{"foo", "bar"}};   // missing run_id / node_id
    auto res = r.applier->apply(p, /*dry_run=*/false);
    EXPECT_FALSE(res.success);
    EXPECT_EQ(res.error_code, "schema_invalid");
}

// --- isLowRisk 4-rule predicate (SR2 anchor) -------------------------------

TEST(WorkflowApprovalApplierTest, IsLowRiskConservativeByDefault) {
    ApplierRig r;
    ay::ApprovalProposal p;
    p.source = ay::AutonomySource::Workflow;
    p.payload = nlohmann::json::object();
    EXPECT_FALSE(r.applier->isLowRisk(p));
}

TEST(WorkflowApprovalApplierTest, IsLowRiskRequiresAllFourRules) {
    ApplierRig r;
    // Rule 1: target_tool in safelist (read-only / observability tools)
    // Rule 2: arguments.size() < 5
    // Rule 3: timeout_ms <= 10000
    // Rule 4: tags.contains("low_risk_audited")
    ay::ApprovalProposal p;
    p.source  = ay::AutonomySource::Workflow;
    p.payload = nlohmann::json{
        {"tool_id",    "read_only_metrics_lookup"},
        {"arguments",  {{"k1","v1"},{"k2","v2"}}},
        {"timeout_ms", 5000},
        {"tags",       {"low_risk_audited"}},
    };
    EXPECT_TRUE(r.applier->isLowRisk(p));

    // Remove tag -> should drop to false (rule 4 fail).
    p.payload["tags"] = nlohmann::json::array();
    EXPECT_FALSE(r.applier->isLowRisk(p));

    // Tag back, oversized args (>=5) -> rule 2 fail.
    p.payload["tags"] = {"low_risk_audited"};
    p.payload["arguments"] = nlohmann::json{
        {"a",1},{"b",2},{"c",3},{"d",4},{"e",5}};
    EXPECT_FALSE(r.applier->isLowRisk(p));

    // Reset args, overlong timeout -> rule 3 fail.
    p.payload["arguments"] = nlohmann::json{{"a",1}};
    p.payload["timeout_ms"] = 30000;
    EXPECT_FALSE(r.applier->isLowRisk(p));

    // Reset timeout, tool not in safelist -> rule 1 fail.
    p.payload["timeout_ms"] = 5000;
    p.payload["tool_id"]    = "shell_exec";
    EXPECT_FALSE(r.applier->isLowRisk(p));
}
