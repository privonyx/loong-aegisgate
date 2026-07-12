#pragma once

// Phase 11.1 TASK-20260523-01 — Epic 1.2 in-memory IGuardModelRegistry.
//
// Pure in-RAM implementation used by unit tests and for ephemeral preview
// installs. Thread-safe via an internal shared_mutex (reads are concurrent;
// writes hold exclusive).

#include "guardrail/model/i_guard_model_registry.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace aegisgate::guard {

class MemoryGuardModelRegistry : public IGuardModelRegistry {
public:
    MemoryGuardModelRegistry() = default;

    RegistryOpResult insert(const ModelRegistryRecord& record) override;
    std::optional<ModelRegistryRecord> get(
        const std::string& model_id, const std::string& version) const override;
    std::vector<ModelRegistryRecord> list(
        const std::string& model_id) const override;
    std::vector<ModelRegistryRecord> listByStatus(
        const std::string& model_id, GuardModelStatus status) const override;
    RegistryOpResult promote(const std::string& model_id,
                             const std::string& version,
                             std::int64_t promoted_at_ms) override;
    RegistryOpResult revert(const std::string& model_id,
                            const std::string& version) override;

private:
    static std::string key(const std::string& model_id,
                           const std::string& version) {
        return model_id + "::" + version;
    }

    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, ModelRegistryRecord> store_;
};

}  // namespace aegisgate::guard
