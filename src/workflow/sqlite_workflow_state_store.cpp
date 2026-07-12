#include "workflow/sqlite_workflow_state_store.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <sstream>
#include <utility>

namespace aegisgate::workflow {

namespace {

constexpr const char* kRunsDDL = R"SQL(
CREATE TABLE IF NOT EXISTS workflow_runs (
    run_id              TEXT PRIMARY KEY,
    workflow_id         TEXT NOT NULL,
    dsl_hash            TEXT NOT NULL,
    status              TEXT NOT NULL
                          CHECK (status IN ('pending','running',
                                             'waiting_for_approval',
                                             'succeeded','failed',
                                             'cancelled','dead_letter')),
    created_at_ms       INTEGER NOT NULL,
    updated_at_ms       INTEGER NOT NULL,
    dsl_json            TEXT NOT NULL DEFAULT '',
    context_json        TEXT NOT NULL DEFAULT '{}',
    initiator_user_id   TEXT NOT NULL DEFAULT ''
);
)SQL";

constexpr const char* kNodeRunsDDL = R"SQL(
CREATE TABLE IF NOT EXISTS workflow_node_runs (
    run_id                  TEXT NOT NULL,
    node_id                 TEXT NOT NULL,
    attempt                 INTEGER NOT NULL DEFAULT 1,
    status                  TEXT NOT NULL
                              CHECK (status IN ('pending','running','succeeded',
                                                 'failed','skipped',
                                                 'waiting_for_approval',
                                                 'dead_letter')),
    started_at_ms           INTEGER NOT NULL DEFAULT 0,
    ended_at_ms             INTEGER NOT NULL DEFAULT 0,
    result_json             TEXT NOT NULL DEFAULT '',
    error_message           TEXT NOT NULL DEFAULT '',
    approval_proposal_id    TEXT NOT NULL DEFAULT '',
    -- I30 (TASK-20260703-04)：attempt 纳入主键 → 每次重试独立行（审计链），
    -- 不再互相覆盖。getNodeRun 取最高 attempt。
    PRIMARY KEY (run_id, node_id, attempt),
    FOREIGN KEY (run_id) REFERENCES workflow_runs(run_id) ON DELETE CASCADE
);
)SQL";

std::string safeText(sqlite3_stmt* stmt, int col) {
    const auto* raw = sqlite3_column_text(stmt, col);
    if (!raw) return {};
    return reinterpret_cast<const char*>(raw);
}

bool execSimple(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        spdlog::error("[workflow-store] {} -> {}", sql, err ? err : "?");
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool execSimple(sqlite3* db, const std::string& sql) {
    return execSimple(db, sql.c_str());
}

// Bind helper that pivots based on whether the source string is empty so
// SQLite stores NULL for absent optionals (consistent with PG store).
void bindText(sqlite3_stmt* stmt, int idx, const std::string& v) {
    sqlite3_bind_text(stmt, idx, v.c_str(), -1, SQLITE_TRANSIENT);
}

} // namespace

SQLiteWorkflowStateStore::SQLiteWorkflowStateStore(const std::string& db_path)
    : db_path_(db_path) {
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    int rc = sqlite3_open_v2(db_path_.c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::error("[workflow-store] open {} failed: {}", db_path_,
                       db_ ? sqlite3_errmsg(db_) : "unknown");
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
    }
}

SQLiteWorkflowStateStore::~SQLiteWorkflowStateStore() {
    if (db_) sqlite3_close(db_);
}

bool SQLiteWorkflowStateStore::initialize() {
    if (!db_) return false;
    std::lock_guard<std::mutex> g(mu_);
    if (!execSimple(db_, "PRAGMA journal_mode=WAL;"))   return false;
    if (!execSimple(db_, "PRAGMA synchronous=NORMAL;")) return false;
    if (!execSimple(db_, "PRAGMA foreign_keys=ON;"))    return false;
    if (!execSimple(db_, kRunsDDL))                       return false;
    if (!execSimple(db_, kNodeRunsDDL))                   return false;
    return true;
}

