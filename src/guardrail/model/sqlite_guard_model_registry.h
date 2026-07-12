#pragma once

// Phase 11.1 TASK-20260523-01 — Epic 1.3 SQLite IGuardModelRegistry.
//
// Persistent registry backend. Sharing an existing sqlite3* (owned by the
// caller / PersistentStore) is preferred so we live inside the same WAL
// transaction boundary as the rest of the audit / cost ledger. For tests we
// also accept a path-owning constructor that opens a fresh database.

#include "guardrail/model/i_guard_model_registry.h"

#include <memory>
#include <mutex>
#include <string>

struct sqlite3;

namespace aegisgate::guard {

class SQLiteGuardModelRegistry : public IGuardModelRegistry {
public:
    // Owning ctor — opens the file and keeps it for the lifetime of this
    // object. Used by the standalone tests and dev-only setups.
    explicit SQLiteGuardModelRegistry(const std::string& db_path);

    ~SQLiteGuardModelRegistry() override;

    SQLiteGuardModelRegistry(const SQLiteGuardModelRegistry&) = delete;
    SQLiteGuardModelRegistry& operator=(const SQLiteGuardModelRegistry&) = delete;

    bool initialize();  // CREATE TABLE IF NOT EXISTS

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
    mutable std::mutex mu_;
    sqlite3* db_ = nullptr;
    bool owns_db_ = true;
};

}  // namespace aegisgate::guard
