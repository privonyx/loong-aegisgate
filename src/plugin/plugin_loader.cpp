#include "plugin/plugin_loader.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <dlfcn.h>

namespace aegisgate {

namespace {

class SharedPluginStage : public PipelineStage {
public:
    SharedPluginStage(std::shared_ptr<PipelineStage> impl,
                       PluginDestroyFunc destroy)
        : impl_(std::move(impl)) {
        (void)destroy;
    }

    StageResult process(RequestContext& ctx) override {
        try {
            return impl_->process(ctx);
        } catch (const std::exception& e) {
            spdlog::error("Plugin '{}' threw exception: {}", impl_->name(), e.what());
            return StageResult::Continue;
        } catch (...) {
            spdlog::error("Plugin '{}' threw unknown exception", impl_->name());
            return StageResult::Continue;
        }
    }

    StageResult processChunk(RequestContext& ctx, std::string_view chunk) override {
        try {
            return impl_->processChunk(ctx, chunk);
        } catch (...) {
            return StageResult::Continue;
        }
    }

    std::string name() const override { return impl_->name(); }

private:
    std::shared_ptr<PipelineStage> impl_;
};

}  // namespace

PluginLoader::~PluginLoader() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, plugin] : plugins_) {
        if (plugin.handle) {
            dlclose(plugin.handle);
            spdlog::debug("Plugin '{}' unloaded", name);
        }
    }
    plugins_.clear();
}

bool PluginLoader::load(const std::string& so_path) {
    void* handle = dlopen(so_path.c_str(), RTLD_NOW);
    if (!handle) {
        spdlog::error("PluginLoader: failed to load '{}': {}", so_path, dlerror());
        return false;
    }

    auto create_fn = reinterpret_cast<PluginCreateFunc>(
        dlsym(handle, "aegisgate_plugin_create"));
    auto destroy_fn = reinterpret_cast<PluginDestroyFunc>(
        dlsym(handle, "aegisgate_plugin_destroy"));
    auto info_fn = reinterpret_cast<PluginInfoFunc>(
        dlsym(handle, "aegisgate_plugin_info"));

    if (!create_fn || !destroy_fn || !info_fn) {
        spdlog::error("PluginLoader: '{}' missing required exports "
                       "(aegisgate_plugin_create/destroy/info)", so_path);
        dlclose(handle);
        return false;
    }

    std::string name, version, description;
    int api_ver = 0;
    try {
        const char* info_str = info_fn();
        auto info = nlohmann::json::parse(info_str);
        name = info.value("name", "");
        version = info.value("version", "0.0.0");
        description = info.value("description", "");

        api_ver = info.value("api_version", 0);
        if (api_ver != 0 && api_ver != AEGISGATE_PLUGIN_API_VERSION) {
            spdlog::warn("PluginLoader: '{}' api_version={} (expected {}), "
                          "may be incompatible",
                          so_path, api_ver, AEGISGATE_PLUGIN_API_VERSION);
        }
    } catch (const std::exception& e) {
        spdlog::error("PluginLoader: '{}' info() returned invalid JSON: {}",
                       so_path, e.what());
        dlclose(handle);
        return false;
    }

    if (name.empty()) {
        spdlog::error("PluginLoader: '{}' info() returned empty name", so_path);
        dlclose(handle);
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (plugins_.count(name)) {
        spdlog::warn("PluginLoader: plugin '{}' already loaded, skipping", name);
        dlclose(handle);
        return false;
    }

    LoadedPlugin lp;
    lp.path = so_path;
    lp.name = name;
    lp.version = version;
    lp.description = description;
    lp.api_version = api_ver;
    lp.handle = handle;
    lp.create = create_fn;
    lp.destroy = destroy_fn;
    plugins_[name] = std::move(lp);
    spdlog::info("Plugin loaded: {} v{} from {}", name, version, so_path);
    return true;
}

void PluginLoader::unload(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = plugins_.find(name);
    if (it == plugins_.end()) return;
    if (it->second.handle) {
        dlclose(it->second.handle);
    }
    plugins_.erase(it);
    spdlog::info("Plugin unloaded: {}", name);
}

std::unique_ptr<PipelineStage> PluginLoader::createStage(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = plugins_.find(name);
    if (it == plugins_.end()) {
        spdlog::error("PluginLoader: plugin '{}' not loaded", name);
        return nullptr;
    }

    PipelineStage* raw = it->second.create();
    if (!raw) {
        spdlog::error("PluginLoader: plugin '{}' create() returned nullptr", name);
        return nullptr;
    }

    auto shared = std::shared_ptr<PipelineStage>(raw,
        [destroy = it->second.destroy](PipelineStage* p) {
            if (destroy) destroy(p);
        });

    return std::make_unique<SharedPluginStage>(std::move(shared),
                                                it->second.destroy);
}

std::vector<PluginLoader::LoadedPlugin> PluginLoader::listLoaded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LoadedPlugin> result;
    result.reserve(plugins_.size());
    for (const auto& [_, p] : plugins_) {
        result.push_back(p);
    }
    return result;
}

bool PluginLoader::isLoaded(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return plugins_.count(name) > 0;
}

}  // namespace aegisgate
