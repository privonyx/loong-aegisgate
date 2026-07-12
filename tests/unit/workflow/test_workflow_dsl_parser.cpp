// Phase 11.3 TASK-20260523-02 — Epic 1.2 + 1.3: DSL parser + validators.
//
// parseWorkflowDslYaml handles the wire format the Engine accepts. The two
// validators guard SR-NEW3 (cycle detection) and SR3 (sandbox bypass).

#include "workflow/workflow_dsl_parser.h"

#include <gtest/gtest.h>

namespace aw = aegisgate::workflow;

namespace {

const char* kValidYaml = R"YAML(
id: wf_demo
version: v1
description: parser sample
nodes:
  - id: n1
    type: tool
    tool_id: http_fetch
    arguments:
      url: https://example.com
    timeout_ms: 5000
  - id: n2
    type: human_approval
    tool_id: security_admin
    depends_on:
      - n1
    timeout_ms: 60000
  - id: n3
    type: tool
    tool_id: render_response
    depends_on:
      - n2
    retry:
      max_attempts: 3
      backoff_ms: 100
)YAML";

const char* kCyclicYaml = R"YAML(
id: wf_cycle
version: v1
nodes:
  - id: a
    type: tool
    tool_id: t
    depends_on: [b]
  - id: b
    type: tool
    tool_id: t
    depends_on: [a]
)YAML";

const char* kSelfLoopYaml = R"YAML(
id: wf_self
version: v1
nodes:
  - id: a
    type: tool
    tool_id: t
    depends_on: [a]
)YAML";

const char* kDuplicateIdYaml = R"YAML(
id: wf_dup
version: v1
nodes:
  - id: a
    type: tool
    tool_id: t
  - id: a
    type: tool
    tool_id: t2
)YAML";

const char* kMissingDepYaml = R"YAML(
id: wf_dep
version: v1
nodes:
  - id: a
    type: tool
    tool_id: t
    depends_on: [missing]
)YAML";

} // namespace

TEST(WorkflowDslParserTest, ParsesValidYamlIntoDsl) {
    auto r = aw::parseWorkflowDslYaml(kValidYaml);
    ASSERT_TRUE(r.ok) << "errors: " << (r.errors.empty() ? "" : r.errors[0]);
    ASSERT_TRUE(r.dsl.has_value());
    EXPECT_EQ(r.dsl->id, "wf_demo");
    EXPECT_EQ(r.dsl->nodes.size(), 3u);
    EXPECT_EQ(r.dsl->nodes[1].type, aw::NodeType::HumanApproval);
    EXPECT_EQ(r.dsl->nodes[2].retry.max_attempts, 3);
    EXPECT_EQ(r.dsl->nodes[2].retry.backoff_ms, 100);
}

TEST(WorkflowDslParserTest, RejectsMalformedYaml) {
    auto r = aw::parseWorkflowDslYaml("nodes: [garbage:");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.errors.empty());
}

TEST(WorkflowDslParserTest, ValidateNoCycleAcceptsLinearDag) {
    auto r = aw::parseWorkflowDslYaml(kValidYaml);
    ASSERT_TRUE(r.ok);
    std::string err_node;
    EXPECT_TRUE(aw::validateNoCycle(*r.dsl, &err_node));
    EXPECT_TRUE(err_node.empty());
}

TEST(WorkflowDslParserTest, ValidateNoCycleDetectsTwoNodeCycle) {
    auto r = aw::parseWorkflowDslYaml(kCyclicYaml);
    ASSERT_TRUE(r.ok);
    std::string err_node;
    EXPECT_FALSE(aw::validateNoCycle(*r.dsl, &err_node));
    EXPECT_FALSE(err_node.empty());
}

TEST(WorkflowDslParserTest, ValidateNoCycleDetectsSelfLoop) {
    auto r = aw::parseWorkflowDslYaml(kSelfLoopYaml);
    ASSERT_TRUE(r.ok);
    std::string err_node;
    EXPECT_FALSE(aw::validateNoCycle(*r.dsl, &err_node));
    EXPECT_EQ(err_node, "a");
}

TEST(WorkflowDslParserTest, ParseRejectsDuplicateNodeIds) {
    auto r = aw::parseWorkflowDslYaml(kDuplicateIdYaml);
    EXPECT_FALSE(r.ok);
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors[0].find("duplicate"), std::string::npos);
}

TEST(WorkflowDslParserTest, ParseRejectsMissingDependency) {
    auto r = aw::parseWorkflowDslYaml(kMissingDepYaml);
    EXPECT_FALSE(r.ok);
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors[0].find("missing"), std::string::npos);
}

TEST(WorkflowDslParserTest, DslRejectsUnsandboxedToolNode) {
    // SR3 anchor — M1 mutation target. The parser is allowed to accept a
    // tool_id-less node (it only checks structural shape), but
    // validateNoSandboxBypass must refuse it so the Engine can never
    // dispatch a node outside ToolSandbox::execute.
    aw::WorkflowDsl dsl;
    dsl.id      = "wf_bypass";
    dsl.version = "v1";
    aw::NodeSpec n;
    n.id      = "exec";
    n.type    = aw::NodeType::Tool;
    n.tool_id = "";  // bypass attempt
    dsl.nodes.push_back(n);

    std::vector<std::string> offenders;
    EXPECT_FALSE(aw::validateNoSandboxBypass(dsl, /*registry=*/nullptr, &offenders));
    ASSERT_EQ(offenders.size(), 1u);
    EXPECT_EQ(offenders[0], "exec");
}

TEST(WorkflowDslParserTest, JsonParserIsEquivalent) {
    auto y = aw::parseWorkflowDslYaml(kValidYaml);
    ASSERT_TRUE(y.ok);
    auto j_text = aw::toJson(*y.dsl).dump();
    auto j = aw::parseWorkflowDslJson(j_text);
    ASSERT_TRUE(j.ok);
    EXPECT_EQ(j.dsl->id, y.dsl->id);
    EXPECT_EQ(j.dsl->nodes.size(), y.dsl->nodes.size());
}
