#pragma once
#include "base.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aegisgate {

class ConnectorFactory {
public:
    using Creator = std::function<std::unique_ptr<ModelConnector>(const ProviderConfig&)>;

    void registerType(const std::string& type, Creator creator);
    std::unique_ptr<ModelConnector> create(const ProviderConfig& config) const;
    bool hasType(const std::string& type) const;
    std::vector<std::string> registeredTypes() const;

    static ConnectorFactory& defaults();

private:
    std::unordered_map<std::string, Creator> creators_;
};

} // namespace aegisgate
