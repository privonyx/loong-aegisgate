#include <gtest/gtest.h>
#include "agent/orchestrator.h"
#include "observe/metrics.h"

using namespace aegisgate;

TEST(OrchestratorTest, DefaultConfig) {
    Orchestrator orch;
    EXPECT_EQ(orch.config().max_steps, 10);
    EXPECT_EQ(orch.config().max_total_timeout_ms, 120000);
    EXPECT_EQ(orch.config().mode, OrchestrationMode::ReAct);
}

TEST(OrchestratorTest, CustomConfig) {
    OrchestrationConfig cfg(5, 60000, OrchestrationMode::PlanAndExecute);
    Orchestrator orch(cfg);
    EXPECT_EQ(orch.config().max_steps, 5);
    EXPECT_EQ(orch.config().max_total_timeout_ms, 60000);
    EXPECT_EQ(orch.config().mode, OrchestrationMode::PlanAndExecute);
}

TEST(OrchestratorTest, RunSimpleQuery) {
    Orchestrator orch;
    auto result = orch.run("What is the weather?");

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.final_answer, "What is the weather?");
    EXPECT_EQ(result.total_steps, 1);
    EXPECT_FALSE(result.steps.empty());
    EXPECT_GT(result.total_duration_ms, 0.0);
}

// P2-#5: running the orchestrator must advance agent_steps_total (was always 0).
TEST(OrchestratorTest, RunIncrementsAgentStepsMetric) {
    MetricsRegistry::instance().agentStepsTotal().reset();
    Orchestrator orch;
    auto result = orch.run("What is the weather?");
    EXPECT_EQ(result.total_steps, 1);
    EXPECT_DOUBLE_EQ(MetricsRegistry::instance().agentStepsTotal().get(), 1.0);
}

TEST(OrchestratorTest, RunWithToolCall) {
    ToolRegistry registry;
    ToolDefinition tool("search", "search", "Search the web");
    tool.parameters_schema = {
        {"type", "object"},
        {"properties", {{"query", {{"type", "string"}}}}},
        {"required", {"query"}}
    };
    registry.registerTool(tool);

    ToolSandbox sandbox(&registry);
    sandbox.setExecutor([](const std::string&,
                           const nlohmann::json& args) -> std::string {
        return "Results for: " + args["query"].get<std::string>();
    });

    Orchestrator orch;
    orch.setToolRegistry(&registry);
    orch.setToolSandbox(&sandbox);

    std::string query = R"({"tool": "search", "arguments": {"query": "weather"}})";
    auto result = orch.run(query);

    EXPECT_TRUE(result.success);
    // C10（TASK-20260703-02）：工具执行后把结果反馈为下一步输入并继续循环；
    // 结果非工具调用 → 第 2 步终止为最终答案 → total_steps=2（修复前单步 return=1）。
    EXPECT_EQ(result.total_steps, 2);
    ASSERT_FALSE(result.steps.empty());
    ASSERT_TRUE(result.steps[0].tool_result.has_value());
    EXPECT_EQ(result.steps[0].tool_result->status, ToolExecutionStatus::Success);
    EXPECT_EQ(result.steps[0].tool_result->output, "Results for: weather");
    EXPECT_EQ(result.final_answer, "Results for: weather");
}

// ---------------------------------------------------------------------------
// TASK-20260703-02 Epic 5 / C10 — 多步循环 + 无 sandbox 不误报成功。
// 根因：run() 循环体第一步无条件 return → max_steps 死配置；且 has_tool_call
// 但无 sandbox 时落入成功分支误报 success=true。
// ---------------------------------------------------------------------------

// C10-a：有工具调用但未配置 sandbox → 无法执行 → 必须报错，不得误报成功。
TEST(OrchestratorTest, ToolCallWithoutSandboxFails) {
    Orchestrator orch;  // 无 sandbox
    std::string query = R"({"tool": "search", "arguments": {"query": "x"}})";
    auto result = orch.run(query);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
}

// C10-b：工具结果持续产出新工具调用 → 循环应迭代直到 max_steps（修复前恒 1）。
TEST(OrchestratorTest, MultiStepLoopHonorsMaxSteps) {
    ToolRegistry registry;
    ToolDefinition tool("search", "search", "Search the web");
    tool.parameters_schema = {
        {"type", "object"},
        {"properties", {{"query", {{"type", "string"}}}}},
        {"required", {"query"}}
    };
    registry.registerTool(tool);

    ToolSandbox sandbox(&registry);
    // executor 再次返回工具调用 JSON → 迫使多步迭代直到 max_steps。
    sandbox.setExecutor([](const std::string&, const nlohmann::json&) -> std::string {
        return R"({"tool": "search", "arguments": {"query": "again"}})";
    });

    OrchestrationConfig cfg(3, 120000, OrchestrationMode::ReAct);
    Orchestrator orch(cfg);
    orch.setToolRegistry(&registry);
    orch.setToolSandbox(&sandbox);

    std::string query = R"({"tool": "search", "arguments": {"query": "start"}})";
    auto result = orch.run(query);
    EXPECT_EQ(result.total_steps, 3);  // 修复前恒为 1（单步 return）
    EXPECT_TRUE(result.success);
}

