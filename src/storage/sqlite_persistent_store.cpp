#include "storage/sqlite_persistent_store.h"
#include "storage/approval_proposal_schema.h"
#include "storage/json_helpers.h"
#include "storage/rollout_schema.h"
#include <spdlog/spdlog.h>
#include <sys/stat.h>
#include <chrono>
#include <functional>

namespace aegisgate {

namespace {
std::string safeColumnText(sqlite3_stmt* stmt, int col) {
    auto p = sqlite3_column_text(stmt, col);
    return p ? reinterpret_cast<const char*>(p) : "";
}
} // namespace

SQLitePersistentStore::SQLitePersistentStore(const std::string& db_path,
                                               bool wal_mode)
    : db_path_(db_path), wal_mode_(wal_mode) {}

SQLitePersistentStore::~SQLitePersistentStore() { close(); }

bool SQLitePersistentStore::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) return true;

    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    int rc = sqlite3_open_v2(db_path_.c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::error("SQLite open failed: {}", sqlite3_errmsg(db_));
        db_ = nullptr;
        return false;
    }

    if (db_path_ != ":memory:") {
        chmod(db_path_.c_str(), 0600);
    }

    if (wal_mode_ && !enableWAL()) {
        spdlog::warn("Failed to enable WAL mode, continuing with default journal");
    }

    if (!createTables()) {
        closeLocked();
        return false;
    }

    spdlog::info("SQLite persistent store initialized: {}", db_path_);
    return true;
}

void SQLitePersistentStore::closeLocked() {
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

void SQLitePersistentStore::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closeLocked();
}

bool SQLitePersistentStore::isHealthy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return db_ != nullptr;
}

std::string SQLitePersistentStore::backendName() const { return "sqlite"; }

