#include "guardrail/model/sqlite_guard_model_registry.h"

#include "guardrail/model/guard_model_schema.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <string>
#include <utility>

namespace aegisgate::guard {

namespace {

constexpr const char* kInsertSql = R"SQL(
INSERT INTO guard_models
    (model_id, version, path, classifier_threshold, status,
     promoted_at_ms, artifact_sha256, metrics_summary)
VALUES (?, ?, ?, ?, ?, ?, ?, ?);
)SQL";

constexpr const char* kGetSql = R"SQL(
SELECT model_id, version, path, classifier_threshold, status,
       promoted_at_ms, artifact_sha256, metrics_summary
FROM guard_models
WHERE model_id = ? AND version = ?;
)SQL";

constexpr const char* kListSql = R"SQL(
SELECT model_id, version, path, classifier_threshold, status,
       promoted_at_ms, artifact_sha256, metrics_summary
FROM guard_models
WHERE model_id = ?;
)SQL";

constexpr const char* kPromoteSql = R"SQL(
UPDATE guard_models
SET status = 'live', promoted_at_ms = ?
WHERE model_id = ? AND version = ? AND status != 'retired';
)SQL";

constexpr const char* kDemoteOthersSql = R"SQL(
UPDATE guard_models
SET status = 'retired'
WHERE model_id = ? AND version != ? AND status = 'live';
)SQL";

constexpr const char* kRevertSql = R"SQL(
UPDATE guard_models
SET status = 'retired'
WHERE model_id = ? AND version = ? AND status = 'live';
)SQL";

constexpr const char* kStatusFromRowSql = R"SQL(
SELECT status FROM guard_models WHERE model_id = ? AND version = ?;
)SQL";

std::string safeText(sqlite3_stmt* stmt, int col) {
    const auto* raw = sqlite3_column_text(stmt, col);
    if (!raw) return {};
    return reinterpret_cast<const char*>(raw);
}

ModelRegistryRecord rowToRecord(sqlite3_stmt* stmt) {
    ModelRegistryRecord r;
    r.model_id = safeText(stmt, 0);
    r.version = safeText(stmt, 1);
    r.path = safeText(stmt, 2);
    r.classifier_threshold = static_cast<float>(sqlite3_column_double(stmt, 3));
    auto status_str = safeText(stmt, 4);
    if (auto s = statusFromString(status_str)) {
        r.status = *s;
    }
    r.promoted_at_ms = sqlite3_column_int64(stmt, 5);
    r.artifact_sha256 = safeText(stmt, 6);
    r.metrics_summary = safeText(stmt, 7);
    return r;
}

}  // namespace

SQLiteGuardModelRegistry::SQLiteGuardModelRegistry(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        spdlog::error("[guard-registry] sqlite3_open failed: {}",
                       db_ ? sqlite3_errmsg(db_) : "unknown");
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
    }
}

SQLiteGuardModelRegistry::~SQLiteGuardModelRegistry() {
    if (db_ && owns_db_) sqlite3_close(db_);
}

bool SQLiteGuardModelRegistry::initialize() {
    if (!db_) return false;
    std::lock_guard lock(mu_);
    char* err = nullptr;
    if (sqlite3_exec(db_, kGuardModelTableDDL, nullptr, nullptr, &err) != SQLITE_OK) {
        spdlog::error("[guard-registry] CREATE TABLE failed: {}", err ? err : "?");
        sqlite3_free(err);
        return false;
    }
    if (sqlite3_exec(db_, kGuardModelLiveUniqueDDL, nullptr, nullptr, &err) != SQLITE_OK) {
        spdlog::error("[guard-registry] CREATE INDEX failed: {}", err ? err : "?");
        sqlite3_free(err);
        return false;
    }
    return true;
}

RegistryOpResult SQLiteGuardModelRegistry::insert(
    const ModelRegistryRecord& record) {
    if (!db_) return RegistryOpResult::fail("db_unavailable");
    if (record.model_id.empty() || record.version.empty()) {
        return RegistryOpResult::fail("invalid_input",
                                       "model_id and version required");
    }
    std::lock_guard lock(mu_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kInsertSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return RegistryOpResult::fail("prepare_failed", sqlite3_errmsg(db_));
    }
    sqlite3_bind_text(stmt, 1, record.model_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, record.version.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, record.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 4, static_cast<double>(record.classifier_threshold));
    sqlite3_bind_text(stmt, 5, std::string(statusToString(record.status)).c_str(),
                       -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, record.promoted_at_ms);
    sqlite3_bind_text(stmt, 7, record.artifact_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, record.metrics_summary.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_CONSTRAINT) {
        return RegistryOpResult::fail("duplicate_version",
                                       "PRIMARY KEY or unique-live conflict");
    }
    if (rc != SQLITE_DONE) {
        return RegistryOpResult::fail("step_failed", sqlite3_errmsg(db_));
    }
    return RegistryOpResult::success();
}