TEST(OrchestratorTest, MaxStepsLimit) {
    OrchestrationConfig cfg(1, 120000, OrchestrationMode::ReAct);
    Orchestrator orch(cfg);

    auto result = orch.run("simple query");
    EXPECT_LE(result.total_steps, 1);
}

TEST(OrchestratorTest, TimeoutEnforcement) {
    OrchestrationConfig cfg(1000, 1, OrchestrationMode::ReAct);
    Orchestrator orch(cfg);

    auto result = orch.run("quick query");
    EXPECT_LE(result.total_steps, cfg.max_steps);
}

// ---------------------------------------------------------------------------
// TASK-20260703-04 Epic 1 / D1=A — LLM 驱动的 text-based ReAct 闭环。
// ---------------------------------------------------------------------------

// D1-a：设置 llm_fn_ 后，run() 每步以累积对话调用真实模型；工具结果作为
// Observation 反馈驱动多步，直到模型给出 Final Answer 收敛。
TEST(OrchestratorTest, LlmDrivenReActLoop) {
    ToolRegistry registry;
    ToolDefinition tool("search", "search", "Search the web");
    tool.parameters_schema = {
        {"type", "object"},
        {"properties", {{"query", {{"type", "string"}}}}},
        {"required", {"query"}}
    };
    registry.registerTool(tool);

    ToolSandbox sandbox(&registry);
    sandbox.setExecutor([](const std::string&,
                           const nlohmann::json& args) -> std::string {
        return "Results for: " + args["query"].get<std::string>();
    });

    Orchestrator orch;
    orch.setToolRegistry(&registry);
    orch.setToolSandbox(&sandbox);

    int call_count = 0;
    std::string last_model;
    size_t last_msg_count = 0;
    orch.setLlmFn([&](const ChatRequest& req) -> ChatResponse {
        ++call_count;
        last_model = req.model;
        last_msg_count = req.messages.size();
        ChatResponse resp;
        if (call_count == 1) {
            resp.content = R"({"tool": "search", "arguments": {"query": "weather"}})";
        } else {
            resp.content = "Final Answer: it is sunny";
        }
        return resp;
    });

    auto result = orch.run("What is the weather?", "gpt-4o");

    EXPECT_TRUE(result.success);
    EXPECT_EQ(call_count, 2);                    // 步1 工具调用 + 步2 最终答案
    EXPECT_EQ(last_model, "gpt-4o");             // model 透传到 ChatRequest
    EXPECT_EQ(result.total_steps, 2);
    EXPECT_EQ(result.final_answer, "Final Answer: it is sunny");
    ASSERT_FALSE(result.steps.empty());
    ASSERT_TRUE(result.steps[0].tool_result.has_value());
    EXPECT_EQ(result.steps[0].tool_result->output, "Results for: weather");
    // 第 2 次调用时对话应累积 system+user+assistant+observation = 4 条。
    EXPECT_EQ(last_msg_count, 4u);
}

// D1-b / SR-2：模型请求未注册（或参数不合规）的工具时，必须拒绝且不得进入沙箱。
TEST(OrchestratorTest, RejectsUnregisteredToolCall) {
    ToolRegistry registry;
    ToolDefinition tool("search", "search", "Search the web");
    tool.parameters_schema = {
        {"type", "object"},
        {"properties", {{"query", {{"type", "string"}}}}},
        {"required", {"query"}}
    };
    registry.registerTool(tool);

    ToolSandbox sandbox(&registry);
    sandbox.setExecutor([](const std::string&, const nlohmann::json&) -> std::string {
        return "SHOULD NOT RUN";
    });

    Orchestrator orch;
    orch.setToolRegistry(&registry);
    orch.setToolSandbox(&sandbox);
    orch.setLlmFn([](const ChatRequest&) -> ChatResponse {
        ChatResponse resp;
        resp.content = R"({"tool": "danger_delete_all", "arguments": {}})";
        return resp;
    });

    auto result = orch.run("do something", "gpt-4o");

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
    EXPECT_EQ(sandbox.totalExecutions(), 0u);    // 越权工具从未进入沙箱
}

// D1-c / I34：工具调用 JSON 后带尾随文本时仍能解析（括号平衡扫描）。
// 通过 legacy 自解析模式驱动 run()，验证尾随文本不再导致漏判。
TEST(OrchestratorTest, ParsesToolCallWithTrailingText) {
    ToolRegistry registry;
    ToolDefinition tool("search", "search", "Search the web");
    tool.parameters_schema = {
        {"type", "object"},
        {"properties", {{"query", {{"type", "string"}}}}},
        {"required", {"query"}}
    };
    registry.registerTool(tool);

    ToolSandbox sandbox(&registry);
    sandbox.setExecutor([](const std::string&,
                           const nlohmann::json& args) -> std::string {
        return "Results for: " + args["query"].get<std::string>();
    });

    Orchestrator orch;
    orch.setToolRegistry(&registry);
    orch.setToolSandbox(&sandbox);

    std::string query =
        R"(Thought: I should search. {"tool": "search", "arguments": {"query": "weather"}} now executing.)";
    auto result = orch.run(query);

    ASSERT_FALSE(result.steps.empty());
    ASSERT_TRUE(result.steps[0].tool_result.has_value());
    EXPECT_EQ(result.steps[0].tool_result->output, "Results for: weather");
}
