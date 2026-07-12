#include "agent/tool_sandbox.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace aegisgate {

ToolSandbox::ToolSandbox(ToolRegistry* registry) : registry_(registry) {}

void ToolSandbox::setExecutor(ToolExecutor executor) {
    executor_ = std::move(executor);
}

ToolExecutionResult ToolSandbox::execute(const std::string& tool_id,
                                          const nlohmann::json& arguments) {
    ++total_executions_;

    if (!executor_) {
        ++failed_executions_;
        ToolExecutionResult r;
        r.tool_id = tool_id;
        r.status = ToolExecutionStatus::Error;
        r.error_message = "No tool executor configured";
        return r;
    }

    auto tool_opt = registry_->getTool(tool_id);
    if (!tool_opt) {
        ++failed_executions_;
        ToolExecutionResult r;
        r.tool_id = tool_id;
        r.status = ToolExecutionStatus::Error;
        r.error_message = "Unknown tool: " + tool_id;
        return r;
    }

    if (!tool_opt->enabled) {
        ++failed_executions_;
        ToolExecutionResult r;
        r.tool_id = tool_id;
        r.status = ToolExecutionStatus::Rejected;
        r.error_message = "Tool is disabled: " + tool_id;
        return r;
    }

    auto validation = registry_->validateCall(tool_id, arguments);
    if (!validation.valid) {
        ++failed_executions_;
        ToolExecutionResult r;
        r.tool_id = tool_id;
        r.status = ToolExecutionStatus::Error;
        r.error_message = validation.error;
        return r;
    }

    auto start = std::chrono::steady_clock::now();
    try {
        auto output = executor_(tool_id, arguments);
        auto end = std::chrono::steady_clock::now();
        double duration = std::chrono::duration<double, std::milli>(end - start).count();

        if (output.size() > kMaxToolOutputBytes) {
            spdlog::warn("Tool {} output truncated from {} to {} bytes",
                         tool_id, output.size(), kMaxToolOutputBytes);
            output.resize(kMaxToolOutputBytes);
        }

        ToolExecutionResult r;
        r.tool_id = tool_id;
        r.status = ToolExecutionStatus::Success;
        r.output = std::move(output);
        r.duration_ms = duration;
        return r;
    } catch (const std::exception& e) {
        auto end = std::chrono::steady_clock::now();
        double duration = std::chrono::duration<double, std::milli>(end - start).count();
        ++failed_executions_;
        spdlog::error("Tool {} execution failed: {}", tool_id, e.what());
        ToolExecutionResult r;
        r.tool_id = tool_id;
        r.status = ToolExecutionStatus::Error;
        r.error_message = e.what();
        r.duration_ms = duration;
        return r;
    }
}

} // namespace aegisgate
