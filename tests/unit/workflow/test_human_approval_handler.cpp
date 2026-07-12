// Phase 11.3 TASK-20260523-02 — Epic 4.1: HumanApprovalNodeHandler.
//
// When the WorkflowEngine reaches a NodeType::HumanApproval node it calls
// HumanApprovalNodeHandler::enqueue() which crafts an ApprovalProposal and
// pushes it into AutonomyApprovalWorkflow. This test covers:
//   - propose() invoked with AutonomySource::Workflow
//   - payload carries run_id + node_id + dsl_hash for round-trip safety
//   - decision_trace populated with required 4 fields
//   - SR-NEW4 PII scrub on payload + decision_trace strings

#include "guardrail/audit.h"
#include "guardrail/inbound/pii_filter.h"
#include "observe/autonomy/approval_queue.h"
#include "observe/autonomy/approval_workflow.h"
#include "storage/memory_persistent_store.h"
#include "workflow/human_approval_node_handler.h"

#include <gtest/gtest.h>

namespace aw = aegisgate::workflow;

namespace {

struct HandlerFixture {
    std::shared_ptr<aegisgate::MemoryPersistentStore>        store;
    std::shared_ptr<aegisgate::autonomy::ApprovalQueue>      queue;
    std::shared_ptr<aegisgate::AuditLogger>                   audit;
    std::shared_ptr<aegisgate::autonomy::AutonomyApprovalWorkflow> wf;
    std::shared_ptr<aegisgate::PIIFilter>                     pii;

    HandlerFixture() {
        store = std::make_shared<aegisgate::MemoryPersistentStore>();
        store->initialize();
        queue = std::make_shared<aegisgate::autonomy::ApprovalQueue>(store.get());
        queue->initialize();
        audit = std::make_shared<aegisgate::AuditLogger>();
        pii   = std::make_shared<aegisgate::PIIFilter>();
        wf = std::make_shared<aegisgate::autonomy::AutonomyApprovalWorkflow>(
                queue, audit, pii);
        wf->setAutonomyEnabledOverride(true);
    }
};

aw::NodeSpec makeApprovalNode(const std::string& id) {
    aw::NodeSpec n;
    n.id      = id;
    n.type    = aw::NodeType::HumanApproval;
    n.tool_id = "security_reviewer";
    n.arguments = {{"reason", "deploy to prod"},
                   {"email", "alice@example.com"}};
    return n;
}

} // namespace

TEST(HumanApprovalNodeHandlerTest, EnqueueCreatesProposalWithWorkflowSource) {
    HandlerFixture f;
    aw::HumanApprovalNodeHandler h(f.wf, f.pii);
    auto node = makeApprovalNode("approval-1");

    auto id = h.enqueue("run-A", "wf_demo", "hash_abc", node,
                         nlohmann::json{{"trace_id", "t-1"}});
    ASSERT_FALSE(id.empty());

    auto prop = f.wf->get(id);
    ASSERT_TRUE(prop.has_value());
    EXPECT_EQ(prop->source,
               aegisgate::autonomy::AutonomySource::Workflow);
    EXPECT_EQ(prop->payload.value("run_id", std::string{}), "run-A");
    EXPECT_EQ(prop->payload.value("node_id", std::string{}), "approval-1");
    EXPECT_EQ(prop->payload.value("dsl_hash", std::string{}), "hash_abc");
    EXPECT_EQ(prop->payload.value("workflow_id", std::string{}), "wf_demo");
}

TEST(HumanApprovalNodeHandlerTest, DecisionTraceCarriesRequiredFields) {
    HandlerFixture f;
    aw::HumanApprovalNodeHandler h(f.wf, f.pii);
    auto node = makeApprovalNode("approval-2");
    auto id = h.enqueue("run-B", "wf_demo", "hash_xyz", node,
                         nlohmann::json::object());
    ASSERT_FALSE(id.empty());

    auto prop = f.wf->get(id);
    ASSERT_TRUE(prop.has_value());
    for (const auto& field : {"source_id", "algorithm_name",
                                "input_hash_sha256", "proposed_at_ms"}) {
        EXPECT_TRUE(prop->decision_trace.contains(field))
            << "missing trace field " << field;
    }
    EXPECT_EQ(prop->decision_trace.value("source_id", std::string{}),
                "run-B");
}

TEST(HumanApprovalNodeHandlerTest, ScrubsPiiFromPayloadStrings) {
    // SR-NEW4 anchor — DSL or runtime context that smuggles PII must be
    // masked before it reaches the approval queue / audit log.
    HandlerFixture f;
    aw::HumanApprovalNodeHandler h(f.wf, f.pii);
    auto node = makeApprovalNode("approval-3");
    auto id = h.enqueue("run-C", "wf_demo", "hash_pii", node,
                         nlohmann::json{{"contact",
                                          "call me at 13800138000"}});
    ASSERT_FALSE(id.empty());

    auto prop = f.wf->get(id);
    ASSERT_TRUE(prop.has_value());
    // Original phone / email present in node.arguments should be redacted.
    auto args_dump = prop->payload.dump();
    EXPECT_EQ(args_dump.find("13800138000"), std::string::npos)
        << "phone leaked: " << args_dump;
    EXPECT_EQ(args_dump.find("alice@example.com"), std::string::npos)
        << "email leaked: " << args_dump;
}

TEST(HumanApprovalNodeHandlerTest, RefusesEnqueueWhenAutonomyDisabled) {
    HandlerFixture f;
    f.wf->setAutonomyEnabledOverride(false);
    aw::HumanApprovalNodeHandler h(f.wf, f.pii);
    auto id = h.enqueue("run-D", "wf_demo", "hash", makeApprovalNode("a"),
                         nlohmann::json::object());
    EXPECT_TRUE(id.empty());
}
