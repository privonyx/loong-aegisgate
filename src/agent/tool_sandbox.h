#pragma once
#include "agent/tool_registry.h"
#include <string>
#include <functional>
#include <atomic>

namespace aegisgate {

enum class ToolExecutionStatus { Success, Timeout, Error, Rejected, RateLimited };

struct ToolExecutionResult {
    std::string tool_id;
    ToolExecutionStatus status = ToolExecutionStatus::Error;
    std::string output;
    double duration_ms = 0.0;
    std::string error_message;

    ToolExecutionResult() = default;
    ToolExecutionResult(std::string id_, ToolExecutionStatus s, std::string out)
        : tool_id(std::move(id_)), status(s), output(std::move(out)) {}
};

using ToolExecutor = std::function<std::string(const std::string& tool_id,
                                                const nlohmann::json& arguments)>;

class ToolSandbox {
public:
    static constexpr size_t kMaxToolOutputBytes = 1048576;

    explicit ToolSandbox(ToolRegistry* registry);

    void setExecutor(ToolExecutor executor);

    ToolExecutionResult execute(const std::string& tool_id,
                                 const nlohmann::json& arguments);

    size_t totalExecutions() const { return total_executions_.load(); }
    size_t failedExecutions() const { return failed_executions_.load(); }

private:
    ToolRegistry* registry_;
    ToolExecutor executor_;
    std::atomic<size_t> total_executions_{0};
    std::atomic<size_t> failed_executions_{0};
};

} // namespace aegisgate
