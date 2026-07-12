#include "registry.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace aegisgate {

void ConnectorRegistry::registerConnector(std::unique_ptr<ModelConnector> connector) {
    spdlog::info("Registered connector: {}", connector->provider());
    connectors_.push_back(std::move(connector));
}

ModelConnector* ConnectorRegistry::findByProvider(const std::string& provider) const {
    for (const auto& c : connectors_) {
        if (c->provider() == provider) return c.get();
    }
    return nullptr;
}

ModelConnector* ConnectorRegistry::findByModel(const std::string& model) const {
    for (const auto& c : connectors_) {
        if (c->supportsModel(model)) return c.get();
    }
    return nullptr;
}

std::vector<ModelConnector*> ConnectorRegistry::all() const {
    std::vector<ModelConnector*> result;
    result.reserve(connectors_.size());
    for (const auto& c : connectors_) {
        result.push_back(c.get());
    }
    return result;
}

void ConnectorRegistry::registerModelInfo(const ModelInfo& info) {
    model_infos_[info.id] = info;
}

const ModelInfo* ConnectorRegistry::findModelInfo(const std::string& model) const {
    auto it = model_infos_.find(model);
    if (it != model_infos_.end()) return &it->second;
    return nullptr;
}

std::vector<const ModelInfo*> ConnectorRegistry::findModelsByTag(const std::string& tag) const {
    std::vector<const ModelInfo*> result;
    for (const auto& [id, info] : model_infos_) {
        for (const auto& t : info.tags) {
            if (t == tag) {
                result.push_back(&info);
                break;
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const ModelInfo* a, const ModelInfo* b) {
        return (a->cost_per_1k_input + a->cost_per_1k_output) <
               (b->cost_per_1k_input + b->cost_per_1k_output);
    });
    return result;
}

std::vector<ModelInfo> ConnectorRegistry::allModelInfos() const {
    std::vector<ModelInfo> result;
    result.reserve(model_infos_.size());
    for (const auto& [id, info] : model_infos_) {
        result.push_back(info);
    }
    return result;
}

std::string ConnectorRegistry::defaultModel() const {
    return default_model_;
}

void ConnectorRegistry::setDefaultModel(const std::string& model) {
    default_model_ = model;
}

} // namespace aegisgate