bool SQLitePersistentStore::enableWAL() {
    char* err = nullptr;
    int rc = sqlite3_exec(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;",
                          nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

bool SQLitePersistentStore::createTables() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS audits (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            request_id TEXT NOT NULL,
            timestamp TEXT NOT NULL,
            tenant_id TEXT NOT NULL DEFAULT '',
            action TEXT NOT NULL DEFAULT '',
            stage_name TEXT NOT NULL DEFAULT '',
            detail TEXT NOT NULL DEFAULT '',
            input_hash TEXT NOT NULL DEFAULT '',
            output_hash TEXT NOT NULL DEFAULT '',
            chain_hash TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_audits_tenant ON audits(tenant_id);
        CREATE INDEX IF NOT EXISTS idx_audits_timestamp ON audits(timestamp);
    )";

    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }

    sqlite3_exec(db_, "ALTER TABLE audits ADD COLUMN chain_hash TEXT NOT NULL DEFAULT ''",
                 nullptr, nullptr, nullptr);

    const char* sql2 = R"(
        CREATE TABLE IF NOT EXISTS cost_records (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            request_id TEXT NOT NULL,
            tenant_id TEXT NOT NULL DEFAULT '',
            app_id TEXT NOT NULL DEFAULT '',
            model TEXT NOT NULL DEFAULT '',
            input_tokens INTEGER NOT NULL DEFAULT 0,
            output_tokens INTEGER NOT NULL DEFAULT 0,
            input_cost REAL NOT NULL DEFAULT 0.0,
            output_cost REAL NOT NULL DEFAULT 0.0,
            total_cost REAL NOT NULL DEFAULT 0.0,
            timestamp TEXT NOT NULL DEFAULT '',
            modality TEXT NOT NULL DEFAULT 'chat',
            baseline_cost REAL NOT NULL DEFAULT 0.0,
            routing_decision_reason TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_cost_model ON cost_records(model);
        CREATE INDEX IF NOT EXISTS idx_cost_timestamp ON cost_records(timestamp);

        -- TASK-20260617-02: durable savings events for dashboard reload.
        -- Only numeric / categorical fields persisted (SR2: no raw prompt/response).
        CREATE TABLE IF NOT EXISTS savings_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            type INTEGER NOT NULL DEFAULT 0,
            model TEXT NOT NULL DEFAULT '',
            tenant_id TEXT NOT NULL DEFAULT '',
            tokens_saved INTEGER NOT NULL DEFAULT 0,
            cost_saved REAL NOT NULL DEFAULT 0.0,
            fallback_pricing INTEGER NOT NULL DEFAULT 0,
            timestamp TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_savings_timestamp ON savings_events(timestamp);

        CREATE TABLE IF NOT EXISTS tenants (
            id TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            status TEXT NOT NULL DEFAULT 'active',
            model_whitelist TEXT NOT NULL DEFAULT '[]',
            daily_cost_limit REAL NOT NULL DEFAULT -1,
            monthly_cost_limit REAL NOT NULL DEFAULT -1,
            rate_limit_tokens INTEGER NOT NULL DEFAULT -1,
            rate_limit_refill REAL NOT NULL DEFAULT -1,
            created_at TEXT NOT NULL DEFAULT '',
            updated_at TEXT NOT NULL DEFAULT ''
        );

        CREATE TABLE IF NOT EXISTS users (
            id TEXT PRIMARY KEY,
            tenant_id TEXT NOT NULL,
            username TEXT NOT NULL,
            display_name TEXT NOT NULL DEFAULT '',
            role TEXT NOT NULL DEFAULT 'viewer',
            status TEXT NOT NULL DEFAULT 'active',
            created_at TEXT NOT NULL DEFAULT '',
            updated_at TEXT NOT NULL DEFAULT '',
            FOREIGN KEY (tenant_id) REFERENCES tenants(id)
        );
        CREATE INDEX IF NOT EXISTS idx_users_tenant ON users(tenant_id);
        CREATE UNIQUE INDEX IF NOT EXISTS idx_users_tenant_username ON users(tenant_id, username);

        CREATE TABLE IF NOT EXISTS api_keys (
            id TEXT PRIMARY KEY,
            user_id TEXT NOT NULL,
            tenant_id TEXT NOT NULL,
            name TEXT NOT NULL DEFAULT '',
            key_prefix TEXT NOT NULL,
            key_hash TEXT NOT NULL,
            role TEXT NOT NULL DEFAULT 'developer',
            status TEXT NOT NULL DEFAULT 'active',
            expires_at TEXT,
            last_used_at TEXT,
            created_at TEXT NOT NULL DEFAULT '',
            updated_at TEXT NOT NULL DEFAULT '',
            FOREIGN KEY (user_id) REFERENCES users(id),
            FOREIGN KEY (tenant_id) REFERENCES tenants(id)
        );
        CREATE INDEX IF NOT EXISTS idx_api_keys_prefix ON api_keys(key_prefix);
        CREATE INDEX IF NOT EXISTS idx_api_keys_tenant ON api_keys(tenant_id);
        CREATE INDEX IF NOT EXISTS idx_api_keys_user ON api_keys(user_id);

        CREATE TABLE IF NOT EXISTS sso_providers (
            id TEXT PRIMARY KEY,
            tenant_id TEXT NOT NULL,
            name TEXT NOT NULL DEFAULT '',
            issuer_url TEXT NOT NULL DEFAULT '',
            client_id TEXT NOT NULL DEFAULT '',
            client_secret_enc TEXT NOT NULL DEFAULT '',
            redirect_uri TEXT NOT NULL DEFAULT '',
            scopes_json TEXT NOT NULL DEFAULT '[]',
            claim_mapping_json TEXT NOT NULL DEFAULT '{}',
            group_role_mapping_json TEXT NOT NULL DEFAULT '{}',
            jit_provisioning INTEGER NOT NULL DEFAULT 1,
            default_role TEXT NOT NULL DEFAULT 'viewer',
            enabled INTEGER NOT NULL DEFAULT 1,
            created_at TEXT NOT NULL DEFAULT '',
            updated_at TEXT NOT NULL DEFAULT '',
            FOREIGN KEY (tenant_id) REFERENCES tenants(id)
        );
        CREATE INDEX IF NOT EXISTS idx_sso_providers_tenant ON sso_providers(tenant_id);

        CREATE TABLE IF NOT EXISTS identity_mappings (
            id TEXT PRIMARY KEY,
            tenant_id TEXT NOT NULL,
            external_subject TEXT NOT NULL,
            external_issuer TEXT NOT NULL,
            user_id TEXT NOT NULL,
            email TEXT NOT NULL DEFAULT '',
            last_login_at TEXT NOT NULL DEFAULT '',
            created_at TEXT NOT NULL DEFAULT '',
            FOREIGN KEY (tenant_id) REFERENCES tenants(id),
            FOREIGN KEY (user_id) REFERENCES users(id)
        );
        CREATE UNIQUE INDEX IF NOT EXISTS idx_identity_ext ON identity_mappings(external_subject, external_issuer);
        CREATE INDEX IF NOT EXISTS idx_identity_tenant ON identity_mappings(tenant_id);

        CREATE TABLE IF NOT EXISTS sessions (
            id TEXT PRIMARY KEY,
            user_id TEXT NOT NULL,
            tenant_id TEXT NOT NULL DEFAULT '',
            ip_address TEXT NOT NULL DEFAULT '',
            user_agent TEXT NOT NULL DEFAULT '',
            auth_method TEXT NOT NULL DEFAULT '',
            mfa_verified INTEGER NOT NULL DEFAULT 0,
            created_at TEXT NOT NULL DEFAULT '',
            last_active_at TEXT NOT NULL DEFAULT '',
            expires_at TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);
        CREATE INDEX IF NOT EXISTS idx_sessions_expires ON sessions(expires_at);

        CREATE TABLE IF NOT EXISTS mfa_secrets (
            user_id TEXT PRIMARY KEY,
            secret_enc TEXT NOT NULL DEFAULT '',
            enabled INTEGER NOT NULL DEFAULT 0,
            recovery_codes_enc TEXT NOT NULL DEFAULT '[]',
            created_at TEXT NOT NULL DEFAULT ''
        );

        CREATE TABLE IF NOT EXISTS mfa_failures (
            user_id TEXT PRIMARY KEY,
            fail_count INTEGER NOT NULL DEFAULT 0,
            first_fail_at INTEGER NOT NULL DEFAULT 0,
            locked_until INTEGER NOT NULL DEFAULT 0
        );

        CREATE TABLE IF NOT EXISTS scim_tokens (
            id TEXT PRIMARY KEY,
            tenant_id TEXT NOT NULL,
            token_hash TEXT NOT NULL,
            description TEXT NOT NULL DEFAULT '',
            created_at TEXT NOT NULL DEFAULT '',
            expires_at TEXT NOT NULL DEFAULT '',
            FOREIGN KEY (tenant_id) REFERENCES tenants(id)
        );
        CREATE INDEX IF NOT EXISTS idx_scim_tokens_hash ON scim_tokens(token_hash);
        CREATE INDEX IF NOT EXISTS idx_scim_tokens_tenant ON scim_tokens(tenant_id);

        CREATE TABLE IF NOT EXISTS scim_groups (
            id TEXT PRIMARY KEY,
            tenant_id TEXT NOT NULL,
            display_name TEXT NOT NULL,
            created_at TEXT DEFAULT (datetime('now')),
            updated_at TEXT DEFAULT (datetime('now'))
        );
        CREATE INDEX IF NOT EXISTS idx_scim_groups_tenant ON scim_groups(tenant_id);

        CREATE TABLE IF NOT EXISTS scim_group_members (
            group_id TEXT NOT NULL,
            user_id TEXT NOT NULL,
            PRIMARY KEY (group_id, user_id),
            FOREIGN KEY (group_id) REFERENCES scim_groups(id) ON DELETE CASCADE
        );

        CREATE TABLE IF NOT EXISTS prompt_templates (
            id TEXT PRIMARY KEY,
            tenant_id TEXT NOT NULL,
            name TEXT NOT NULL,
            content TEXT NOT NULL,
            version INTEGER NOT NULL DEFAULT 1,
            weight INTEGER NOT NULL DEFAULT 100,
            is_active INTEGER NOT NULL DEFAULT 1,
            created_at TEXT NOT NULL DEFAULT '',
            updated_at TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_prompt_templates_tenant ON prompt_templates(tenant_id);
        CREATE INDEX IF NOT EXISTS idx_prompt_templates_name ON prompt_templates(tenant_id, name);

        CREATE TABLE IF NOT EXISTS rule_sets (
            tenant_id TEXT NOT NULL,
            version INTEGER NOT NULL,
            rules_json TEXT NOT NULL,
            created_at TEXT NOT NULL DEFAULT '',
            is_active INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (tenant_id, version)
        );
        CREATE INDEX IF NOT EXISTS idx_rule_sets_active ON rule_sets(tenant_id, is_active);

        -- Phase 9.3 control plane: versioned aegisgate.yaml bundles.
        CREATE TABLE IF NOT EXISTS config_versions (
            version_id         TEXT PRIMARY KEY,
            content_sha256     TEXT NOT NULL DEFAULT '',
            yaml_content       BLOB NOT NULL,
            size_bytes         INTEGER NOT NULL DEFAULT 0,
            status             TEXT NOT NULL DEFAULT 'PENDING',
            submitter          TEXT NOT NULL DEFAULT '',
            submitter_comment  TEXT NOT NULL DEFAULT '',
            submitted_at       INTEGER NOT NULL DEFAULT 0,
            reviewer           TEXT NOT NULL DEFAULT '',
            reviewer_comment   TEXT NOT NULL DEFAULT '',
            reviewed_at        INTEGER NOT NULL DEFAULT 0,
            activator          TEXT NOT NULL DEFAULT '',
            activated_at       INTEGER NOT NULL DEFAULT 0,
            deactivated_at     INTEGER NOT NULL DEFAULT 0,
            chain_hash         TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_config_versions_submitted
            ON config_versions(submitted_at DESC);
        CREATE INDEX IF NOT EXISTS idx_config_versions_status
            ON config_versions(status);
        -- SR8 defense-in-depth: enforce at-most-one ACTIVE bundle at schema level.
        CREATE UNIQUE INDEX IF NOT EXISTS uq_config_versions_active
            ON config_versions(status) WHERE status = 'ACTIVE';
    )";

    err = nullptr;
    rc = sqlite3_exec(db_, sql2, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        spdlog::error("SQLite create tables failed: {}", err ? err : "unknown");
        if (err) sqlite3_free(err);
        return false;
    }

    // P1-6: idempotent migration for pre-existing cost_records tables created
    // before modality/baseline_cost/routing_decision_reason existed. ALTER TABLE
    // ADD COLUMN errors if the column already exists; we ignore that (same
    // pattern as the audits.chain_hash migration above). New DBs already have
    // these columns from the CREATE TABLE above, so this is a no-op there.
    sqlite3_exec(db_,
        "ALTER TABLE cost_records ADD COLUMN modality TEXT NOT NULL DEFAULT 'chat'",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db_,
        "ALTER TABLE cost_records ADD COLUMN baseline_cost REAL NOT NULL DEFAULT 0.0",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db_,
        "ALTER TABLE cost_records ADD COLUMN routing_decision_reason TEXT NOT NULL DEFAULT ''",
        nullptr, nullptr, nullptr);

    // Phase 9.3.4 Rollout schema (TASK-20260422-01). Kept as its own
    // executeStatement so a failure here does not silently mask failures
    // in the older sql/sql2 blocks above.
    err = nullptr;
    rc = sqlite3_exec(db_, kRolloutsSchemaSql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        spdlog::error("SQLite create rollout tables failed: {}",
                      err ? err : "unknown");
        if (err) sqlite3_free(err);
        return false;
    }

    // Phase 11.5 Autonomy approval proposals (TASK-20260518-02 Epic 1.0).
    err = nullptr;
    rc = sqlite3_exec(db_, kApprovalProposalsSchemaSql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        spdlog::error("SQLite create autonomy_proposals table failed: {}",
                      err ? err : "unknown");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

bool SQLitePersistentStore::insertAudit(const AuditEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql =
        "INSERT INTO audits (request_id, timestamp, tenant_id, action, "
        "stage_name, detail, input_hash, output_hash, chain_hash) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, entry.request_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, entry.timestamp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, entry.tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, entry.action.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, entry.stage_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, entry.detail.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, entry.input_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, entry.output_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, entry.chain_hash.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::vector<AuditEntry> SQLitePersistentStore::queryAudits(
    const std::string& tenant_id, int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AuditEntry> result;
    if (!db_) return result;

    std::string sql =
        "SELECT request_id, timestamp, tenant_id, action, stage_name, "
        "detail, input_hash, output_hash, chain_hash FROM audits";
    if (!tenant_id.empty()) sql += " WHERE tenant_id = ?";
    sql += " ORDER BY id DESC LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    int idx = 1;
    if (!tenant_id.empty())
        sqlite3_bind_text(stmt, idx++, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx, offset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AuditEntry e;
        e.request_id  = safeColumnText(stmt, 0);
        e.timestamp   = safeColumnText(stmt, 1);
        e.tenant_id   = safeColumnText(stmt, 2);
        e.action      = safeColumnText(stmt, 3);
        e.stage_name  = safeColumnText(stmt, 4);
        e.detail      = safeColumnText(stmt, 5);
        e.input_hash  = safeColumnText(stmt, 6);
        e.output_hash = safeColumnText(stmt, 7);
        e.chain_hash  = safeColumnText(stmt, 8);
        result.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    return result;
}

int64_t SQLitePersistentStore::auditCount(const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;

    std::string sql = "SELECT COUNT(*) FROM audits";
    if (!tenant_id.empty()) sql += " WHERE tenant_id = ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return 0;

    if (!tenant_id.empty())
        sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);

    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

bool SQLitePersistentStore::insertCostRecord(const CostRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql =
        "INSERT INTO cost_records (request_id, tenant_id, app_id, model, "
        "input_tokens, output_tokens, input_cost, output_cost, total_cost, "
        "timestamp, modality, baseline_cost, routing_decision_reason) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, record.request_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, record.tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, record.app_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, record.model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, record.input_tokens);
    sqlite3_bind_int(stmt, 6, record.output_tokens);
    sqlite3_bind_double(stmt, 7, record.input_cost);
    sqlite3_bind_double(stmt, 8, record.output_cost);
    sqlite3_bind_double(stmt, 9, record.total_cost);
    sqlite3_bind_text(stmt, 10, record.timestamp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, record.modality.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 12, record.baseline_cost);
    sqlite3_bind_text(stmt, 13, record.routing_decision_reason.c_str(), -1,
                      SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::vector<CostRecord> SQLitePersistentStore::queryCosts(
    const std::string& model, int limit, int offset, const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CostRecord> result;
    if (!db_) return result;

    std::string sql =
        "SELECT request_id, tenant_id, app_id, model, input_tokens, "
        "output_tokens, input_cost, output_cost, total_cost, timestamp, "
        "modality, baseline_cost, routing_decision_reason "
        "FROM cost_records";
    // TASK-20260604-01 P0-E/D1=A：model + tenant 联合过滤（tenant 下沉 DB / 折叠 P1-11）。
    std::string where;
    if (!model.empty()) where += (where.empty() ? " WHERE " : " AND ") + std::string("model = ?");
    if (!tenant_id.empty()) where += (where.empty() ? " WHERE " : " AND ") + std::string("tenant_id = ?");
    // C11 ②：newest-first（DESC），对齐 PG queryCosts + audits（此前误用 ASC）。
    sql += where + " ORDER BY id DESC LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    int idx = 1;
    if (!model.empty())
        sqlite3_bind_text(stmt, idx++, model.c_str(), -1, SQLITE_TRANSIENT);
    if (!tenant_id.empty())
        sqlite3_bind_text(stmt, idx++, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx, offset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CostRecord r;
        r.request_id    = safeColumnText(stmt, 0);
        r.tenant_id     = safeColumnText(stmt, 1);
        r.app_id        = safeColumnText(stmt, 2);
        r.model         = safeColumnText(stmt, 3);
        r.input_tokens  = sqlite3_column_int(stmt, 4);
        r.output_tokens = sqlite3_column_int(stmt, 5);
        r.input_cost    = sqlite3_column_double(stmt, 6);
        r.output_cost   = sqlite3_column_double(stmt, 7);
        r.total_cost    = sqlite3_column_double(stmt, 8);
        r.timestamp     = safeColumnText(stmt, 9);
        r.modality      = safeColumnText(stmt, 10);
        r.baseline_cost = sqlite3_column_double(stmt, 11);
        r.routing_decision_reason = safeColumnText(stmt, 12);
        result.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return result;
}

int64_t SQLitePersistentStore::costRecordCount(const std::string& model,
                                               const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;

    std::string sql = "SELECT COUNT(*) FROM cost_records";
    std::string where;
    if (!model.empty()) where += (where.empty() ? " WHERE " : " AND ") + std::string("model = ?");
    if (!tenant_id.empty()) where += (where.empty() ? " WHERE " : " AND ") + std::string("tenant_id = ?");
    sql += where;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return 0;

    int idx = 1;
    if (!model.empty())
        sqlite3_bind_text(stmt, idx++, model.c_str(), -1, SQLITE_TRANSIENT);
    if (!tenant_id.empty())
        sqlite3_bind_text(stmt, idx++, tenant_id.c_str(), -1, SQLITE_TRANSIENT);

    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

// TASK-20260702-01 P1-2 — total_cost DB 级 SUM（无 10k 截断）。tenant 空 = 全局。
double SQLitePersistentStore::costTotal(const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0.0;

    std::string sql = "SELECT COALESCE(SUM(total_cost), 0) FROM cost_records";
    if (!tenant_id.empty()) sql += " WHERE tenant_id = ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return 0.0;
    if (!tenant_id.empty())
        sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);

    double total = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        total = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    return total;
}

// TASK-20260604-01 P0-E/D1=A — 分页总数。tenant 为空 = 全局（SuperAdmin 视角，SR-3）。
namespace {
int64_t sqliteCountWhereTenant(sqlite3* db, std::mutex& mtx, const char* table,
                               const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mtx);
    if (!db) return 0;
    std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
    if (!tenant_id.empty()) sql += " WHERE tenant_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return 0;
    if (!tenant_id.empty())
        sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}
} // namespace

int64_t SQLitePersistentStore::tenantCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM tenants", -1, &stmt, nullptr) != SQLITE_OK)
        return 0;
    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

int64_t SQLitePersistentStore::userCount(const std::string& tenant_id) {
    return sqliteCountWhereTenant(db_, mutex_, "users", tenant_id);
}

int64_t SQLitePersistentStore::apiKeyCount(const std::string& tenant_id) {
    return sqliteCountWhereTenant(db_, mutex_, "api_keys", tenant_id);
}

int64_t SQLitePersistentStore::promptTemplateCount(const std::string& tenant_id) {
    return sqliteCountWhereTenant(db_, mutex_, "prompt_templates", tenant_id);
}

int64_t SQLitePersistentStore::ruleSetCount(const std::string& tenant_id) {
    return sqliteCountWhereTenant(db_, mutex_, "rule_sets", tenant_id);
}

int64_t SQLitePersistentStore::pruneAudits(int retention_days) {
    if (retention_days <= 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;

    // C11 ①：timestamp 写入为 ISO8601（"...THH:MM:SSZ"）；比较基准须同格式，
    // 否则 datetime('now',...)（空格无 T/Z）在同日边界字符串错位 → 超期留存。
    const char* sql = "DELETE FROM audits WHERE timestamp < "
                      "strftime('%Y-%m-%dT%H:%M:%SZ', 'now', '-' || ? || ' days')";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;

    std::string days_str = std::to_string(retention_days);
    sqlite3_bind_text(stmt, 1, days_str.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    int64_t deleted = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return deleted;
}

int64_t SQLitePersistentStore::pruneCostRecords(int retention_days) {
    if (retention_days <= 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;

    // C11 ①：见 pruneAudits，ISO8601 比较基准统一。
    const char* sql = "DELETE FROM cost_records WHERE timestamp < "
                      "strftime('%Y-%m-%dT%H:%M:%SZ', 'now', '-' || ? || ' days')";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;

    std::string days_str = std::to_string(retention_days);
    sqlite3_bind_text(stmt, 1, days_str.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    int64_t deleted = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return deleted;
}

// --- Savings events (dashboard persistence, TASK-20260617-02) ---

bool SQLitePersistentStore::insertSavingsEvent(const SavingsEventRecord& ev) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql =
        "INSERT INTO savings_events (type, model, tenant_id, tokens_saved, "
        "cost_saved, fallback_pricing, timestamp) VALUES (?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, ev.type);
    sqlite3_bind_text(stmt, 2, ev.model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, ev.tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, ev.tokens_saved);
    sqlite3_bind_double(stmt, 5, ev.cost_saved);
    sqlite3_bind_int(stmt, 6, ev.fallback_pricing ? 1 : 0);
    sqlite3_bind_text(stmt, 7, ev.timestamp.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::vector<PersistentStore::SavingsEventRecord>
SQLitePersistentStore::querySavingsEventsByDateRange(
    const std::string& from_iso, const std::string& to_iso, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SavingsEventRecord> result;
    if (!db_) return result;
    // Empty bound = open on that side (mirrors PersistentStore contract).
    const char* sql =
        "SELECT type, model, tenant_id, tokens_saved, cost_saved, "
        "fallback_pricing, timestamp FROM savings_events "
        "WHERE (? = '' OR timestamp >= ?) AND (? = '' OR timestamp <= ?) "
        "ORDER BY timestamp ASC LIMIT ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_text(stmt, 1, from_iso.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, from_iso.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, to_iso.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, to_iso.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, limit < 0 ? -1 : limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SavingsEventRecord ev;
        ev.type = sqlite3_column_int(stmt, 0);
        ev.model = safeColumnText(stmt, 1);
        ev.tenant_id = safeColumnText(stmt, 2);
        ev.tokens_saved = sqlite3_column_int(stmt, 3);
        ev.cost_saved = sqlite3_column_double(stmt, 4);
        ev.fallback_pricing = sqlite3_column_int(stmt, 5) != 0;
        ev.timestamp = safeColumnText(stmt, 6);
        result.push_back(std::move(ev));
    }
    sqlite3_finalize(stmt);
    return result;
}

int64_t SQLitePersistentStore::savingsEventCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;
    const char* sql = "SELECT COUNT(*) FROM savings_events";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

int64_t SQLitePersistentStore::pruneSavingsEvents(int retention_days) {
    if (retention_days <= 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;
    const char* sql =
        "DELETE FROM savings_events WHERE timestamp < "  // C11 ①：ISO8601 比较基准统一
        "strftime('%Y-%m-%dT%H:%M:%SZ', 'now', '-' || ? || ' days')";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    std::string days_str = std::to_string(retention_days);
    sqlite3_bind_text(stmt, 1, days_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int64_t deleted = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return deleted;
}

// --- RBAC: Tenant ---

bool SQLitePersistentStore::insertTenant(const Tenant& tenant) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql =
        "INSERT INTO tenants (id, name, status, model_whitelist, daily_cost_limit, "
        "monthly_cost_limit, rate_limit_tokens, rate_limit_refill, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    std::string wl = serializeStringList(tenant.model_whitelist);

    sqlite3_bind_text(stmt, 1, tenant.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, tenant.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, tenant.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, wl.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 5, tenant.daily_cost_limit);
    sqlite3_bind_double(stmt, 6, tenant.monthly_cost_limit);
    sqlite3_bind_int(stmt, 7, tenant.rate_limit_tokens);
    sqlite3_bind_double(stmt, 8, tenant.rate_limit_refill);
    sqlite3_bind_text(stmt, 9, tenant.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, tenant.updated_at.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

namespace {

Tenant tenantFromStmt(sqlite3_stmt* stmt) {
    Tenant t;
    t.id                 = safeColumnText(stmt, 0);
    t.name               = safeColumnText(stmt, 1);
    t.status             = safeColumnText(stmt, 2);
    t.model_whitelist    = parseStringList(safeColumnText(stmt, 3));
    t.daily_cost_limit   = sqlite3_column_double(stmt, 4);
    t.monthly_cost_limit = sqlite3_column_double(stmt, 5);
    t.rate_limit_tokens  = sqlite3_column_int(stmt, 6);
    t.rate_limit_refill  = sqlite3_column_double(stmt, 7);
    t.created_at         = safeColumnText(stmt, 8);
    t.updated_at         = safeColumnText(stmt, 9);
    return t;
}

User userFromStmt(sqlite3_stmt* stmt) {
    User u;
    u.id           = safeColumnText(stmt, 0);
    u.tenant_id    = safeColumnText(stmt, 1);
    u.username     = safeColumnText(stmt, 2);
    u.display_name = safeColumnText(stmt, 3);
    auto role_opt  = roleFromString(safeColumnText(stmt, 4));
    u.role         = role_opt.value_or(Role::Viewer);
    u.status       = safeColumnText(stmt, 5);
    u.created_at   = safeColumnText(stmt, 6);
    u.updated_at   = safeColumnText(stmt, 7);
    return u;
}

ApiKeyRecord apiKeyFromStmt(sqlite3_stmt* stmt) {
    ApiKeyRecord k;
    k.id           = safeColumnText(stmt, 0);
    k.user_id      = safeColumnText(stmt, 1);
    k.tenant_id    = safeColumnText(stmt, 2);
    k.name         = safeColumnText(stmt, 3);
    k.key_prefix   = safeColumnText(stmt, 4);
    k.key_hash     = safeColumnText(stmt, 5);
    auto role_opt  = roleFromString(safeColumnText(stmt, 6));
    k.role         = role_opt.value_or(Role::Developer);
    k.status       = safeColumnText(stmt, 7);
    k.expires_at   = safeColumnText(stmt, 8);
    k.last_used_at = safeColumnText(stmt, 9);
    k.created_at   = safeColumnText(stmt, 10);
    k.updated_at   = safeColumnText(stmt, 11);
    return k;
}
} // namespace

std::optional<Tenant> SQLitePersistentStore::getTenant(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    const char* sql = "SELECT id, name, status, model_whitelist, daily_cost_limit, "
        "monthly_cost_limit, rate_limit_tokens, rate_limit_refill, created_at, updated_at "
        "FROM tenants WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<Tenant> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) result = tenantFromStmt(stmt);
    sqlite3_finalize(stmt);
    return result;
}

std::vector<Tenant> SQLitePersistentStore::listTenants(int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Tenant> result;
    if (!db_) return result;
    const char* sql = "SELECT id, name, status, model_whitelist, daily_cost_limit, "
        "monthly_cost_limit, rate_limit_tokens, rate_limit_refill, created_at, updated_at "
        "FROM tenants ORDER BY created_at ASC LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_int(stmt, 1, limit);
    sqlite3_bind_int(stmt, 2, offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) result.push_back(tenantFromStmt(stmt));
    sqlite3_finalize(stmt);
    return result;
}

bool SQLitePersistentStore::updateTenant(const Tenant& tenant) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql =
        "UPDATE tenants SET name=?, status=?, model_whitelist=?, daily_cost_limit=?, "
        "monthly_cost_limit=?, rate_limit_tokens=?, rate_limit_refill=?, updated_at=? WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    std::string wl = serializeStringList(tenant.model_whitelist);
    sqlite3_bind_text(stmt, 1, tenant.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, tenant.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, wl.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 4, tenant.daily_cost_limit);
    sqlite3_bind_double(stmt, 5, tenant.monthly_cost_limit);
    sqlite3_bind_int(stmt, 6, tenant.rate_limit_tokens);
    sqlite3_bind_double(stmt, 7, tenant.rate_limit_refill);
    sqlite3_bind_text(stmt, 8, tenant.updated_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, tenant.id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db_) > 0;
}

bool SQLitePersistentStore::deleteTenant(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "DELETE FROM tenants WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

// --- RBAC: User ---

bool SQLitePersistentStore::insertUser(const User& user) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql =
        "INSERT INTO users (id, tenant_id, username, display_name, role, status, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, user.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, user.tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, user.username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, user.display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, roleToString(user.role), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, user.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, user.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, user.updated_at.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<User> SQLitePersistentStore::getUser(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    const char* sql = "SELECT id, tenant_id, username, display_name, role, status, "
        "created_at, updated_at FROM users WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<User> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) result = userFromStmt(stmt);
    sqlite3_finalize(stmt);
    return result;
}

std::optional<User> SQLitePersistentStore::getUserByUsername(
    const std::string& tenant_id, const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    const char* sql = "SELECT id, tenant_id, username, display_name, role, status, "
        "created_at, updated_at FROM users WHERE tenant_id = ? AND username = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<User> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) result = userFromStmt(stmt);
    sqlite3_finalize(stmt);
    return result;
}

std::vector<User> SQLitePersistentStore::listUsers(
    const std::string& tenant_id, int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<User> result;
    if (!db_) return result;
    std::string sql = "SELECT id, tenant_id, username, display_name, role, status, "
        "created_at, updated_at FROM users";
    if (!tenant_id.empty()) sql += " WHERE tenant_id = ?";
    sql += " ORDER BY created_at ASC LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return result;
    int idx = 1;
    if (!tenant_id.empty()) sqlite3_bind_text(stmt, idx++, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx, offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) result.push_back(userFromStmt(stmt));
    sqlite3_finalize(stmt);
    return result;
}

bool SQLitePersistentStore::updateUser(const User& user) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "UPDATE users SET username=?, display_name=?, role=?, status=?, updated_at=? WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, user.username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, user.display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, roleToString(user.role), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, user.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, user.updated_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, user.id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db_) > 0;
}

bool SQLitePersistentStore::deleteUser(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "DELETE FROM users WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

// --- RBAC: API Key ---

bool SQLitePersistentStore::insertApiKey(const ApiKeyRecord& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql =
        "INSERT INTO api_keys (id, user_id, tenant_id, name, key_prefix, key_hash, "
        "role, status, expires_at, last_used_at, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, key.user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, key.tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, key.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, key.key_prefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, key.key_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, roleToString(key.role), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, key.status.c_str(), -1, SQLITE_TRANSIENT);
    if (key.expires_at.empty()) sqlite3_bind_null(stmt, 9);
    else sqlite3_bind_text(stmt, 9, key.expires_at.c_str(), -1, SQLITE_TRANSIENT);
    if (key.last_used_at.empty()) sqlite3_bind_null(stmt, 10);
    else sqlite3_bind_text(stmt, 10, key.last_used_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, key.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, key.updated_at.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<ApiKeyRecord> SQLitePersistentStore::getApiKeyByHash(const std::string& key_hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    const char* sql = "SELECT id, user_id, tenant_id, name, key_prefix, key_hash, "
        "role, status, expires_at, last_used_at, created_at, updated_at "
        "FROM api_keys WHERE key_hash = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, key_hash.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<ApiKeyRecord> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) result = apiKeyFromStmt(stmt);
    sqlite3_finalize(stmt);
    return result;
}

std::vector<ApiKeyRecord> SQLitePersistentStore::getApiKeysByPrefix(const std::string& prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ApiKeyRecord> result;
    if (!db_) return result;
    const char* sql = "SELECT id, user_id, tenant_id, name, key_prefix, key_hash, "
        "role, status, expires_at, last_used_at, created_at, updated_at "
        "FROM api_keys WHERE key_prefix = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_text(stmt, 1, prefix.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) result.push_back(apiKeyFromStmt(stmt));
    sqlite3_finalize(stmt);
    return result;
}

std::vector<ApiKeyRecord> SQLitePersistentStore::listApiKeys(
    const std::string& tenant_id, int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ApiKeyRecord> result;
    if (!db_) return result;
    std::string sql = "SELECT id, user_id, tenant_id, name, key_prefix, key_hash, "
        "role, status, expires_at, last_used_at, created_at, updated_at FROM api_keys";
    if (!tenant_id.empty()) sql += " WHERE tenant_id = ?";
    sql += " ORDER BY created_at ASC LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return result;
    int idx = 1;
    if (!tenant_id.empty()) sqlite3_bind_text(stmt, idx++, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx, offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) result.push_back(apiKeyFromStmt(stmt));
    sqlite3_finalize(stmt);
    return result;
}

bool SQLitePersistentStore::updateApiKey(const ApiKeyRecord& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "UPDATE api_keys SET name=?, role=?, status=?, expires_at=?, "
        "last_used_at=?, updated_at=? WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, roleToString(key.role), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, key.status.c_str(), -1, SQLITE_TRANSIENT);
    if (key.expires_at.empty()) sqlite3_bind_null(stmt, 4);
    else sqlite3_bind_text(stmt, 4, key.expires_at.c_str(), -1, SQLITE_TRANSIENT);
    if (key.last_used_at.empty()) sqlite3_bind_null(stmt, 5);
    else sqlite3_bind_text(stmt, 5, key.last_used_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, key.updated_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, key.id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db_) > 0;
}

bool SQLitePersistentStore::revokeApiKey(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "UPDATE api_keys SET status='revoked' WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db_) > 0;
}

// --- Tenant cost aggregation ---

double SQLitePersistentStore::getTenantCostInPeriod(
    const std::string& tenant_id, const std::string& start, const std::string& end) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0.0;
    const char* sql = "SELECT COALESCE(SUM(total_cost), 0) FROM cost_records "
        "WHERE tenant_id = ? AND timestamp >= ? AND timestamp <= ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0.0;
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, start.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, end.c_str(), -1, SQLITE_TRANSIENT);
    double total = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    return total;
}

std::vector<CostRecord> SQLitePersistentStore::queryCostsByDateRange(
    const std::string& tenant_id, const std::string& from,
    const std::string& to) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CostRecord> result;
    if (!db_) return result;
    const char* sql =
        "SELECT request_id, tenant_id, app_id, model, input_tokens, output_tokens, "
        "input_cost, output_cost, total_cost, timestamp, modality, baseline_cost, "
        "routing_decision_reason FROM cost_records "
        "WHERE timestamp >= ? AND timestamp <= ? "
        "AND (? = '' OR tenant_id = ?) "
        "ORDER BY timestamp ASC LIMIT 100000";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_text(stmt, 1, from.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, to.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CostRecord rec;
        rec.request_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        rec.tenant_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        rec.app_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        rec.model = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        rec.input_tokens = sqlite3_column_int(stmt, 4);
        rec.output_tokens = sqlite3_column_int(stmt, 5);
        rec.input_cost = sqlite3_column_double(stmt, 6);
        rec.output_cost = sqlite3_column_double(stmt, 7);
        rec.total_cost = sqlite3_column_double(stmt, 8);
        rec.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        rec.modality = safeColumnText(stmt, 10);
        rec.baseline_cost = sqlite3_column_double(stmt, 11);
        rec.routing_decision_reason = safeColumnText(stmt, 12);
        result.push_back(std::move(rec));
    }
    sqlite3_finalize(stmt);
    return result;
}

// --- SSO Provider ---

bool SQLitePersistentStore::insertSsoProvider(const SsoProvider& p) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql =
        "INSERT INTO sso_providers (id, tenant_id, name, issuer_url, client_id, "
        "client_secret_enc, redirect_uri, scopes_json, claim_mapping_json, "
        "group_role_mapping_json, jit_provisioning, default_role, enabled, "
        "created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    std::string scopes_json = serializeStringList(p.scopes);
    sqlite3_bind_text(stmt, 1, p.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, p.tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, p.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, p.issuer_url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, p.client_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, p.client_secret_enc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, p.redirect_uri.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, scopes_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, p.claim_mapping_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, p.group_role_mapping_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 11, p.jit_provisioning ? 1 : 0);
    sqlite3_bind_text(stmt, 12, p.default_role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 13, p.enabled ? 1 : 0);
    sqlite3_bind_text(stmt, 14, p.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 15, p.updated_at.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

namespace {
SsoProvider ssoProviderFromStmt(sqlite3_stmt* stmt) {
    SsoProvider p;
    p.id                     = safeColumnText(stmt, 0);
    p.tenant_id              = safeColumnText(stmt, 1);
    p.name                   = safeColumnText(stmt, 2);
    p.issuer_url             = safeColumnText(stmt, 3);
    p.client_id              = safeColumnText(stmt, 4);
    p.client_secret_enc      = safeColumnText(stmt, 5);
    p.redirect_uri           = safeColumnText(stmt, 6);
    p.scopes                 = parseStringList(safeColumnText(stmt, 7));
    p.claim_mapping_json     = safeColumnText(stmt, 8);
    p.group_role_mapping_json = safeColumnText(stmt, 9);
    p.jit_provisioning       = sqlite3_column_int(stmt, 10) != 0;
    p.default_role           = safeColumnText(stmt, 11);
    p.enabled                = sqlite3_column_int(stmt, 12) != 0;
    p.created_at             = safeColumnText(stmt, 13);
    p.updated_at             = safeColumnText(stmt, 14);
    return p;
}

IdentityMapping identityMappingFromStmt(sqlite3_stmt* stmt) {
    IdentityMapping m;
    m.id               = safeColumnText(stmt, 0);
    m.tenant_id        = safeColumnText(stmt, 1);
    m.external_subject = safeColumnText(stmt, 2);
    m.external_issuer  = safeColumnText(stmt, 3);
    m.user_id          = safeColumnText(stmt, 4);
    m.email            = safeColumnText(stmt, 5);
    m.last_login_at    = safeColumnText(stmt, 6);
    m.created_at       = safeColumnText(stmt, 7);
    return m;
}

Session sessionFromStmt(sqlite3_stmt* stmt) {
    Session s;
    s.id             = safeColumnText(stmt, 0);
    s.user_id        = safeColumnText(stmt, 1);
    s.tenant_id      = safeColumnText(stmt, 2);
    s.ip_address     = safeColumnText(stmt, 3);
    s.user_agent     = safeColumnText(stmt, 4);
    s.auth_method    = safeColumnText(stmt, 5);
    s.mfa_verified   = sqlite3_column_int(stmt, 6) != 0;
    s.created_at     = safeColumnText(stmt, 7);
    s.last_active_at = safeColumnText(stmt, 8);
    s.expires_at     = safeColumnText(stmt, 9);
    return s;
}

MfaSecret mfaSecretFromStmt(sqlite3_stmt* stmt) {
    MfaSecret m;
    m.user_id             = safeColumnText(stmt, 0);
    m.secret_enc          = safeColumnText(stmt, 1);
    m.enabled             = sqlite3_column_int(stmt, 2) != 0;
    m.recovery_codes_hash = parseStringList(safeColumnText(stmt, 3));
    m.created_at          = safeColumnText(stmt, 4);
    return m;
}

ScimToken scimTokenFromStmt(sqlite3_stmt* stmt) {
    ScimToken t;
    t.id          = safeColumnText(stmt, 0);
    t.tenant_id   = safeColumnText(stmt, 1);
    t.token_hash  = safeColumnText(stmt, 2);
    t.description = safeColumnText(stmt, 3);
    t.created_at  = safeColumnText(stmt, 4);
    t.expires_at  = safeColumnText(stmt, 5);
    return t;
}
} // namespace

std::optional<SsoProvider> SQLitePersistentStore::getSsoProvider(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    const char* sql =
        "SELECT id, tenant_id, name, issuer_url, client_id, client_secret_enc, "
        "redirect_uri, scopes_json, claim_mapping_json, group_role_mapping_json, "
        "jit_provisioning, default_role, enabled, created_at, updated_at "
        "FROM sso_providers WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<SsoProvider> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) result = ssoProviderFromStmt(stmt);
    sqlite3_finalize(stmt);
    return result;
}

std::optional<SsoProvider> SQLitePersistentStore::getSsoProviderByTenant(const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    const char* sql =
        "SELECT id, tenant_id, name, issuer_url, client_id, client_secret_enc, "
        "redirect_uri, scopes_json, claim_mapping_json, group_role_mapping_json, "
        "jit_provisioning, default_role, enabled, created_at, updated_at "
        "FROM sso_providers WHERE tenant_id = ? AND enabled = 1 LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<SsoProvider> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) result = ssoProviderFromStmt(stmt);
    sqlite3_finalize(stmt);
    return result;
}

std::vector<SsoProvider> SQLitePersistentStore::listSsoProviders(int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SsoProvider> result;
    if (!db_) return result;
    const char* sql =
        "SELECT id, tenant_id, name, issuer_url, client_id, client_secret_enc, "
        "redirect_uri, scopes_json, claim_mapping_json, group_role_mapping_json, "
        "jit_provisioning, default_role, enabled, created_at, updated_at "
        "FROM sso_providers LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_int(stmt, 1, limit);
    sqlite3_bind_int(stmt, 2, offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) result.push_back(ssoProviderFromStmt(stmt));
    sqlite3_finalize(stmt);
    return result;
}

int64_t SQLitePersistentStore::ssoProviderCount() {
    return sqliteCountWhereTenant(db_, mutex_, "sso_providers", "");
}

bool SQLitePersistentStore::updateSsoProvider(const SsoProvider& p) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql =
        "UPDATE sso_providers SET tenant_id=?, name=?, issuer_url=?, client_id=?, "
        "client_secret_enc=?, redirect_uri=?, scopes_json=?, claim_mapping_json=?, "
        "group_role_mapping_json=?, jit_provisioning=?, default_role=?, enabled=?, "
        "updated_at=? WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    std::string scopes_json = serializeStringList(p.scopes);
    sqlite3_bind_text(stmt, 1, p.tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, p.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, p.issuer_url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, p.client_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, p.client_secret_enc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, p.redirect_uri.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, scopes_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, p.claim_mapping_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, p.group_role_mapping_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 10, p.jit_provisioning ? 1 : 0);
    sqlite3_bind_text(stmt, 11, p.default_role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 12, p.enabled ? 1 : 0);
    sqlite3_bind_text(stmt, 13, p.updated_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 14, p.id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db_) > 0;
}

bool SQLitePersistentStore::deleteSsoProvider(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "DELETE FROM sso_providers WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

// --- Identity Mapping ---

bool SQLitePersistentStore::insertIdentityMapping(const IdentityMapping& m) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql =
        "INSERT INTO identity_mappings (id, tenant_id, external_subject, external_issuer, "
        "user_id, email, last_login_at, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, m.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, m.tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, m.external_subject.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, m.external_issuer.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, m.user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, m.email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, m.last_login_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, m.created_at.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<IdentityMapping> SQLitePersistentStore::getIdentityMapping(
    const std::string& external_subject, const std::string& external_issuer) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    const char* sql =
        "SELECT id, tenant_id, external_subject, external_issuer, user_id, email, "
        "last_login_at, created_at FROM identity_mappings "
        "WHERE external_subject = ? AND external_issuer = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, external_subject.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, external_issuer.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<IdentityMapping> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) result = identityMappingFromStmt(stmt);
    sqlite3_finalize(stmt);
    return result;
}

std::vector<IdentityMapping> SQLitePersistentStore::listIdentityMappings(
    const std::string& tenant_id, int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<IdentityMapping> result;
    if (!db_) return result;
    const char* sql =
        "SELECT id, tenant_id, external_subject, external_issuer, user_id, email, "
        "last_login_at, created_at FROM identity_mappings "
        "WHERE tenant_id = ? LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    sqlite3_bind_int(stmt, 3, offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) result.push_back(identityMappingFromStmt(stmt));
    sqlite3_finalize(stmt);
    return result;
}

bool SQLitePersistentStore::deleteIdentityMapping(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "DELETE FROM identity_mappings WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

bool SQLitePersistentStore::updateIdentityMappingLastLogin(const std::string& id, const std::string& ts) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "UPDATE identity_mappings SET last_login_at = ? WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db_) > 0;
}

// --- Session ---

bool SQLitePersistentStore::insertSession(const Session& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql =
        "INSERT INTO sessions (id, user_id, tenant_id, ip_address, user_agent, "
        "auth_method, mfa_verified, created_at, last_active_at, expires_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, s.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, s.user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, s.tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, s.ip_address.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, s.user_agent.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, s.auth_method.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, s.mfa_verified ? 1 : 0);
    sqlite3_bind_text(stmt, 8, s.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, s.last_active_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, s.expires_at.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<Session> SQLitePersistentStore::getSession(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    const char* sql =
        "SELECT id, user_id, tenant_id, ip_address, user_agent, auth_method, "
        "mfa_verified, created_at, last_active_at, expires_at FROM sessions WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<Session> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) result = sessionFromStmt(stmt);
    sqlite3_finalize(stmt);
    return result;
}

std::vector<Session> SQLitePersistentStore::listSessionsByUser(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Session> result;
    if (!db_) return result;
    const char* sql =
        "SELECT id, user_id, tenant_id, ip_address, user_agent, auth_method, "
        "mfa_verified, created_at, last_active_at, expires_at "
        "FROM sessions WHERE user_id = ? ORDER BY created_at DESC";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) result.push_back(sessionFromStmt(stmt));
    sqlite3_finalize(stmt);
    return result;
}

bool SQLitePersistentStore::updateSessionActivity(const std::string& id, const std::string& ts) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "UPDATE sessions SET last_active_at = ? WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db_) > 0;
}

bool SQLitePersistentStore::deleteSession(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "DELETE FROM sessions WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

int64_t SQLitePersistentStore::deleteExpiredSessions() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;
    // C11 ①：expires_at 为 ISO8601（nowIso），比较基准须同格式避免同日边界错位。
    const char* sql = "DELETE FROM sessions WHERE expires_at < "
                      "strftime('%Y-%m-%dT%H:%M:%SZ', 'now')";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_step(stmt);
    int64_t deleted = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return deleted;
}

int64_t SQLitePersistentStore::countSessionsByUser(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;
    const char* sql = "SELECT COUNT(*) FROM sessions WHERE user_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

bool SQLitePersistentStore::updateSessionMfaVerified(const std::string& id, bool verified) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "UPDATE sessions SET mfa_verified = ? WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, verified ? 1 : 0);
    sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db_) > 0;
}

// --- MFA ---

bool SQLitePersistentStore::upsertMfaSecret(const MfaSecret& m) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql =
        "INSERT OR REPLACE INTO mfa_secrets (user_id, secret_enc, enabled, "
        "recovery_codes_enc, created_at) VALUES (?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    std::string codes_json = serializeStringList(m.recovery_codes_hash);
    sqlite3_bind_text(stmt, 1, m.user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, m.secret_enc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, m.enabled ? 1 : 0);
    sqlite3_bind_text(stmt, 4, codes_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, m.created_at.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<MfaSecret> SQLitePersistentStore::getMfaSecret(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    const char* sql =
        "SELECT user_id, secret_enc, enabled, recovery_codes_enc, created_at "
        "FROM mfa_secrets WHERE user_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<MfaSecret> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) result = mfaSecretFromStmt(stmt);
    sqlite3_finalize(stmt);
    return result;
}

bool SQLitePersistentStore::recordMfaFailure(const std::string& user_id,
                                             int max_failures, int window_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    int64_t first_fail_at = 0, locked_until = 0;
    int fail_count = 0;
    {
        const char* q = "SELECT fail_count, first_fail_at, locked_until "
                        "FROM mfa_failures WHERE user_id = ?";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, q, -1, &st, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(st, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            fail_count = sqlite3_column_int(st, 0);
            first_fail_at = sqlite3_column_int64(st, 1);
            locked_until = sqlite3_column_int64(st, 2);
        }
        sqlite3_finalize(st);
    }

    if (first_fail_at == 0 || now - first_fail_at > window_sec) {
        first_fail_at = now;
        fail_count = 1;
    } else {
        fail_count += 1;
    }
    if (fail_count >= max_failures) {
        locked_until = now + window_sec;
    }

    const char* up =
        "INSERT OR REPLACE INTO mfa_failures "
        "(user_id, fail_count, first_fail_at, locked_until) VALUES (?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, up, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, fail_count);
    sqlite3_bind_int64(stmt, 3, first_fail_at);
    sqlite3_bind_int64(stmt, 4, locked_until);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return locked_until > now;
}

int64_t SQLitePersistentStore::getMfaLockedUntil(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;
    const char* sql = "SELECT locked_until FROM mfa_failures WHERE user_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    int64_t locked_until = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) locked_until = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return locked_until;
}

bool SQLitePersistentStore::clearMfaFailures(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "DELETE FROM mfa_failures WHERE user_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return true;
}

bool SQLitePersistentStore::deleteMfaSecret(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "DELETE FROM mfa_secrets WHERE user_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

// --- SCIM Token ---

bool SQLitePersistentStore::insertScimToken(const ScimToken& t) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql =
        "INSERT INTO scim_tokens (id, tenant_id, token_hash, description, "
        "created_at, expires_at) VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, t.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, t.tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, t.token_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, t.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, t.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, t.expires_at.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<ScimToken> SQLitePersistentStore::getScimTokenByHash(const std::string& hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    const char* sql =
        "SELECT id, tenant_id, token_hash, description, created_at, expires_at "
        "FROM scim_tokens WHERE token_hash = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<ScimToken> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) result = scimTokenFromStmt(stmt);
    sqlite3_finalize(stmt);
    return result;
}

std::vector<ScimToken> SQLitePersistentStore::listScimTokens(const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ScimToken> result;
    if (!db_) return result;
    const char* sql =
        "SELECT id, tenant_id, token_hash, description, created_at, expires_at "
        "FROM scim_tokens WHERE tenant_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) result.push_back(scimTokenFromStmt(stmt));
    sqlite3_finalize(stmt);
    return result;
}

bool SQLitePersistentStore::deleteScimToken(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "DELETE FROM scim_tokens WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

// --- SCIM Group ---

bool SQLitePersistentStore::insertScimGroup(const std::string& id,
                                             const std::string& tenant_id,
                                             const std::string& display_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql =
        "INSERT INTO scim_groups (id, tenant_id, display_name) VALUES (?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, display_name.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<PersistentStore::ScimGroupRecord>
SQLitePersistentStore::getScimGroup(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    const char* sql =
        "SELECT id, tenant_id, display_name, created_at, updated_at FROM scim_groups WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<ScimGroupRecord> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ScimGroupRecord r;
        r.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        r.tenant_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        r.display_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        auto ca = sqlite3_column_text(stmt, 3);
        if (ca) r.created_at = reinterpret_cast<const char*>(ca);
        auto ua = sqlite3_column_text(stmt, 4);
        if (ua) r.updated_at = reinterpret_cast<const char*>(ua);
        result = std::move(r);
    }
    sqlite3_finalize(stmt);
    return result;
}

bool SQLitePersistentStore::updateScimGroup(const std::string& id,
                                             const std::string& display_name,
                                             const std::vector<std::string>& member_ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql_upd =
        "UPDATE scim_groups SET display_name = ?, updated_at = datetime('now') WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql_upd, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (changes == 0) return false;

    const char* sql_del = "DELETE FROM scim_group_members WHERE group_id = ?";
    sqlite3_stmt* del_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql_del, -1, &del_stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(del_stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(del_stmt);
        sqlite3_finalize(del_stmt);
    }

    const char* sql_ins =
        "INSERT INTO scim_group_members (group_id, user_id) VALUES (?, ?)";
    for (const auto& uid : member_ids) {
        sqlite3_stmt* ins = nullptr;
        if (sqlite3_prepare_v2(db_, sql_ins, -1, &ins, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(ins, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins, 2, uid.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(ins);
            sqlite3_finalize(ins);
        }
    }
    return true;
}

bool SQLitePersistentStore::deleteScimGroup(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "DELETE FROM scim_groups WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

std::vector<PersistentStore::ScimGroupRecord>
SQLitePersistentStore::listScimGroups(const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ScimGroupRecord> result;
    if (!db_) return result;
    const char* sql =
        "SELECT id, tenant_id, display_name, created_at, updated_at "
        "FROM scim_groups WHERE tenant_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ScimGroupRecord r;
        r.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        r.tenant_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        r.display_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        auto ca = sqlite3_column_text(stmt, 3);
        if (ca) r.created_at = reinterpret_cast<const char*>(ca);
        auto ua = sqlite3_column_text(stmt, 4);
        if (ua) r.updated_at = reinterpret_cast<const char*>(ua);
        result.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<std::string>
SQLitePersistentStore::getScimGroupMembers(const std::string& group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    if (!db_) return result;
    const char* sql = "SELECT user_id FROM scim_group_members WHERE group_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    return result;
}

// --- Prompt Template ---

bool SQLitePersistentStore::insertPromptTemplate(const PromptTemplateRecord& tpl) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql =
        "INSERT INTO prompt_templates (id, tenant_id, name, content, version, weight, "
        "is_active, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, tpl.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, tpl.tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, tpl.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, tpl.content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, tpl.version);
    sqlite3_bind_int(stmt, 6, tpl.weight);
    sqlite3_bind_int(stmt, 7, tpl.is_active ? 1 : 0);
    sqlite3_bind_text(stmt, 8, tpl.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, tpl.updated_at.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<PersistentStore::PromptTemplateRecord>
SQLitePersistentStore::getPromptTemplate(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    const char* sql =
        "SELECT id, tenant_id, name, content, version, weight, is_active, "
        "created_at, updated_at FROM prompt_templates WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return std::nullopt; }
    PromptTemplateRecord r;
    r.id = safeColumnText(stmt, 0);
    r.tenant_id = safeColumnText(stmt, 1);
    r.name = safeColumnText(stmt, 2);
    r.content = safeColumnText(stmt, 3);
    r.version = sqlite3_column_int(stmt, 4);
    r.weight = sqlite3_column_int(stmt, 5);
    r.is_active = sqlite3_column_int(stmt, 6) != 0;
    r.created_at = safeColumnText(stmt, 7);
    r.updated_at = safeColumnText(stmt, 8);
    sqlite3_finalize(stmt);
    return r;
}

bool SQLitePersistentStore::updatePromptTemplate(const PromptTemplateRecord& tpl) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql =
        "UPDATE prompt_templates SET name=?, content=?, version=?, weight=?, "
        "is_active=?, updated_at=? WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, tpl.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, tpl.content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, tpl.version);
    sqlite3_bind_int(stmt, 4, tpl.weight);
    sqlite3_bind_int(stmt, 5, tpl.is_active ? 1 : 0);
    sqlite3_bind_text(stmt, 6, tpl.updated_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, tpl.id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    sqlite3_finalize(stmt);
    return ok;
}

bool SQLitePersistentStore::deletePromptTemplate(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "DELETE FROM prompt_templates WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<PersistentStore::PromptTemplateRecord>
SQLitePersistentStore::listPromptTemplates(const std::string& tenant_id,
                                            int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PromptTemplateRecord> result;
    if (!db_) return result;
    const char* sql =
        "SELECT id, tenant_id, name, content, version, weight, is_active, "
        "created_at, updated_at FROM prompt_templates WHERE tenant_id = ? "
        "LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    sqlite3_bind_int(stmt, 3, offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PromptTemplateRecord r;
        r.id = safeColumnText(stmt, 0);
        r.tenant_id = safeColumnText(stmt, 1);
        r.name = safeColumnText(stmt, 2);
        r.content = safeColumnText(stmt, 3);
        r.version = sqlite3_column_int(stmt, 4);
        r.weight = sqlite3_column_int(stmt, 5);
        r.is_active = sqlite3_column_int(stmt, 6) != 0;
        r.created_at = safeColumnText(stmt, 7);
        r.updated_at = safeColumnText(stmt, 8);
        result.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<PersistentStore::PromptTemplateRecord>
SQLitePersistentStore::listPromptTemplatesByName(const std::string& tenant_id,
                                                  const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PromptTemplateRecord> result;
    if (!db_) return result;
    const char* sql =
        "SELECT id, tenant_id, name, content, version, weight, is_active, "
        "created_at, updated_at FROM prompt_templates "
        "WHERE tenant_id = ? AND name = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PromptTemplateRecord r;
        r.id = safeColumnText(stmt, 0);
        r.tenant_id = safeColumnText(stmt, 1);
        r.name = safeColumnText(stmt, 2);
        r.content = safeColumnText(stmt, 3);
        r.version = sqlite3_column_int(stmt, 4);
        r.weight = sqlite3_column_int(stmt, 5);
        r.is_active = sqlite3_column_int(stmt, 6) != 0;
        r.created_at = safeColumnText(stmt, 7);
        r.updated_at = safeColumnText(stmt, 8);
        result.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return result;
}

// --- Rule Set ---

bool SQLitePersistentStore::insertRuleSet(const std::string& tenant_id,
                                           const RuleSetRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    if (record.is_active) {
        const char* deactivate_sql =
            "UPDATE rule_sets SET is_active = 0 WHERE tenant_id = ?";
        sqlite3_stmt* ds = nullptr;
        if (sqlite3_prepare_v2(db_, deactivate_sql, -1, &ds, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(ds, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(ds);
            sqlite3_finalize(ds);
        }
    }

    const char* sql =
        "INSERT INTO rule_sets (tenant_id, version, rules_json, created_at, is_active) "
        "VALUES (?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, record.version);
    sqlite3_bind_text(stmt, 3, record.rules_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, record.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, record.is_active ? 1 : 0);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<PersistentStore::RuleSetRecord>
SQLitePersistentStore::getActiveRuleSet(const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    const char* sql =
        "SELECT version, rules_json, created_at, is_active "
        "FROM rule_sets WHERE tenant_id = ? AND is_active = 1 LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    RuleSetRecord r;
    r.tenant_id = tenant_id;
    r.version = sqlite3_column_int64(stmt, 0);
    r.rules_json = safeColumnText(stmt, 1);
    r.created_at = safeColumnText(stmt, 2);
    r.is_active = sqlite3_column_int(stmt, 3) != 0;
    sqlite3_finalize(stmt);
    return r;
}

std::vector<PersistentStore::RuleSetRecord>
SQLitePersistentStore::listRuleSetVersions(const std::string& tenant_id, int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RuleSetRecord> result;
    if (!db_) return result;
    const char* sql =
        "SELECT version, rules_json, created_at, is_active "
        "FROM rule_sets WHERE tenant_id = ? ORDER BY version DESC LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    sqlite3_bind_int(stmt, 3, offset < 0 ? 0 : offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RuleSetRecord r;
        r.tenant_id = tenant_id;
        r.version = sqlite3_column_int64(stmt, 0);
        r.rules_json = safeColumnText(stmt, 1);
        r.created_at = safeColumnText(stmt, 2);
        r.is_active = sqlite3_column_int(stmt, 3) != 0;
        result.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return result;
}

bool SQLitePersistentStore::activateRuleSetVersion(const std::string& tenant_id,
                                                    int64_t version) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* check_sql =
        "SELECT COUNT(*) FROM rule_sets WHERE tenant_id = ? AND version = ?";
    sqlite3_stmt* cs = nullptr;
    if (sqlite3_prepare_v2(db_, check_sql, -1, &cs, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(cs, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(cs, 2, version);
    bool exists = false;
    if (sqlite3_step(cs) == SQLITE_ROW) {
        exists = sqlite3_column_int(cs, 0) > 0;
    }
    sqlite3_finalize(cs);
    if (!exists) return false;

    const char* deactivate_sql =
        "UPDATE rule_sets SET is_active = 0 WHERE tenant_id = ?";
    sqlite3_stmt* ds = nullptr;
    if (sqlite3_prepare_v2(db_, deactivate_sql, -1, &ds, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(ds, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(ds);
        sqlite3_finalize(ds);
    }

    const char* activate_sql =
        "UPDATE rule_sets SET is_active = 1 WHERE tenant_id = ? AND version = ?";
    sqlite3_stmt* as = nullptr;
    if (sqlite3_prepare_v2(db_, activate_sql, -1, &as, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(as, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(as, 2, version);
    bool ok = sqlite3_step(as) == SQLITE_DONE;
    sqlite3_finalize(as);
    return ok;
}

// --- ConfigBundle Versioning (Phase 9.3) ---

namespace {

// sqlite3_finalize-safe wrapper over prepare + single-step execution,
// only used for short statements inside activateConfig's transaction.
bool execStmt(sqlite3* db, const char* sql,
              const std::function<void(sqlite3_stmt*)>& bind_fn) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) return false;
    bind_fn(s);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}

ConfigVersionRecord rowToConfigVersion(sqlite3_stmt* stmt) {
    ConfigVersionRecord r{};
    r.version_id        = safeColumnText(stmt, 0);
    r.content_sha256    = safeColumnText(stmt, 1);
    const void* blob    = sqlite3_column_blob(stmt, 2);
    int         blen    = sqlite3_column_bytes(stmt, 2);
    if (blob && blen > 0) {
        r.yaml_content.assign(static_cast<const char*>(blob),
                              static_cast<size_t>(blen));
    }
    r.size_bytes        = sqlite3_column_int64(stmt, 3);
    auto parsed = configStatusFromString(safeColumnText(stmt, 4));
    r.status            = parsed.value_or(ConfigStatus::PENDING);
    r.submitter         = safeColumnText(stmt, 5);
    r.submitter_comment = safeColumnText(stmt, 6);
    r.submitted_at      = sqlite3_column_int64(stmt, 7);
    r.reviewer          = safeColumnText(stmt, 8);
    r.reviewer_comment  = safeColumnText(stmt, 9);
    r.reviewed_at       = sqlite3_column_int64(stmt, 10);
    r.activator         = safeColumnText(stmt, 11);
    r.activated_at      = sqlite3_column_int64(stmt, 12);
    r.deactivated_at    = sqlite3_column_int64(stmt, 13);
    r.chain_hash        = safeColumnText(stmt, 14);
    return r;
}

const char* kConfigVersionCols =
    "version_id, content_sha256, yaml_content, size_bytes, status, "
    "submitter, submitter_comment, submitted_at, reviewer, reviewer_comment, "
    "reviewed_at, activator, activated_at, deactivated_at, chain_hash";

} // namespace

bool SQLitePersistentStore::insertConfigVersion(const ConfigVersionRecord& rec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    if (rec.version_id.empty()) return false;

    const char* sql =
        "INSERT INTO config_versions (version_id, content_sha256, yaml_content, "
        "size_bytes, status, submitter, submitter_comment, submitted_at, "
        "reviewer, reviewer_comment, reviewed_at, activator, activated_at, "
        "deactivated_at, chain_hash) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    int i = 1;
    sqlite3_bind_text(stmt, i++, rec.version_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, i++, rec.content_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, i++, rec.yaml_content.data(),
                      static_cast<int>(rec.yaml_content.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, i++, rec.size_bytes);
    sqlite3_bind_text(stmt, i++, configStatusToString(rec.status), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, i++, rec.submitter.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, i++, rec.submitter_comment.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, i++, rec.submitted_at);
    sqlite3_bind_text(stmt, i++, rec.reviewer.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, i++, rec.reviewer_comment.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, i++, rec.reviewed_at);
    sqlite3_bind_text(stmt, i++, rec.activator.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, i++, rec.activated_at);
    sqlite3_bind_int64(stmt, i++, rec.deactivated_at);
    sqlite3_bind_text(stmt, i++, rec.chain_hash.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool SQLitePersistentStore::updateConfigStatus(
    const std::string& version_id,
    ConfigStatus new_status,
    const std::string& actor,
    const std::string& comment,
    std::int64_t timestamp_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    // Caller must exist; a raw UPDATE would return 0 rows but still SQLITE_DONE.
    const char* count_sql =
        "SELECT COUNT(*) FROM config_versions WHERE version_id = ?";
    sqlite3_stmt* cs = nullptr;
    if (sqlite3_prepare_v2(db_, count_sql, -1, &cs, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(cs, 1, version_id.c_str(), -1, SQLITE_TRANSIENT);
    bool exists = sqlite3_step(cs) == SQLITE_ROW && sqlite3_column_int(cs, 0) > 0;
    sqlite3_finalize(cs);
    if (!exists) return false;

    const bool is_review = (new_status == ConfigStatus::APPROVED ||
                            new_status == ConfigStatus::REJECTED);
    const char* sql = is_review
        ? "UPDATE config_versions SET status = ?, reviewer = ?, "
          "reviewer_comment = ?, reviewed_at = ? WHERE version_id = ?"
        : "UPDATE config_versions SET status = ? WHERE version_id = ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    int i = 1;
    sqlite3_bind_text(stmt, i++, configStatusToString(new_status), -1, SQLITE_STATIC);
    if (is_review) {
        sqlite3_bind_text(stmt, i++, actor.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, i++, comment.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, i++, timestamp_ms);
    }
    sqlite3_bind_text(stmt, i++, version_id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<ConfigVersionRecord>
SQLitePersistentStore::getConfigVersion(const std::string& version_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    std::string sql = std::string("SELECT ") + kConfigVersionCols +
                      " FROM config_versions WHERE version_id = ? LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, version_id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<ConfigVersionRecord> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = rowToConfigVersion(stmt);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<ConfigVersionRecord>
SQLitePersistentStore::listConfigVersions(const ConfigVersionQuery& q) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConfigVersionRecord> out;
    if (!db_) return out;

    // Build WHERE dynamically; SQLite supports up to 999 parameters.
    std::string sql = std::string("SELECT ") + kConfigVersionCols +
                      " FROM config_versions WHERE 1=1";
    std::vector<std::string> bindings; // keeps lifetimes until step
    std::vector<std::int64_t> bindings_int;
    if (!q.statuses.empty()) {
        sql += " AND status IN (";
        for (size_t k = 0; k < q.statuses.size(); ++k) {
            sql += (k == 0 ? "?" : ",?");
            bindings.emplace_back(configStatusToString(q.statuses[k]));
        }
        sql += ")";
    }
    if (q.since_millis > 0) {
        sql += " AND submitted_at >= ?";
        bindings_int.push_back(q.since_millis);
    }
    // Cursor: rows strictly older than the page_token row.
    // Since ordering is submitted_at DESC, version_id DESC, "after" means:
    //   submitted_at < tok_submitted OR
    //   (submitted_at = tok_submitted AND version_id < tok_version_id)
    std::int64_t tok_submitted = -1;
    if (!q.page_token.empty()) {
        const char* lookup_sql =
            "SELECT submitted_at FROM config_versions WHERE version_id = ?";
        sqlite3_stmt* ls = nullptr;
        if (sqlite3_prepare_v2(db_, lookup_sql, -1, &ls, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(ls, 1, q.page_token.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(ls) == SQLITE_ROW) {
                tok_submitted = sqlite3_column_int64(ls, 0);
            }
            sqlite3_finalize(ls);
        }
        if (tok_submitted >= 0) {
            sql += " AND (submitted_at < ? OR (submitted_at = ? AND version_id < ?))";
            bindings_int.push_back(tok_submitted);
            bindings_int.push_back(tok_submitted);
            bindings.emplace_back(q.page_token);
        }
    }
    sql += " ORDER BY submitted_at DESC, version_id DESC LIMIT ?";
    const int cap = q.limit > 0 ? (q.limit > 500 ? 500 : q.limit) : 50;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    // Re-bind in declaration order (SQLite numbers '?' left-to-right).
    int idx = 1;
    // Rebuild: statuses first, then since_millis, then (page_token triple),
    // finally LIMIT. We need a single pass that matches the construction.
    size_t txt_cursor = 0;
    size_t int_cursor = 0;
    for (size_t k = 0; k < q.statuses.size(); ++k) {
        sqlite3_bind_text(stmt, idx++, bindings[txt_cursor++].c_str(), -1,
                          SQLITE_TRANSIENT);
    }
    if (q.since_millis > 0) {
        sqlite3_bind_int64(stmt, idx++, bindings_int[int_cursor++]);
    }
    if (!q.page_token.empty() && tok_submitted >= 0) {
        sqlite3_bind_int64(stmt, idx++, bindings_int[int_cursor++]); // <
        sqlite3_bind_int64(stmt, idx++, bindings_int[int_cursor++]); // =
        sqlite3_bind_text(stmt, idx++, bindings[txt_cursor++].c_str(), -1,
                          SQLITE_TRANSIENT);                          // version_id <
    }
    sqlite3_bind_int(stmt, idx++, cap);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(rowToConfigVersion(stmt));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::optional<ConfigVersionRecord>
SQLitePersistentStore::getActiveConfig() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    std::string sql = std::string("SELECT ") + kConfigVersionCols +
                      " FROM config_versions WHERE status = 'ACTIVE' LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    std::optional<ConfigVersionRecord> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = rowToConfigVersion(stmt);
    }
    sqlite3_finalize(stmt);
    return out;
}

bool SQLitePersistentStore::activateConfig(
    const std::string& version_id,
    const std::string& activator,
    std::int64_t activate_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    if (version_id.empty()) return false;

    // Fetch current status of target in a separate statement (inside tx).
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }

    auto rollback = [&]() {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    };

    // 1) Load target status.
    std::string target_status;
    {
        const char* sql = "SELECT status FROM config_versions "
                          "WHERE version_id = ? LIMIT 1";
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &s, nullptr) != SQLITE_OK) {
            rollback(); return false;
        }
        sqlite3_bind_text(s, 1, version_id.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(s);
        if (rc == SQLITE_ROW) target_status = safeColumnText(s, 0);
        sqlite3_finalize(s);
        if (target_status.empty()) { rollback(); return false; }
    }

    // 2) Idempotent: target already ACTIVE -> success with no writes.
    if (target_status == "ACTIVE") {
        sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
        return true;
    }
    // 3) Only APPROVED / SUPERSEDED may activate (R2 rollback uses latter).
    if (target_status != "APPROVED" && target_status != "SUPERSEDED") {
        rollback(); return false;
    }

    // 4) Demote any existing ACTIVE to SUPERSEDED.
    bool ok = execStmt(db_,
        "UPDATE config_versions "
        "SET status = 'SUPERSEDED', deactivated_at = ? "
        "WHERE status = 'ACTIVE' AND version_id <> ?",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_int64(s, 1, activate_ms);
            sqlite3_bind_text(s, 2, version_id.c_str(), -1, SQLITE_TRANSIENT);
        });
    if (!ok) { rollback(); return false; }

    // 5) Promote target to ACTIVE.
    ok = execStmt(db_,
        "UPDATE config_versions "
        "SET status = 'ACTIVE', activator = ?, activated_at = ?, "
        "    deactivated_at = 0 "
        "WHERE version_id = ?",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, activator.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 2, activate_ms);
            sqlite3_bind_text(s, 3, version_id.c_str(), -1, SQLITE_TRANSIENT);
        });
    if (!ok) { rollback(); return false; }

    return sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK;
}

// ======================================================================
// Phase 9.3.4 RolloutController — SQLite rollout CRUD + stage-event log.
// Schema: see storage/rollout_schema.h (§5.1 of design doc).
// The partial UNIQUE INDEX on (target_version_id WHERE status IN (1,2,3))
// enforces the "at most one active rollout per target" invariant at the
// schema layer; application-level findActiveRolloutByTarget provides the
// read side and a friendlier error path.
// ======================================================================

namespace {

// Column order used by SELECT / row binding. Keep in sync with bindAll().
constexpr const char* kRolloutCols =
    "rollout_id, target_version_id, previous_active_version_id, "
    "spec_json, status, current_stage_index, started_at, stage_started_at, "
    "paused_at, pause_reason, pause_detail, creator, last_actor, "
    "completed_at, chain_hash";

RolloutRecord rowToRollout(sqlite3_stmt* stmt) {
    RolloutRecord r{};
    r.rollout_id                 = safeColumnText(stmt, 0);
    r.target_version_id          = safeColumnText(stmt, 1);
    r.previous_active_version_id = safeColumnText(stmt, 2);

    // spec_json is a BLOB (may contain NULs — keep byte-safe read).
    const void* spec_blob = sqlite3_column_blob(stmt, 3);
    int spec_bytes = sqlite3_column_bytes(stmt, 3);
    std::string spec_str(static_cast<const char*>(spec_blob),
                         spec_blob ? static_cast<size_t>(spec_bytes) : 0u);
    r.spec = parseRolloutSpec(spec_str);

    r.status              = rolloutStatusFromWire(sqlite3_column_int(stmt, 4));
    r.current_stage_index = sqlite3_column_int(stmt, 5);
    r.started_at          = sqlite3_column_int64(stmt, 6);
    r.stage_started_at    = sqlite3_column_int64(stmt, 7);
    r.paused_at           = sqlite3_column_int64(stmt, 8);
    r.pause_reason        = pauseReasonFromWire(sqlite3_column_int(stmt, 9));
    r.pause_detail        = safeColumnText(stmt, 10);
    r.creator             = safeColumnText(stmt, 11);
    r.last_actor          = safeColumnText(stmt, 12);
    r.completed_at        = sqlite3_column_int64(stmt, 13);
    r.chain_hash          = safeColumnText(stmt, 14);
    return r;
}

// Binds 15 columns starting at parameter index 1.
void bindRollout(sqlite3_stmt* stmt, const RolloutRecord& r,
                  const std::string& spec_json) {
    int i = 1;
    sqlite3_bind_text (stmt, i++, r.rollout_id.c_str(),          -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, r.target_version_id.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, r.previous_active_version_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob (stmt, i++, spec_json.data(),
                       static_cast<int>(spec_json.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int  (stmt, i++, rolloutStatusToWire(r.status));
    sqlite3_bind_int  (stmt, i++, r.current_stage_index);
    sqlite3_bind_int64(stmt, i++, r.started_at);
    sqlite3_bind_int64(stmt, i++, r.stage_started_at);
    sqlite3_bind_int64(stmt, i++, r.paused_at);
    sqlite3_bind_int  (stmt, i++, pauseReasonToWire(r.pause_reason));
    sqlite3_bind_text (stmt, i++, r.pause_detail.c_str(),        -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, r.creator.c_str(),             -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, r.last_actor.c_str(),          -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, i++, r.completed_at);
    sqlite3_bind_text (stmt, i++, r.chain_hash.c_str(),          -1, SQLITE_TRANSIENT);
}

} // namespace

bool SQLitePersistentStore::insertRollout(const RolloutRecord& rec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    if (rec.rollout_id.empty()) return false;

    const std::string sql =
        std::string("INSERT INTO rollouts (") + kRolloutCols +
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    const std::string spec_json = serializeRolloutSpec(rec.spec);
    bindRollout(stmt, rec, spec_json);
    // SQLITE_CONSTRAINT can fire from either PRIMARY KEY (dup id) or the
    // partial UNIQUE INDEX (dup active target). Either way the insert is
    // rejected atomically — callers treat both as "false".
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool SQLitePersistentStore::updateRollout(const RolloutRecord& rec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    // UPDATE ... WHERE version_id=? returns SQLITE_DONE for 0 rows too, so
    // we need an explicit existence check (mirrors updateConfigStatus).
    const char* count_sql =
        "SELECT COUNT(*) FROM rollouts WHERE rollout_id = ?";
    sqlite3_stmt* cs = nullptr;
    if (sqlite3_prepare_v2(db_, count_sql, -1, &cs, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(cs, 1, rec.rollout_id.c_str(), -1, SQLITE_TRANSIENT);
    bool exists = sqlite3_step(cs) == SQLITE_ROW && sqlite3_column_int(cs, 0) > 0;
    sqlite3_finalize(cs);
    if (!exists) return false;

    const char* sql =
        "UPDATE rollouts SET "
        "target_version_id = ?, previous_active_version_id = ?, "
        "spec_json = ?, status = ?, current_stage_index = ?, "
        "started_at = ?, stage_started_at = ?, paused_at = ?, "
        "pause_reason = ?, pause_detail = ?, creator = ?, last_actor = ?, "
        "completed_at = ?, chain_hash = ? "
        "WHERE rollout_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    const std::string spec_json = serializeRolloutSpec(rec.spec);
    int i = 1;
    sqlite3_bind_text (stmt, i++, rec.target_version_id.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, rec.previous_active_version_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob (stmt, i++, spec_json.data(),
                       static_cast<int>(spec_json.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int  (stmt, i++, rolloutStatusToWire(rec.status));
    sqlite3_bind_int  (stmt, i++, rec.current_stage_index);
    sqlite3_bind_int64(stmt, i++, rec.started_at);
    sqlite3_bind_int64(stmt, i++, rec.stage_started_at);
    sqlite3_bind_int64(stmt, i++, rec.paused_at);
    sqlite3_bind_int  (stmt, i++, pauseReasonToWire(rec.pause_reason));
    sqlite3_bind_text (stmt, i++, rec.pause_detail.c_str(),        -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, rec.creator.c_str(),             -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, rec.last_actor.c_str(),          -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, i++, rec.completed_at);
    sqlite3_bind_text (stmt, i++, rec.chain_hash.c_str(),          -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, rec.rollout_id.c_str(),          -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<RolloutRecord>
SQLitePersistentStore::getRollout(const std::string& rollout_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    const std::string sql = std::string("SELECT ") + kRolloutCols +
                            " FROM rollouts WHERE rollout_id = ? LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, rollout_id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<RolloutRecord> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) out = rowToRollout(stmt);
    sqlite3_finalize(stmt);
    return out;
}

std::optional<RolloutRecord>
SQLitePersistentStore::findActiveRolloutByTarget(
    const std::string& target_version_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    // status IN (1,2,3) mirrors the partial UNIQUE INDEX so we leverage it.
    const std::string sql =
        std::string("SELECT ") + kRolloutCols +
        " FROM rollouts WHERE target_version_id = ? AND status IN (1,2,3) "
        "LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, target_version_id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<RolloutRecord> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) out = rowToRollout(stmt);
    sqlite3_finalize(stmt);
    return out;
}

std::vector<RolloutRecord>
SQLitePersistentStore::listRollouts(const RolloutQuery& q) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RolloutRecord> out;
    if (!db_) return out;

    std::string sql = std::string("SELECT ") + kRolloutCols +
                      " FROM rollouts WHERE 1=1";
    std::vector<int> bindings_int;
    if (!q.statuses.empty()) {
        sql += " AND status IN (";
        for (size_t k = 0; k < q.statuses.size(); ++k) {
            sql += (k == 0 ? "?" : ",?");
            bindings_int.push_back(rolloutStatusToWire(q.statuses[k]));
        }
        sql += ")";
    }
    // Cursor: page_token is the last rollout_id of previous page. Ordering
    // is (started_at DESC, rollout_id DESC). Matching predicate:
    //   started_at < tok_started OR
    //   (started_at = tok_started AND rollout_id < tok_rollout_id)
    std::int64_t tok_started = -1;
    std::string tok_id;
    if (!q.page_token.empty()) {
        // Look up the token row to materialize its started_at; skip if not
        // found (treat as "first page").
        sqlite3_stmt* tstmt = nullptr;
        if (sqlite3_prepare_v2(db_,
                "SELECT started_at FROM rollouts WHERE rollout_id = ?", -1,
                &tstmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(tstmt, 1, q.page_token.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(tstmt) == SQLITE_ROW) {
                tok_started = sqlite3_column_int64(tstmt, 0);
                tok_id = q.page_token;
            }
            sqlite3_finalize(tstmt);
        }
    }
    if (!tok_id.empty()) {
        sql += " AND (started_at < ? OR (started_at = ? AND rollout_id < ?))";
    }
    sql += " ORDER BY started_at DESC, rollout_id DESC";
    if (q.limit > 0) sql += " LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    int idx = 1;
    for (int s : bindings_int) sqlite3_bind_int(stmt, idx++, s);
    if (!tok_id.empty()) {
        sqlite3_bind_int64(stmt, idx++, tok_started);
        sqlite3_bind_int64(stmt, idx++, tok_started);
        sqlite3_bind_text (stmt, idx++, tok_id.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (q.limit > 0) sqlite3_bind_int(stmt, idx++, q.limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(rowToRollout(stmt));
    }
    sqlite3_finalize(stmt);
    return out;
}

bool SQLitePersistentStore::appendRolloutStageEvent(
    const RolloutStageEvent& ev) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    if (ev.event_id.empty() || ev.rollout_id.empty()) return false;

    const char* sql =
        "INSERT INTO rollout_stage_events (event_id, rollout_id, "
        "stage_index, event_type, reason, metrics_json, at_millis, actor) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    int i = 1;
    sqlite3_bind_text (stmt, i++, ev.event_id.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, ev.rollout_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (stmt, i++, ev.stage_index);
    sqlite3_bind_text (stmt, i++, ev.event_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, ev.reason.c_str(),     -1, SQLITE_TRANSIENT);
    if (ev.metrics_json.empty()) {
        sqlite3_bind_null(stmt, i++);
    } else {
        sqlite3_bind_blob(stmt, i++, ev.metrics_json.data(),
                          static_cast<int>(ev.metrics_json.size()),
                          SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(stmt, i++, ev.at_millis);
    sqlite3_bind_text (stmt, i++, ev.actor.c_str(),      -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<RolloutStageEvent>
SQLitePersistentStore::listRolloutStageEvents(const std::string& rollout_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RolloutStageEvent> out;
    if (!db_) return out;
    const char* sql =
        "SELECT event_id, rollout_id, stage_index, event_type, reason, "
        "metrics_json, at_millis, actor FROM rollout_stage_events "
        "WHERE rollout_id = ? ORDER BY at_millis ASC, event_id ASC";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(stmt, 1, rollout_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RolloutStageEvent e{};
        e.event_id    = safeColumnText(stmt, 0);
        e.rollout_id  = safeColumnText(stmt, 1);
        e.stage_index = sqlite3_column_int(stmt, 2);
        e.event_type  = safeColumnText(stmt, 3);
        e.reason      = safeColumnText(stmt, 4);
        const void* blob = sqlite3_column_blob(stmt, 5);
        int blob_bytes   = sqlite3_column_bytes(stmt, 5);
        if (blob && blob_bytes > 0) {
            e.metrics_json.assign(static_cast<const char*>(blob),
                                  static_cast<size_t>(blob_bytes));
        }
        e.at_millis = sqlite3_column_int64(stmt, 6);
        e.actor     = safeColumnText(stmt, 7);
        out.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    return out;
}

// =========================================================================
// Phase 11.5 AutonomyApprovalWorkflow — TASK-20260518-02 Epic 1.0
// =========================================================================

namespace {

constexpr const char* kApprovalCols =
    "id, source, subject, payload_json, decision_trace_json, "
    "proposed_at_ms, proposer_user_id, state, reviewer_user_id, "
    "reviewed_at_ms, reject_reason, payload_sha256";

ApprovalProposalRecord rowToApprovalProposal(sqlite3_stmt* stmt) {
    ApprovalProposalRecord r;
    auto col = [&](int i) -> std::string {
        auto p = sqlite3_column_text(stmt, i);
        return p ? reinterpret_cast<const char*>(p) : "";
    };
    r.id                  = col(0);
    r.source              = col(1);
    r.subject             = col(2);
    r.payload_json        = col(3);
    r.decision_trace_json = col(4);
    r.proposed_at_ms      = sqlite3_column_int64(stmt, 5);
    r.proposer_user_id    = col(6);
    r.state               = col(7);
    r.reviewer_user_id    = col(8);
    r.reviewed_at_ms      = sqlite3_column_int64(stmt, 9);
    r.reject_reason       = col(10);
    r.payload_sha256      = col(11);
    return r;
}

void bindApprovalProposal(sqlite3_stmt* stmt, const ApprovalProposalRecord& r) {
    int i = 1;
    sqlite3_bind_text (stmt, i++, r.id.c_str(),                  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, r.source.c_str(),              -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, r.subject.c_str(),             -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, r.payload_json.c_str(),        -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, r.decision_trace_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, i++, r.proposed_at_ms);
    sqlite3_bind_text (stmt, i++, r.proposer_user_id.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, r.state.c_str(),               -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, r.reviewer_user_id.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, i++, r.reviewed_at_ms);
    sqlite3_bind_text (stmt, i++, r.reject_reason.c_str(),       -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, r.payload_sha256.c_str(),      -1, SQLITE_TRANSIENT);
}

} // namespace

bool SQLitePersistentStore::insertApprovalProposal(
    const ApprovalProposalRecord& rec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    if (rec.id.empty()) return false;

    const std::string sql =
        std::string("INSERT INTO autonomy_proposals (") + kApprovalCols +
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    bindApprovalProposal(stmt, rec);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<ApprovalProposalRecord>
SQLitePersistentStore::getApprovalProposal(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    const std::string sql = std::string("SELECT ") + kApprovalCols +
                            " FROM autonomy_proposals WHERE id = ? LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<ApprovalProposalRecord> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) out = rowToApprovalProposal(stmt);
    sqlite3_finalize(stmt);
    return out;
}

bool SQLitePersistentStore::updateApprovalProposal(
    const ApprovalProposalRecord& rec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    // Existence check first; UPDATE ... WHERE id=? returns SQLITE_DONE for
    // zero rows too. Mirrors updateRollout semantics.
    const char* count_sql =
        "SELECT COUNT(*) FROM autonomy_proposals WHERE id = ?";
    sqlite3_stmt* cs = nullptr;
    if (sqlite3_prepare_v2(db_, count_sql, -1, &cs, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(cs, 1, rec.id.c_str(), -1, SQLITE_TRANSIENT);
    bool exists =
        sqlite3_step(cs) == SQLITE_ROW && sqlite3_column_int(cs, 0) > 0;
    sqlite3_finalize(cs);
    if (!exists) return false;

    const char* sql =
        "UPDATE autonomy_proposals SET "
        "source = ?, subject = ?, payload_json = ?, decision_trace_json = ?, "
        "proposed_at_ms = ?, proposer_user_id = ?, state = ?, "
        "reviewer_user_id = ?, reviewed_at_ms = ?, reject_reason = ?, "
        "payload_sha256 = ? "
        "WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    int i = 1;
    sqlite3_bind_text (stmt, i++, rec.source.c_str(),              -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, rec.subject.c_str(),             -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, rec.payload_json.c_str(),        -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, rec.decision_trace_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, i++, rec.proposed_at_ms);
    sqlite3_bind_text (stmt, i++, rec.proposer_user_id.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, rec.state.c_str(),               -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, rec.reviewer_user_id.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, i++, rec.reviewed_at_ms);
    sqlite3_bind_text (stmt, i++, rec.reject_reason.c_str(),       -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, rec.payload_sha256.c_str(),      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, rec.id.c_str(),                  -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<ApprovalProposalRecord>
SQLitePersistentStore::listApprovalProposals(const ApprovalProposalQuery& q) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ApprovalProposalRecord> out;
    if (!db_) return out;

    std::string sql = std::string("SELECT ") + kApprovalCols +
                      " FROM autonomy_proposals WHERE 1=1";
    if (!q.state_filter.empty())  sql += " AND state = ?";
    if (!q.source_filter.empty()) sql += " AND source = ?";
    sql += " ORDER BY proposed_at_ms DESC, id DESC LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    int i = 1;
    if (!q.state_filter.empty()) {
        sqlite3_bind_text(stmt, i++, q.state_filter.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (!q.source_filter.empty()) {
        sqlite3_bind_text(stmt, i++, q.source_filter.c_str(), -1, SQLITE_TRANSIENT);
    }
    int limit  = q.limit  > 0 ? q.limit  : 1000;
    int offset = q.offset > 0 ? q.offset : 0;
    sqlite3_bind_int(stmt, i++, limit);
    sqlite3_bind_int(stmt, i++, offset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(rowToApprovalProposal(stmt));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::int64_t SQLitePersistentStore::pruneApprovalProposals(int retention_days) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || retention_days <= 0) return 0;

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::int64_t cutoff_ms = now_ms -
        static_cast<std::int64_t>(retention_days) * 86400LL * 1000LL;

    const char* sql =
        "DELETE FROM autonomy_proposals WHERE proposed_at_ms < ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, cutoff_ms);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return 0;
    return static_cast<std::int64_t>(sqlite3_changes(db_));
}

} // namespace aegisgate