bool SQLiteWorkflowStateStore::createRun(const WorkflowRunRecord& r) {
    if (!db_) return false;
    std::lock_guard<std::mutex> g(mu_);
    constexpr const char* kSql = R"SQL(
        INSERT INTO workflow_runs
            (run_id, workflow_id, dsl_hash, status,
             created_at_ms, updated_at_ms,
             dsl_json, context_json, initiator_user_id)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    bindText(stmt, 1, r.run_id);
    bindText(stmt, 2, r.workflow_id);
    bindText(stmt, 3, r.dsl_hash);
    bindText(stmt, 4, toString(r.status));
    sqlite3_bind_int64(stmt, 5, r.created_at_ms);
    sqlite3_bind_int64(stmt, 6, r.updated_at_ms);
    bindText(stmt, 7, r.dsl_json);
    bindText(stmt, 8, r.context_json.empty() ? "{}" : r.context_json);
    bindText(stmt, 9, r.initiator_user_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<WorkflowRunRecord>
SQLiteWorkflowStateStore::getRun(const std::string& run_id) {
    if (!db_) return std::nullopt;
    std::lock_guard<std::mutex> g(mu_);
    constexpr const char* kSql = R"SQL(
        SELECT run_id, workflow_id, dsl_hash, status,
               created_at_ms, updated_at_ms,
               dsl_json, context_json, initiator_user_id
        FROM workflow_runs WHERE run_id = ?;
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    bindText(stmt, 1, run_id);
    std::optional<WorkflowRunRecord> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        WorkflowRunRecord r;
        r.run_id        = safeText(stmt, 0);
        r.workflow_id   = safeText(stmt, 1);
        r.dsl_hash      = safeText(stmt, 2);
        auto s          = workflowRunStatusFromString(safeText(stmt, 3));
        if (s) r.status = *s;
        r.created_at_ms = sqlite3_column_int64(stmt, 4);
        r.updated_at_ms = sqlite3_column_int64(stmt, 5);
        r.dsl_json      = safeText(stmt, 6);
        r.context_json  = safeText(stmt, 7);
        r.initiator_user_id = safeText(stmt, 8);
        out             = std::move(r);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<WorkflowRunRecord>
SQLiteWorkflowStateStore::listRuns(std::optional<WorkflowRunStatus> filter) {
    std::vector<WorkflowRunRecord> out;
    if (!db_) return out;
    std::lock_guard<std::mutex> g(mu_);
    std::string sql =
        "SELECT run_id, workflow_id, dsl_hash, status, created_at_ms, "
        "updated_at_ms, dsl_json, context_json, initiator_user_id FROM "
        "workflow_runs";
    if (filter) sql += " WHERE status = ?";
    sql += ";";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return out;
    if (filter) bindText(stmt, 1, toString(*filter));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WorkflowRunRecord r;
        r.run_id        = safeText(stmt, 0);
        r.workflow_id   = safeText(stmt, 1);
        r.dsl_hash      = safeText(stmt, 2);
        auto s          = workflowRunStatusFromString(safeText(stmt, 3));
        if (s) r.status = *s;
        r.created_at_ms = sqlite3_column_int64(stmt, 4);
        r.updated_at_ms = sqlite3_column_int64(stmt, 5);
        r.dsl_json      = safeText(stmt, 6);
        r.context_json  = safeText(stmt, 7);
        r.initiator_user_id = safeText(stmt, 8);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return out;
}

bool SQLiteWorkflowStateStore::transitionRunStatus(const std::string& run_id,
                                                    WorkflowRunStatus new_status,
                                                    std::int64_t when_ms) {
    if (!db_) return false;
    std::lock_guard<std::mutex> g(mu_);
    if (!execSimple(db_, "BEGIN IMMEDIATE;")) return false;
    constexpr const char* kSql = R"SQL(
        UPDATE workflow_runs
        SET status = ?, updated_at_ms = ?
        WHERE run_id = ?;
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    int changes = 0;
    if (ok) {
        bindText(stmt, 1, toString(new_status));
        sqlite3_bind_int64(stmt, 2, when_ms);
        bindText(stmt, 3, run_id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        changes = sqlite3_changes(db_);
    }
    if (stmt) sqlite3_finalize(stmt);
    execSimple(db_, ok ? "COMMIT;" : "ROLLBACK;");
    return ok && changes > 0;
}

bool SQLiteWorkflowStateStore::updateRunContext(const std::string& run_id,
                                                 const std::string& new_context_json,
                                                 std::int64_t when_ms) {
    if (!db_) return false;
    std::lock_guard<std::mutex> g(mu_);
    if (!execSimple(db_, "BEGIN IMMEDIATE;")) return false;
    constexpr const char* kSql = R"SQL(
        UPDATE workflow_runs SET context_json = ?, updated_at_ms = ?
        WHERE run_id = ?;
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    int changes = 0;
    if (ok) {
        bindText(stmt, 1, new_context_json);
        sqlite3_bind_int64(stmt, 2, when_ms);
        bindText(stmt, 3, run_id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        changes = sqlite3_changes(db_);
    }
    if (stmt) sqlite3_finalize(stmt);
    execSimple(db_, ok ? "COMMIT;" : "ROLLBACK;");
    return ok && changes > 0;
}

bool SQLiteWorkflowStateStore::upsertNodeRun(const WorkflowNodeRunRecord& n) {
    if (!db_) return false;
    std::lock_guard<std::mutex> g(mu_);
    if (!execSimple(db_, "BEGIN IMMEDIATE;")) return false;
    constexpr const char* kSql = R"SQL(
        INSERT INTO workflow_node_runs
            (run_id, node_id, attempt, status,
             started_at_ms, ended_at_ms,
             result_json, error_message, approval_proposal_id)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(run_id, node_id, attempt) DO UPDATE SET
            status               = excluded.status,
            started_at_ms        = excluded.started_at_ms,
            ended_at_ms          = excluded.ended_at_ms,
            result_json          = excluded.result_json,
            error_message        = excluded.error_message,
            approval_proposal_id = excluded.approval_proposal_id;
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) {
        bindText(stmt, 1, n.run_id);
        bindText(stmt, 2, n.node_id);
        sqlite3_bind_int(stmt, 3, n.attempt);
        bindText(stmt, 4, toString(n.status));
        sqlite3_bind_int64(stmt, 5, n.started_at_ms);
        sqlite3_bind_int64(stmt, 6, n.ended_at_ms);
        bindText(stmt, 7, n.result_json);
        bindText(stmt, 8, n.error_message);
        bindText(stmt, 9, n.approval_proposal_id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    if (stmt) sqlite3_finalize(stmt);
    execSimple(db_, ok ? "COMMIT;" : "ROLLBACK;");
    return ok;
}

std::vector<WorkflowNodeRunRecord>
SQLiteWorkflowStateStore::listNodeRuns(const std::string& run_id) {
    std::vector<WorkflowNodeRunRecord> out;
    if (!db_) return out;
    std::lock_guard<std::mutex> g(mu_);
    constexpr const char* kSql = R"SQL(
        SELECT run_id, node_id, attempt, status, started_at_ms, ended_at_ms,
               result_json, error_message, approval_proposal_id
        FROM workflow_node_runs WHERE run_id = ?
        ORDER BY node_id, attempt;
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    bindText(stmt, 1, run_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WorkflowNodeRunRecord n;
        n.run_id              = safeText(stmt, 0);
        n.node_id             = safeText(stmt, 1);
        n.attempt             = sqlite3_column_int(stmt, 2);
        auto s                = workflowNodeStatusFromString(safeText(stmt, 3));
        if (s) n.status       = *s;
        n.started_at_ms       = sqlite3_column_int64(stmt, 4);
        n.ended_at_ms         = sqlite3_column_int64(stmt, 5);
        n.result_json         = safeText(stmt, 6);
        n.error_message       = safeText(stmt, 7);
        n.approval_proposal_id = safeText(stmt, 8);
        out.push_back(std::move(n));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::optional<WorkflowNodeRunRecord>
SQLiteWorkflowStateStore::getNodeRun(const std::string& run_id,
                                       const std::string& node_id) {
    if (!db_) return std::nullopt;
    std::lock_guard<std::mutex> g(mu_);
    constexpr const char* kSql = R"SQL(
        SELECT run_id, node_id, attempt, status, started_at_ms, ended_at_ms,
               result_json, error_message, approval_proposal_id
        FROM workflow_node_runs WHERE run_id = ? AND node_id = ?
        ORDER BY attempt DESC LIMIT 1;
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    bindText(stmt, 1, run_id);
    bindText(stmt, 2, node_id);
    std::optional<WorkflowNodeRunRecord> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        WorkflowNodeRunRecord n;
        n.run_id              = safeText(stmt, 0);
        n.node_id             = safeText(stmt, 1);
        n.attempt             = sqlite3_column_int(stmt, 2);
        auto s                = workflowNodeStatusFromString(safeText(stmt, 3));
        if (s) n.status       = *s;
        n.started_at_ms       = sqlite3_column_int64(stmt, 4);
        n.ended_at_ms         = sqlite3_column_int64(stmt, 5);
        n.result_json         = safeText(stmt, 6);
        n.error_message       = safeText(stmt, 7);
        n.approval_proposal_id = safeText(stmt, 8);
        out                   = std::move(n);
    }
    sqlite3_finalize(stmt);
    return out;
}

int SQLiteWorkflowStateStore::pruneOldRuns(std::int64_t cutoff_ms) {
    if (!db_) return 0;
    std::lock_guard<std::mutex> g(mu_);
    if (!execSimple(db_, "BEGIN IMMEDIATE;")) return 0;
    constexpr const char* kSql =
        "DELETE FROM workflow_runs WHERE updated_at_ms < ?;";
    sqlite3_stmt* stmt = nullptr;
    int pruned = 0;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, cutoff_ms);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            pruned = sqlite3_changes(db_);
        }
        sqlite3_finalize(stmt);
        execSimple(db_, "COMMIT;");
    } else {
        execSimple(db_, "ROLLBACK;");
    }
    return pruned;
}

bool SQLiteWorkflowStateStore::execRawForTesting(const std::string& sql) {
    if (!db_) return false;
    std::lock_guard<std::mutex> g(mu_);
    return execSimple(db_, sql);
}

} // namespace aegisgate::workflow
