#include "guardrail/model/memory_guard_model_registry.h"

namespace aegisgate::guard {

RegistryOpResult MemoryGuardModelRegistry::insert(
    const ModelRegistryRecord& record) {
    if (record.model_id.empty() || record.version.empty()) {
        return RegistryOpResult::fail("invalid_input",
                                       "model_id and version required");
    }
    std::unique_lock lock(mu_);
    auto k = key(record.model_id, record.version);
    if (store_.count(k)) {
        return RegistryOpResult::fail(
            "duplicate_version",
            "(" + record.model_id + ", " + record.version + ") already exists");
    }
    store_.emplace(std::move(k), record);
    return RegistryOpResult::success();
}

std::optional<ModelRegistryRecord> MemoryGuardModelRegistry::get(
    const std::string& model_id, const std::string& version) const {
    std::shared_lock lock(mu_);
    auto it = store_.find(key(model_id, version));
    if (it == store_.end()) return std::nullopt;
    return it->second;
}

std::vector<ModelRegistryRecord> MemoryGuardModelRegistry::list(
    const std::string& model_id) const {
    std::shared_lock lock(mu_);
    std::vector<ModelRegistryRecord> out;
    out.reserve(store_.size());
    for (const auto& [_, rec] : store_) {
        if (rec.model_id == model_id) out.push_back(rec);
    }
    return out;
}

std::vector<ModelRegistryRecord> MemoryGuardModelRegistry::listByStatus(
    const std::string& model_id, GuardModelStatus status) const {
    std::shared_lock lock(mu_);
    std::vector<ModelRegistryRecord> out;
    for (const auto& [_, rec] : store_) {
        if (rec.model_id == model_id && rec.status == status) out.push_back(rec);
    }
    return out;
}

RegistryOpResult MemoryGuardModelRegistry::promote(
    const std::string& model_id, const std::string& version,
    std::int64_t promoted_at_ms) {
    std::unique_lock lock(mu_);
    auto it = store_.find(key(model_id, version));
    if (it == store_.end()) {
        return RegistryOpResult::fail("not_found",
                                       "(" + model_id + ", " + version + ")");
    }
    if (it->second.status == GuardModelStatus::Retired) {
        return RegistryOpResult::fail("illegal_transition",
                                       "cannot promote retired model");
    }
    // Demote any other Live record to Retired (at-most-one-Live invariant).
    for (auto& [k, rec] : store_) {
        if (rec.model_id == model_id &&
            rec.version != version &&
            rec.status == GuardModelStatus::Live) {
            rec.status = GuardModelStatus::Retired;
        }
    }
    it->second.status = GuardModelStatus::Live;
    it->second.promoted_at_ms = promoted_at_ms;
    return RegistryOpResult::success();
}

RegistryOpResult MemoryGuardModelRegistry::revert(
    const std::string& model_id, const std::string& version) {
    std::unique_lock lock(mu_);
    auto it = store_.find(key(model_id, version));
    if (it == store_.end()) {
        return RegistryOpResult::fail("not_found",
                                       "(" + model_id + ", " + version + ")");
    }
    if (it->second.status != GuardModelStatus::Live) {
        return RegistryOpResult::fail(
            "illegal_transition",
            "only Live -> Retired is allowed; current=" +
                std::string(statusToString(it->second.status)));
    }
    it->second.status = GuardModelStatus::Retired;
    return RegistryOpResult::success();
}

}  // namespace aegisgate::guard
