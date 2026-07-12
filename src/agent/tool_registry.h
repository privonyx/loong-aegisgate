#pragma once
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <shared_mutex>
#include <nlohmann/json.hpp>

namespace aegisgate {

struct ToolDefinition {
    std::string id;
    std::string name;
    std::string description;
    std::string version;
    nlohmann::json parameters_schema;
    int timeout_ms = 30000;
    int max_concurrent = 10;
    int rate_limit_max_calls = 100;
    int rate_limit_window_seconds = 60;
    bool enabled = true;
    std::vector<std::string> tags;

    ToolDefinition() = default;
    ToolDefinition(std::string id_, std::string name_, std::string desc)
        : id(std::move(id_)), name(std::move(name_)), description(std::move(desc)) {}
};

struct ToolValidationResult {
    bool valid = false;
    std::string error;

    ToolValidationResult() = default;
    ToolValidationResult(bool v, std::string e)
        : valid(v), error(std::move(e)) {}
};

class ToolRegistry {
public:
    bool registerTool(ToolDefinition tool);
    bool unregisterTool(const std::string& id);
    std::optional<ToolDefinition> getTool(const std::string& id) const;
    std::vector<ToolDefinition> listTools(const std::vector<std::string>& tags_filter = {}) const;
    ToolValidationResult validateCall(const std::string& tool_id,
                                      const nlohmann::json& arguments) const;
    size_t size() const;
    void clear();

    nlohmann::json toOpenAITools() const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, ToolDefinition> tools_;
};

} // namespace aegisgate
