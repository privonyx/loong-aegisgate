#pragma once
#include "agent/tool_registry.h"
#include "agent/tool_sandbox.h"
#include "aegisgate/types.h"
#include <string>
#include <vector>
#include <chrono>
#include <functional>

namespace aegisgate {

enum class OrchestrationMode { ReAct, PlanAndExecute };

struct OrchestrationConfig {
    int max_steps = 10;
    int max_total_timeout_ms = 120000;
    OrchestrationMode mode = OrchestrationMode::ReAct;

    OrchestrationConfig() = default;
    OrchestrationConfig(int steps, int timeout_ms, OrchestrationMode m)
        : max_steps(steps), max_total_timeout_ms(timeout_ms), mode(m) {}
};

struct OrchestrationStep {
    int step_number = 0;
    std::string thought;
    std::string tool_id;
    nlohmann::json tool_arguments;
    std::optional<ToolExecutionResult> tool_result;
    std::chrono::steady_clock::time_point timestamp;

    OrchestrationStep() : timestamp(std::chrono::steady_clock::now()) {}
};

struct OrchestrationResult {
    bool success = false;
    std::string final_answer;
    std::vector<OrchestrationStep> steps;
    int total_steps = 0;
    double total_duration_ms = 0.0;
    std::string error;

    OrchestrationResult() = default;
};

class Orchestrator {
public:
    // D1 (TASK-20260703-04): LLM 驱动函数。设置后 run() 每步以累积对话调用真实
    // 模型（provider-agnostic），未设置时保留 legacy「自解析 user_query」模式
    // （供无 LLM 单元测试 / tool-only 流程使用）。
    using LlmFn = std::function<ChatResponse(const ChatRequest&)>;

    Orchestrator();
    explicit Orchestrator(OrchestrationConfig config);

    void setToolRegistry(ToolRegistry* registry);
    void setToolSandbox(ToolSandbox* sandbox);
    void setLlmFn(LlmFn fn);

    OrchestrationResult run(const std::string& user_query,
                             const std::string& model = "");

    const OrchestrationConfig& config() const { return config_; }

private:
    bool parseToolCall(const std::string& llm_output,
                       std::string& tool_id,
                       nlohmann::json& arguments) const;
    std::string buildSystemPrompt() const;

    OrchestrationConfig config_;
    ToolRegistry* registry_ = nullptr;
    ToolSandbox* sandbox_ = nullptr;
    LlmFn llm_fn_;
};

} // namespace aegisgate
