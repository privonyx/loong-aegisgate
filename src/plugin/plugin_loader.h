#pragma once
#include "plugin/plugin_stage.h"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aegisgate {

class PluginLoader {
public:
    struct LoadedPlugin {
        std::string path;
        std::string name;
        std::string version;
        std::string description;
        int api_version = 0;
        void* handle = nullptr;
        PluginCreateFunc create = nullptr;
        PluginDestroyFunc destroy = nullptr;
    };

    ~PluginLoader();

    bool load(const std::string& so_path);
    void unload(const std::string& name);
    std::unique_ptr<PipelineStage> createStage(const std::string& name);
    std::vector<LoadedPlugin> listLoaded() const;
    bool isLoaded(const std::string& name) const;

private:
    std::unordered_map<std::string, LoadedPlugin> plugins_;
    mutable std::mutex mutex_;
};

}  // namespace aegisgate
