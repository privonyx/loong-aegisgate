#include "factory.h"
#include "openai.h"
#include "claude.h"
#include <mutex>

namespace aegisgate {

void ConnectorFactory::registerType(const std::string& type, Creator creator) {
    creators_[type] = std::move(creator);
}

std::unique_ptr<ModelConnector> ConnectorFactory::create(const ProviderConfig& config) const {
    auto it = creators_.find(config.type);
    if (it == creators_.end()) return nullptr;
    return it->second(config);
}

bool ConnectorFactory::hasType(const std::string& type) const {
    return creators_.count(type) > 0;
}

std::vector<std::string> ConnectorFactory::registeredTypes() const {
    std::vector<std::string> types;
    types.reserve(creators_.size());
    for (const auto& [type, _] : creators_) {
        types.push_back(type);
    }
    return types;
}

ConnectorFactory& ConnectorFactory::defaults() {
    static ConnectorFactory factory;
    static std::once_flag flag;
    std::call_once(flag, [&] {
        auto openai_creator = [](const ProviderConfig& pc) -> std::unique_ptr<ModelConnector> {
            return std::make_unique<OpenAIConnector>(pc);
        };
        factory.registerType("openai", openai_creator);
        factory.registerType("deepseek", openai_creator);
        factory.registerType("doubao", openai_creator);
        factory.registerType("qwen", openai_creator);
        factory.registerType("openai_compatible", openai_creator);
        factory.registerType("gemini", openai_creator);
        factory.registerType("mistral", openai_creator);

        factory.registerType("claude", [](const ProviderConfig& pc) -> std::unique_ptr<ModelConnector> {
            return std::make_unique<ClaudeConnector>(pc);
        });
    });
    return factory;
}

} // namespace aegisgate
