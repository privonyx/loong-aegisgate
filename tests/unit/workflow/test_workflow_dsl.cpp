// Phase 11.3 TASK-20260523-02 — Epic 1.1: WorkflowDsl POD + canonical JSON.
//
// Verifies the immutable data contracts that downstream Engine / StateStore /
// HumanApprovalNodeHandler rely on. canonicalHash gives a stable identifier
// the audit log uses to detect DSL tampering between propose and execute.

#include "workflow/workflow_dsl.h"

#include <gtest/gtest.h>

namespace aw = aegisgate::workflow;

namespace {

aw::WorkflowDsl makeSampleDsl() {
    aw::WorkflowDsl dsl;
    dsl.id = "wf_demo";
    dsl.version = "v1";
    dsl.description = "sample";

    aw::NodeSpec n1;
    n1.id = "n1";
    n1.type = aw::NodeType::Tool;
    n1.tool_id = "http_fetch";
    n1.arguments = {{"url", "https://example.com"}};
    n1.timeout_ms = 5000;

    aw::NodeSpec n2;
    n2.id = "n2";
    n2.type = aw::NodeType::HumanApproval;
    n2.tool_id = "reviewer_role";
    n2.depends_on = {"n1"};
    n2.timeout_ms = 60000;

    dsl.nodes = {n1, n2};
    return dsl;
}

} // namespace

TEST(WorkflowDslTest, NodeSpecRoundTripsThroughJson) {
    auto dsl = makeSampleDsl();
    auto j = aw::toJson(dsl);
    auto restored = aw::fromJson(j);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->id, dsl.id);
    EXPECT_EQ(restored->nodes.size(), dsl.nodes.size());
    EXPECT_EQ(restored->nodes[0].tool_id, "http_fetch");
    EXPECT_EQ(restored->nodes[1].depends_on.size(), 1u);
    EXPECT_EQ(restored->nodes[1].depends_on[0], "n1");
    EXPECT_EQ(restored->nodes[1].type, aw::NodeType::HumanApproval);
}

TEST(WorkflowDslTest, CanonicalHashIsStableAcrossEquivalentDsls) {
    auto a = makeSampleDsl();
    auto b = makeSampleDsl();
    // Mutate non-essential field that should not affect canonical bytes.
    b.description = "different description";
    EXPECT_EQ(aw::canonicalHash(a), aw::canonicalHash(b));
}

TEST(WorkflowDslTest, CanonicalHashChangesWhenNodeMutated) {
    auto a = makeSampleDsl();
    auto b = makeSampleDsl();
    b.nodes[0].tool_id = "shell_exec";
    EXPECT_NE(aw::canonicalHash(a), aw::canonicalHash(b));
}

TEST(WorkflowDslTest, RetryPolicyDefaultsAreConservative) {
    aw::NodeSpec n;
    EXPECT_EQ(n.retry.max_attempts, 1);
    EXPECT_EQ(n.retry.backoff_ms, 0);
    EXPECT_FALSE(n.retry.exponential);
    EXPECT_EQ(n.timeout_ms, 30000);
    EXPECT_EQ(n.type, aw::NodeType::Tool);
}
