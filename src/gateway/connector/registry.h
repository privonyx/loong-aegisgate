#pragma once
#include "base.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aegisgate {

class ConnectorRegistry {
public:
    void registerConnector(std::unique_ptr<ModelConnector> connector);

    ModelConnector* findByProvider(const std::string& provider) const;
    ModelConnector* findByModel(const std::string& model) const;
    const ModelInfo* findModelInfo(const std::string& model) const;
    std::vector<ModelConnector*> all() const;

    void registerModelInfo(const ModelInfo& info);
    std::vector<const ModelInfo*> findModelsByTag(const std::string& tag) const;
    std::vector<ModelInfo> allModelInfos() const;
    std::string defaultModel() const;
    void setDefaultModel(const std::string& model);

private:
    std::vector<std::unique_ptr<ModelConnector>> connectors_;
    std::unordered_map<std::string, ModelInfo> model_infos_;
    std::string default_model_;
};

} // namespace aegisgate