std::optional<ModelRegistryRecord> SQLiteGuardModelRegistry::get(
    const std::string& model_id, const std::string& version) const {
    if (!db_) return std::nullopt;
    std::lock_guard lock(mu_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kGetSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, model_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, version.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<ModelRegistryRecord> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = rowToRecord(stmt);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<ModelRegistryRecord> SQLiteGuardModelRegistry::list(
    const std::string& model_id) const {
    std::vector<ModelRegistryRecord> out;
    if (!db_) return out;
    std::lock_guard lock(mu_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kListSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_text(stmt, 1, model_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(rowToRecord(stmt));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<ModelRegistryRecord> SQLiteGuardModelRegistry::listByStatus(
    const std::string& model_id, GuardModelStatus status) const {
    auto all = list(model_id);
    std::vector<ModelRegistryRecord> out;
    for (auto& r : all) {
        if (r.status == status) out.push_back(std::move(r));
    }
    return out;
}

RegistryOpResult SQLiteGuardModelRegistry::promote(
    const std::string& model_id, const std::string& version,
    std::int64_t promoted_at_ms) {
    if (!db_) return RegistryOpResult::fail("db_unavailable");
    std::lock_guard lock(mu_);

    // Check current status to enforce illegal_transition before mutating.
    sqlite3_stmt* check = nullptr;
    if (sqlite3_prepare_v2(db_, kStatusFromRowSql, -1, &check, nullptr) != SQLITE_OK) {
        return RegistryOpResult::fail("prepare_failed", sqlite3_errmsg(db_));
    }
    sqlite3_bind_text(check, 1, model_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(check, 2, version.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(check) != SQLITE_ROW) {
        sqlite3_finalize(check);
        return RegistryOpResult::fail("not_found");
    }
    auto current = safeText(check, 0);
    sqlite3_finalize(check);
    if (current == "retired") {
        return RegistryOpResult::fail("illegal_transition",
                                       "cannot promote retired model");
    }

    // Atomically demote previous Live (no-op if current already live).
    sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);

    sqlite3_stmt* demote = nullptr;
    sqlite3_prepare_v2(db_, kDemoteOthersSql, -1, &demote, nullptr);
    sqlite3_bind_text(demote, 1, model_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(demote, 2, version.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(demote) != SQLITE_DONE) {
        sqlite3_finalize(demote);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return RegistryOpResult::fail("demote_failed", sqlite3_errmsg(db_));
    }
    sqlite3_finalize(demote);

    sqlite3_stmt* promote_stmt = nullptr;
    sqlite3_prepare_v2(db_, kPromoteSql, -1, &promote_stmt, nullptr);
    sqlite3_bind_int64(promote_stmt, 1, promoted_at_ms);
    sqlite3_bind_text(promote_stmt, 2, model_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(promote_stmt, 3, version.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(promote_stmt);
    sqlite3_finalize(promote_stmt);

    if (rc != SQLITE_DONE) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return RegistryOpResult::fail("step_failed", sqlite3_errmsg(db_));
    }
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    return RegistryOpResult::success();
}

RegistryOpResult SQLiteGuardModelRegistry::revert(
    const std::string& model_id, const std::string& version) {
    if (!db_) return RegistryOpResult::fail("db_unavailable");
    std::lock_guard lock(mu_);

    sqlite3_stmt* check = nullptr;
    if (sqlite3_prepare_v2(db_, kStatusFromRowSql, -1, &check, nullptr) != SQLITE_OK) {
        return RegistryOpResult::fail("prepare_failed", sqlite3_errmsg(db_));
    }
    sqlite3_bind_text(check, 1, model_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(check, 2, version.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(check) != SQLITE_ROW) {
        sqlite3_finalize(check);
        return RegistryOpResult::fail("not_found");
    }
    auto current = safeText(check, 0);
    sqlite3_finalize(check);
    if (current != "live") {
        return RegistryOpResult::fail("illegal_transition",
                                       "only Live -> Retired is allowed; current=" + current);
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kRevertSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return RegistryOpResult::fail("prepare_failed", sqlite3_errmsg(db_));
    }
    sqlite3_bind_text(stmt, 1, model_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, version.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return RegistryOpResult::fail("step_failed", sqlite3_errmsg(db_));
    }
    return RegistryOpResult::success();
}

}  // namespace aegisgate::guard
