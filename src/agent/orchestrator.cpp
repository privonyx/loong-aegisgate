#include "agent/orchestrator.h"
#include "observe/metrics.h"
#include <spdlog/spdlog.h>

namespace aegisgate {

Orchestrator::Orchestrator() = default;

Orchestrator::Orchestrator(OrchestrationConfig config)
    : config_(std::move(config)) {}

void Orchestrator::setToolRegistry(ToolRegistry* registry) {
    registry_ = registry;
}

void Orchestrator::setToolSandbox(ToolSandbox* sandbox) {
    sandbox_ = sandbox;
}

void Orchestrator::setLlmFn(LlmFn fn) {
    llm_fn_ = std::move(fn);
}

std::string Orchestrator::buildSystemPrompt() const {
    // D1=A（TASK-20260703-04）：text-based ReAct，provider-agnostic。注入工具清单
    // + 调用/终止约定。工具以 JSON schema 文本呈现，模型输出 tool-call JSON 或
    // "Final Answer:" 收敛。
    std::string prompt =
        "You are a ReAct agent. On each turn either call a tool or give a final "
        "answer.\n"
        "To call a tool, respond with a single JSON object on its own line:\n"
        "{\"tool\": \"<tool_id>\", \"arguments\": { ... }}\n"
        "To finish, respond with plain text beginning with \"Final Answer:\".\n"
        "Only use tools from the list below. Do not invent tools.\n";
    if (registry_) {
        auto tools = registry_->listTools();
        if (!tools.empty()) {
            prompt += "\nAvailable tools:\n";
            for (const auto& t : tools) {
                if (!t.enabled) continue;
                prompt += "- " + t.id + ": " + t.description +
                          " schema=" + t.parameters_schema.dump() + "\n";
            }
        }
    }
    return prompt;
}

OrchestrationResult Orchestrator::run(const std::string& user_query,
                                       const std::string& model) {
    auto start = std::chrono::steady_clock::now();
    OrchestrationResult result;

    // D1=A（TASK-20260703-04）：llm_fn_ 设置时以累积对话驱动真实模型；未设置时
    // 保留 legacy「自解析」模式（无 LLM 单测 / tool-only）。
    // C10 (TASK-20260703-02): ReAct 输入随工具结果演进（多步反馈），而非每步都用
    // 原始 user_query。修复前循环体第一步无条件 return → max_steps 死配置。
    std::vector<Message> messages;
    if (llm_fn_) {
        Message sys;
        sys.role = "system";
        sys.content = buildSystemPrompt();
        messages.push_back(std::move(sys));
        Message usr;
        usr.role = "user";
        usr.content = user_query;
        messages.push_back(std::move(usr));
    }
    std::string current_input = user_query;  // legacy-mode 演进输入

    for (int step = 0; step < config_.max_steps; ++step) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(now - start).count();
        if (elapsed >= config_.max_total_timeout_ms) {
            result.error = "Orchestration timeout exceeded";
            spdlog::warn("Orchestration timeout after {} steps", step);
            break;
        }

        OrchestrationStep os;
        os.step_number = step + 1;
        os.timestamp = std::chrono::steady_clock::now();
        // P2-#5: count each orchestration step taken so agent_steps_total is no
        // longer a permanently-zero metric.
        MetricsRegistry::instance().agentStepsTotal().inc();

        // 取本步「LLM 输出」：真实模型或 legacy 自解析输入。
        std::string llm_output;
        if (llm_fn_) {
            ChatRequest req;
            req.model = model;
            req.messages = messages;
            ChatResponse resp = llm_fn_(req);
            llm_output = resp.content;
            Message asst;
            asst.role = "assistant";
            asst.content = llm_output;
            messages.push_back(std::move(asst));
        } else {
            llm_output = current_input;
        }
        os.thought = llm_output;

        std::string tool_id;
        nlohmann::json arguments;
        bool has_tool_call = parseToolCall(llm_output, tool_id, arguments);

        if (!has_tool_call) {
            // 无工具调用 = 最终答案，正常收敛终止。
            result.steps.push_back(std::move(os));
            result.success = true;
            result.final_answer = llm_output;
            break;
        }

        // SR-2 (TASK-20260703-04)：仅执行注册且参数合规的工具，杜绝 LLM 越权/幻觉
        // 工具调用越过沙箱。registry_ 未设时（legacy 无注册表单测）跳过此门禁。
        if (registry_) {
            auto validation = registry_->validateCall(tool_id, arguments);
            if (!validation.valid) {
                result.steps.push_back(std::move(os));
                result.success = false;
                result.error = "Rejected tool call: " + validation.error;
                break;
            }
        }

        if (!sandbox_) {
            // C10 FIX: 有工具调用但无 sandbox → 无法执行，必须报错而非误报成功
            //（修复前落入成功分支返回 success=true）。
            result.steps.push_back(std::move(os));
            result.success = false;
            result.error = "Tool call requested but no sandbox configured";
            break;
        }

        // 执行工具，并把结果反馈为下一步输入（多步 ReAct）。
        os.tool_id = tool_id;
        os.tool_arguments = arguments;
        auto exec_result = sandbox_->execute(tool_id, arguments);
        std::string output = exec_result.output;
        os.tool_result = std::move(exec_result);
        result.steps.push_back(std::move(os));
        result.success = true;
        result.final_answer = output;

        if (llm_fn_) {
            Message obs;
            obs.role = "user";
            obs.content = "Observation: " + output;
            messages.push_back(std::move(obs));
        } else {
            current_input = output;
        }
    }

    if (result.steps.empty() && result.error.empty()) {
        result.error = "Max steps reached without resolution";
    }
    auto end = std::chrono::steady_clock::now();
    result.total_duration_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    result.total_steps = static_cast<int>(result.steps.size());
    return result;
}

bool Orchestrator::parseToolCall(const std::string& llm_output,
                                  std::string& tool_id,
                                  nlohmann::json& arguments) const {
    // SR-2 / I34 加固（TASK-20260703-04）：从首个 '{' 起做括号平衡扫描（字符串/
    // 转义感知），只截取第一个平衡 JSON 对象；修复前 substr 到结尾遇尾随文本
    // （"...} done"）即 parse 失败漏判工具调用。
    auto begin = llm_output.find('{');
    if (begin == std::string::npos) return false;

    int depth = 0;
    bool in_str = false;
    bool escaped = false;
    size_t end = std::string::npos;
    for (size_t i = begin; i < llm_output.size(); ++i) {
        char c = llm_output[i];
        if (in_str) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_str = false;
            }
        } else if (c == '"') {
            in_str = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) {
                end = i;
                break;
            }
        }
    }
    if (end == std::string::npos) return false;

    try {
        auto parsed = nlohmann::json::parse(llm_output.substr(begin, end - begin + 1));
        if (parsed.is_object() && parsed.contains("tool") &&
            parsed["tool"].is_string() && parsed.contains("arguments")) {
            tool_id = parsed["tool"].get<std::string>();
            arguments = parsed["arguments"];
            return true;
        }
    } catch (const nlohmann::json::exception&) {
        // Not valid JSON or not in expected format
    }

    return false;
}

} // namespace aegisgate
