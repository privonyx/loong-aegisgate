#include "agent/tool_registry.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace aegisgate {

bool ToolRegistry::registerTool(ToolDefinition tool) {
    std::unique_lock lock(mutex_);
    if (tools_.count(tool.id)) {
        spdlog::warn("Tool already registered: {}", tool.id);
        return false;
    }
    auto id = tool.id;
    tools_.emplace(std::move(id), std::move(tool));
    return true;
}

bool ToolRegistry::unregisterTool(const std::string& id) {
    std::unique_lock lock(mutex_);
    return tools_.erase(id) > 0;
}

std::optional<ToolDefinition> ToolRegistry::getTool(const std::string& id) const {
    std::shared_lock lock(mutex_);
    auto it = tools_.find(id);
    if (it == tools_.end()) return std::nullopt;
    return it->second;
}

std::vector<ToolDefinition> ToolRegistry::listTools(
    const std::vector<std::string>& tags_filter) const {
    std::shared_lock lock(mutex_);
    std::vector<ToolDefinition> result;
    for (const auto& [_, tool] : tools_) {
        if (!tool.enabled) continue;

        if (!tags_filter.empty()) {
            bool match = false;
            for (const auto& filter_tag : tags_filter) {
                if (std::find(tool.tags.begin(), tool.tags.end(), filter_tag) !=
                    tool.tags.end()) {
                    match = true;
                    break;
                }
            }
            if (!match) continue;
        }

        result.push_back(tool);
    }
    return result;
}

ToolValidationResult ToolRegistry::validateCall(
    const std::string& tool_id,
    const nlohmann::json& arguments) const {
    std::shared_lock lock(mutex_);
    auto it = tools_.find(tool_id);
    if (it == tools_.end()) {
        return ToolValidationResult{false, "Unknown tool: " + tool_id};
    }
    if (!it->second.enabled) {
        return ToolValidationResult{false, "Tool is disabled: " + tool_id};
    }

    const auto& schema = it->second.parameters_schema;
    if (schema.contains("required") && schema["required"].is_array()) {
        for (const auto& req : schema["required"]) {
            auto field = req.get<std::string>();
            if (!arguments.contains(field)) {
                return ToolValidationResult{false, "Missing required field: " + field};
            }
        }
    }

    return ToolValidationResult{true, ""};
}

size_t ToolRegistry::size() const {
    std::shared_lock lock(mutex_);
    return tools_.size();
}

void ToolRegistry::clear() {
    std::unique_lock lock(mutex_);
    tools_.clear();
}

nlohmann::json ToolRegistry::toOpenAITools() const {
    std::shared_lock lock(mutex_);
    auto result = nlohmann::json::array();
    for (const auto& [_, tool] : tools_) {
        if (!tool.enabled) continue;
        nlohmann::json func;
        func["type"] = "function";
        func["function"]["name"] = tool.name;
        func["function"]["description"] = tool.description;
        func["function"]["parameters"] = tool.parameters_schema;
        result.push_back(std::move(func));
    }
    return result;
}

} // namespace aegisgate
