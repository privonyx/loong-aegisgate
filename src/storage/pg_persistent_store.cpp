#include "storage/pg_persistent_store.h"
#include "storage/json_helpers.h"
#include "storage/rollout_schema.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <ctime>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace aegisgate {

namespace {

std::string pruneCutoffIsoUtc(int retention_days) {
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(24 * retention_days);
    auto tt = std::chrono::system_clock::to_time_t(cutoff);
    struct tm buf{};
    gmtime_r(&tt, &buf);
    std::ostringstream oss;
    oss << std::put_time(&buf, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

} // namespace

PgPersistentStore::PgPersistentStore(const PgConfig& config)
    : config_(config) {}

PgPersistentStore::~PgPersistentStore() {
    close();
}

PGconn* PgPersistentStore::createConnection() {
    PGconn* conn = PQconnectdb(config_.url.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        spdlog::error("PG connect error: {}", PQerrorMessage(conn));
        PQfinish(conn);
        return nullptr;
    }
    return conn;
}

void PgPersistentStore::destroyConnection(PGconn* conn) {
    if (conn) PQfinish(conn);
}

bool PgPersistentStore::checkConnection(PGconn* conn) {
    if (!conn || PQstatus(conn) != CONNECTION_OK) return false;
    PGresult* res = PQexec(conn, "SELECT 1");
    bool ok = (PQresultStatus(res) == PGRES_TUPLES_OK);
    PQclear(res);
    return ok;
}

bool PgPersistentStore::createTables() {
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS audits (
            id SERIAL PRIMARY KEY,
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

        CREATE TABLE IF NOT EXISTS cost_records (
            id SERIAL PRIMARY KEY,
            request_id TEXT NOT NULL,
            timestamp TEXT NOT NULL,
            tenant_id TEXT NOT NULL DEFAULT '',
            app_id TEXT NOT NULL DEFAULT '',
            model TEXT NOT NULL DEFAULT '',
            input_tokens INTEGER NOT NULL DEFAULT 0,
            output_tokens INTEGER NOT NULL DEFAULT 0,
            input_cost REAL NOT NULL DEFAULT 0.0,
            output_cost REAL NOT NULL DEFAULT 0.0,
            total_cost REAL NOT NULL DEFAULT 0.0,
            modality TEXT NOT NULL DEFAULT 'chat',
            baseline_cost REAL NOT NULL DEFAULT 0.0,
            routing_decision_reason TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_costs_model ON cost_records(model);
        CREATE INDEX IF NOT EXISTS idx_costs_timestamp ON cost_records(timestamp);

        CREATE TABLE IF NOT EXISTS tenants (
            id TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            status TEXT NOT NULL DEFAULT 'active',
            model_whitelist TEXT NOT NULL DEFAULT '[]',
            daily_cost_limit DOUBLE PRECISION NOT NULL DEFAULT -1,
            monthly_cost_limit DOUBLE PRECISION NOT NULL DEFAULT -1,
            rate_limit_tokens INTEGER NOT NULL DEFAULT -1,
            rate_limit_refill DOUBLE PRECISION NOT NULL DEFAULT -1,
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS users (
            id TEXT PRIMARY KEY,
            tenant_id TEXT NOT NULL,
            username TEXT NOT NULL,
            display_name TEXT NOT NULL DEFAULT '',
            role TEXT NOT NULL DEFAULT 'viewer',
            status TEXT NOT NULL DEFAULT 'active',
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );
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
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_api_keys_prefix ON api_keys(key_prefix);
        CREATE INDEX IF NOT EXISTS idx_api_keys_hash ON api_keys(key_hash);
        CREATE INDEX IF NOT EXISTS idx_api_keys_tenant ON api_keys(tenant_id);

        CREATE TABLE IF NOT EXISTS sso_providers (
            id TEXT PRIMARY KEY,
            tenant_id TEXT NOT NULL REFERENCES tenants(id),
            name TEXT NOT NULL DEFAULT '',
            issuer_url TEXT NOT NULL DEFAULT '',
            client_id TEXT NOT NULL DEFAULT '',
            client_secret_enc TEXT NOT NULL DEFAULT '',
            redirect_uri TEXT NOT NULL DEFAULT '',
            scopes_json TEXT NOT NULL DEFAULT '[]',
            claim_mapping_json TEXT NOT NULL DEFAULT '{}',
            group_role_mapping_json TEXT NOT NULL DEFAULT '{}',
            jit_provisioning BOOLEAN NOT NULL DEFAULT TRUE,
            default_role TEXT NOT NULL DEFAULT 'viewer',
            enabled BOOLEAN NOT NULL DEFAULT TRUE,
            created_at TEXT NOT NULL DEFAULT '',
            updated_at TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_sso_providers_tenant ON sso_providers(tenant_id);

        CREATE TABLE IF NOT EXISTS identity_mappings (
            id TEXT PRIMARY KEY,
            tenant_id TEXT NOT NULL REFERENCES tenants(id),
            external_subject TEXT NOT NULL,
            external_issuer TEXT NOT NULL,
            user_id TEXT NOT NULL REFERENCES users(id),
            email TEXT NOT NULL DEFAULT '',
            last_login_at TEXT NOT NULL DEFAULT '',
            created_at TEXT NOT NULL DEFAULT ''
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
            mfa_verified BOOLEAN NOT NULL DEFAULT FALSE,
            created_at TEXT NOT NULL DEFAULT '',
            last_active_at TEXT NOT NULL DEFAULT '',
            expires_at TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);
        CREATE INDEX IF NOT EXISTS idx_sessions_expires ON sessions(expires_at);

        CREATE TABLE IF NOT EXISTS mfa_secrets (
            user_id TEXT PRIMARY KEY,
            secret_enc TEXT NOT NULL DEFAULT '',
            enabled BOOLEAN NOT NULL DEFAULT FALSE,
            recovery_codes_enc TEXT NOT NULL DEFAULT '[]',
            created_at TEXT NOT NULL DEFAULT ''
        );

        CREATE TABLE IF NOT EXISTS mfa_failures (
            user_id TEXT PRIMARY KEY,
            fail_count INTEGER NOT NULL DEFAULT 0,
            first_fail_at BIGINT NOT NULL DEFAULT 0,
            locked_until BIGINT NOT NULL DEFAULT 0
        );

        CREATE TABLE IF NOT EXISTS scim_tokens (
            id TEXT PRIMARY KEY,
            tenant_id TEXT NOT NULL REFERENCES tenants(id),
            token_hash TEXT NOT NULL,
            description TEXT NOT NULL DEFAULT '',
            created_at TEXT NOT NULL DEFAULT '',
            expires_at TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_scim_tokens_hash ON scim_tokens(token_hash);
        CREATE INDEX IF NOT EXISTS idx_scim_tokens_tenant ON scim_tokens(tenant_id);

        CREATE TABLE IF NOT EXISTS scim_groups (
            id TEXT PRIMARY KEY,
            tenant_id TEXT NOT NULL,
            display_name TEXT NOT NULL,
            created_at TIMESTAMPTZ DEFAULT NOW(),
            updated_at TIMESTAMPTZ DEFAULT NOW()
        );
        CREATE INDEX IF NOT EXISTS idx_scim_groups_tenant ON scim_groups(tenant_id);

        CREATE TABLE IF NOT EXISTS scim_group_members (
            group_id TEXT NOT NULL REFERENCES scim_groups(id) ON DELETE CASCADE,
            user_id TEXT NOT NULL,
            PRIMARY KEY (group_id, user_id)
        );
    )";

    PGresult* res = PQexec(handle->get(), sql);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        spdlog::error("PG create tables failed: {}", PQresultErrorMessage(res));
    }
    PQclear(res);
    if (!ok) return false;

    // P1-6: idempotent migration for pre-existing cost_records tables. PG
    // supports ADD COLUMN IF NOT EXISTS natively, so this is a no-op on new
    // DBs (already created with the columns above) and safely backfills old
    // ones with the schema defaults.
    const char* cost_migrate_sql = R"(
        ALTER TABLE cost_records ADD COLUMN IF NOT EXISTS modality TEXT NOT NULL DEFAULT 'chat';
        ALTER TABLE cost_records ADD COLUMN IF NOT EXISTS baseline_cost REAL NOT NULL DEFAULT 0.0;
        ALTER TABLE cost_records ADD COLUMN IF NOT EXISTS routing_decision_reason TEXT NOT NULL DEFAULT '';
    )";
    res = PQexec(handle->get(), cost_migrate_sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        spdlog::error("PG cost_records P1-6 migration failed: {}",
                      PQresultErrorMessage(res));
        PQclear(res);
        return false;
    }
    PQclear(res);

    // Phase 9.3 control plane schema — isolated from the existing
    // createTables string so we can evolve it without risking a mis-merge
    // with the much larger SSO/RBAC block above.
    const char* cp_sql = R"(
        CREATE TABLE IF NOT EXISTS config_versions (
            version_id         TEXT PRIMARY KEY,
            content_sha256     TEXT NOT NULL DEFAULT '',
            yaml_content       BYTEA NOT NULL,
            size_bytes         BIGINT NOT NULL DEFAULT 0,
            status             TEXT NOT NULL DEFAULT 'PENDING',
            submitter          TEXT NOT NULL DEFAULT '',
            submitter_comment  TEXT NOT NULL DEFAULT '',
            submitted_at       BIGINT NOT NULL DEFAULT 0,
            reviewer           TEXT NOT NULL DEFAULT '',
            reviewer_comment   TEXT NOT NULL DEFAULT '',
            reviewed_at        BIGINT NOT NULL DEFAULT 0,
            activator          TEXT NOT NULL DEFAULT '',
            activated_at       BIGINT NOT NULL DEFAULT 0,
            deactivated_at     BIGINT NOT NULL DEFAULT 0,
            chain_hash         TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_config_versions_submitted
            ON config_versions(submitted_at DESC);
        CREATE INDEX IF NOT EXISTS idx_config_versions_status
            ON config_versions(status);
        -- SR8 defense-in-depth: at-most-one ACTIVE enforced at schema level.
        CREATE UNIQUE INDEX IF NOT EXISTS uq_config_versions_active
            ON config_versions(status) WHERE status = 'ACTIVE';
    )";
    res = PQexec(handle->get(), cp_sql);
    ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        spdlog::error("PG control plane create tables failed: {}",
                      PQresultErrorMessage(res));
    }
    PQclear(res);
    if (!ok) return false;

    // Phase 9.3.4 RolloutController (TASK-20260507-01) — PG rollout schema.
    // Mirrors SQLite kRolloutsSchemaSql with three PG-specific adaptations:
    // 1. BLOB → BYTEA for spec_json / metrics_json
    // 2. millisecond timestamps INTEGER → BIGINT (SQLite's INTEGER is 64-bit
    //    by default, PG INTEGER is 32-bit so we pick BIGINT explicitly)
    // 3. partial UNIQUE INDEX kept verbatim (PG 7.0+ supports identical
    //    syntax — this is the SR14 defense-in-depth invariant)
    //
    // Idempotent (IF NOT EXISTS on every DDL) so that initialize() can be
    // safely re-run on every process start.
    const char* rollout_sql = R"(
        CREATE TABLE IF NOT EXISTS rollouts (
            rollout_id                 TEXT    PRIMARY KEY,
            target_version_id          TEXT    NOT NULL,
            previous_active_version_id TEXT    NOT NULL DEFAULT '',
            spec_json                  BYTEA   NOT NULL,
            status                     INTEGER NOT NULL,
            current_stage_index        INTEGER NOT NULL DEFAULT 0,
            started_at                 BIGINT  NOT NULL DEFAULT 0,
            stage_started_at           BIGINT  NOT NULL DEFAULT 0,
            paused_at                  BIGINT  NOT NULL DEFAULT 0,
            pause_reason               INTEGER NOT NULL DEFAULT 0,
            pause_detail               TEXT    NOT NULL DEFAULT '',
            creator                    TEXT    NOT NULL,
            last_actor                 TEXT    NOT NULL DEFAULT '',
            completed_at               BIGINT  NOT NULL DEFAULT 0,
            chain_hash                 TEXT    NOT NULL DEFAULT ''
        );
        CREATE UNIQUE INDEX IF NOT EXISTS rollouts_one_active_per_target
            ON rollouts(target_version_id)
            WHERE status IN (1, 2, 3);
        CREATE INDEX IF NOT EXISTS rollouts_started_at_idx
            ON rollouts(started_at DESC);
        CREATE TABLE IF NOT EXISTS rollout_stage_events (
            event_id     TEXT    PRIMARY KEY,
            rollout_id   TEXT    NOT NULL,
            stage_index  INTEGER NOT NULL,
            event_type   TEXT    NOT NULL,
            reason       TEXT    NOT NULL DEFAULT '',
            metrics_json BYTEA,
            at_millis    BIGINT  NOT NULL,
            actor        TEXT    NOT NULL
        );
        CREATE INDEX IF NOT EXISTS rollout_stage_events_by_rollout
            ON rollout_stage_events(rollout_id, at_millis);
    )";
    res = PQexec(handle->get(), rollout_sql);
    ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        spdlog::error("PG rollout schema migration failed: {}",
                      PQresultErrorMessage(res));
    }
    PQclear(res);
    if (!ok) return ok;

    // Phase 11.5 AutonomyApprovalWorkflow (TASK-20260518-02 Epic 1.0).
    // Mirrors SQLite schema (approval_proposal_schema.h) but with BIGINT for
    // millisecond columns.
    const char* approval_sql = R"(
        CREATE TABLE IF NOT EXISTS autonomy_proposals (
            id                    TEXT    PRIMARY KEY,
            source                TEXT    NOT NULL,
            subject               TEXT    NOT NULL DEFAULT '',
            payload_json          TEXT    NOT NULL DEFAULT '{}',
            decision_trace_json   TEXT    NOT NULL DEFAULT '{}',
            proposed_at_ms        BIGINT  NOT NULL,
            proposer_user_id      TEXT    NOT NULL DEFAULT 'system',
            state                 TEXT    NOT NULL DEFAULT 'PROPOSED',
            reviewer_user_id      TEXT    NOT NULL DEFAULT '',
            reviewed_at_ms        BIGINT  NOT NULL DEFAULT 0,
            reject_reason         TEXT    NOT NULL DEFAULT '',
            payload_sha256        TEXT    NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS autonomy_proposals_state_idx
            ON autonomy_proposals(state, proposed_at_ms DESC);
        CREATE INDEX IF NOT EXISTS autonomy_proposals_source_idx
            ON autonomy_proposals(source, proposed_at_ms DESC);
        CREATE INDEX IF NOT EXISTS autonomy_proposals_proposed_at_idx
            ON autonomy_proposals(proposed_at_ms DESC);
    )";
    res = PQexec(handle->get(), approval_sql);
    ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        spdlog::error("PG autonomy_proposals schema migration failed: {}",
                      PQresultErrorMessage(res));
    }
    PQclear(res);
    if (!ok) return ok;

    // TASK-20260604-01 P0-A — Prompt Template + Rule Set（镜像 SQLite schema，
    // INTEGER bool → 保持 INTEGER 以与 SQLite 行为完全一致；IF NOT EXISTS 幂等）。
    const char* tpl_rule_sql = R"(
        CREATE TABLE IF NOT EXISTS prompt_templates (
            id          TEXT    PRIMARY KEY,
            tenant_id   TEXT    NOT NULL,
            name        TEXT    NOT NULL,
            content     TEXT    NOT NULL,
            version     INTEGER NOT NULL DEFAULT 1,
            weight      INTEGER NOT NULL DEFAULT 100,
            is_active   INTEGER NOT NULL DEFAULT 1,
            created_at  TEXT    NOT NULL DEFAULT '',
            updated_at  TEXT    NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_prompt_templates_tenant
            ON prompt_templates(tenant_id);
        CREATE INDEX IF NOT EXISTS idx_prompt_templates_name
            ON prompt_templates(tenant_id, name);

        CREATE TABLE IF NOT EXISTS rule_sets (
            tenant_id   TEXT    NOT NULL,
            version     BIGINT  NOT NULL,
            rules_json  TEXT    NOT NULL,
            created_at  TEXT    NOT NULL DEFAULT '',
            is_active   INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (tenant_id, version)
        );
        CREATE INDEX IF NOT EXISTS idx_rule_sets_active
            ON rule_sets(tenant_id, is_active);

        -- TASK-20260702-01 P1-5: durable savings events (镜像 SQLite schema)。
        -- 仅数值/类别字段落盘（SR2: 不落原始 prompt/response）。timestamp 存 TEXT
        -- 以与 SQLite / prune 的 ISO 字符串比较行为完全一致。
        CREATE TABLE IF NOT EXISTS savings_events (
            id               BIGSERIAL PRIMARY KEY,
            type             INTEGER NOT NULL DEFAULT 0,
            model            TEXT    NOT NULL DEFAULT '',
            tenant_id        TEXT    NOT NULL DEFAULT '',
            tokens_saved     INTEGER NOT NULL DEFAULT 0,
            cost_saved       DOUBLE PRECISION NOT NULL DEFAULT 0.0,
            fallback_pricing INTEGER NOT NULL DEFAULT 0,
            timestamp        TEXT    NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_savings_timestamp
            ON savings_events(timestamp);
    )";
    res = PQexec(handle->get(), tpl_rule_sql);
    ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        spdlog::error("PG prompt_templates/rule_sets schema migration failed: {}",
                      PQresultErrorMessage(res));
    }
    PQclear(res);
    return ok;
}

bool PgPersistentStore::initialize() {
    if (initialized_) return true;
    try {
        auto self = this;
        pool_ = std::make_unique<ConnectionPool<PGconn>>(
            config_.pool_size,
            [self]() { return self->createConnection(); },
            destroyConnection,
            checkConnection);

        if (!createTables()) {
            pool_.reset();
            return false;
        }

        initialized_ = true;
        spdlog::info("PgPersistentStore initialized: pool={}", config_.pool_size);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("PgPersistentStore init failed: {}", e.what());
        return false;
    }
}

void PgPersistentStore::close() {
    pool_.reset();
    initialized_ = false;
}

bool PgPersistentStore::isHealthy() const {
    return initialized_ && pool_ && pool_->isHealthy();
}

bool PgPersistentStore::isReady() const {
    // P1-B: actively validate a pooled connection (SELECT 1) so a DB outage
    // after startup surfaces in /health/ready instead of being masked.
    return initialized_ && pool_ && pool_->activeHealthCheck();
}

std::string PgPersistentStore::backendName() const {
    return "postgres";
}

bool PgPersistentStore::insertAudit(const AuditEntry& entry) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql =
        "INSERT INTO audits (request_id, timestamp, tenant_id, action, "
        "stage_name, detail, input_hash, output_hash, chain_hash) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)";

    const char* params[9] = {
        entry.request_id.c_str(),
        entry.timestamp.c_str(),
        entry.tenant_id.c_str(),
        entry.action.c_str(),
        entry.stage_name.c_str(),
        entry.detail.c_str(),
        entry.input_hash.c_str(),
        entry.output_hash.c_str(),
        entry.chain_hash.c_str()
    };

    PGresult* res = PQexecParams(handle->get(), sql, 9, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
}

std::vector<AuditEntry> PgPersistentStore::queryAudits(
    const std::string& tenant_id, int limit, int offset) {
    std::vector<AuditEntry> results;
    if (!initialized_) return results;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return results;

    std::string limit_str = std::to_string(limit);
    std::string offset_str = std::to_string(offset);

    PGresult* res;
    if (tenant_id.empty()) {
        const char* sql =
            "SELECT request_id, timestamp, tenant_id, action, stage_name, "
            "detail, input_hash, output_hash, chain_hash FROM audits "
            "ORDER BY id DESC LIMIT $1 OFFSET $2";
        const char* params[2] = { limit_str.c_str(), offset_str.c_str() };
        res = PQexecParams(handle->get(), sql, 2, nullptr,
                            params, nullptr, nullptr, 0);
    } else {
        const char* sql =
            "SELECT request_id, timestamp, tenant_id, action, stage_name, "
            "detail, input_hash, output_hash, chain_hash FROM audits "
            "WHERE tenant_id = $1 ORDER BY id DESC LIMIT $2 OFFSET $3";
        const char* params[3] = { tenant_id.c_str(), limit_str.c_str(),
                                   offset_str.c_str() };
        res = PQexecParams(handle->get(), sql, 3, nullptr,
                            params, nullptr, nullptr, 0);
    }

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i) {
            AuditEntry e;
            e.request_id  = PQgetvalue(res, i, 0);
            e.timestamp   = PQgetvalue(res, i, 1);
            e.tenant_id   = PQgetvalue(res, i, 2);
            e.action      = PQgetvalue(res, i, 3);
            e.stage_name  = PQgetvalue(res, i, 4);
            e.detail      = PQgetvalue(res, i, 5);
            e.input_hash  = PQgetvalue(res, i, 6);
            e.output_hash = PQgetvalue(res, i, 7);
            e.chain_hash  = PQgetvalue(res, i, 8);
            results.push_back(std::move(e));
        }
    }
    PQclear(res);
    return results;
}

int64_t PgPersistentStore::auditCount(const std::string& tenant_id) {
    if (!initialized_) return 0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0;

    PGresult* res;
    if (tenant_id.empty()) {
        res = PQexec(handle->get(), "SELECT COUNT(*) FROM audits");
    } else {
        const char* sql = "SELECT COUNT(*) FROM audits WHERE tenant_id = $1";
        const char* params[1] = { tenant_id.c_str() };
        res = PQexecParams(handle->get(), sql, 1, nullptr,
                            params, nullptr, nullptr, 0);
    }

    int64_t count = 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        count = std::stoll(PQgetvalue(res, 0, 0));
    }
    PQclear(res);
    return count;
}

bool PgPersistentStore::insertCostRecord(const CostRecord& record) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql =
        "INSERT INTO cost_records (request_id, timestamp, tenant_id, app_id, "
        "model, input_tokens, output_tokens, input_cost, output_cost, total_cost, "
        "modality, baseline_cost, routing_decision_reason) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13)";

    std::string it = std::to_string(record.input_tokens);
    std::string ot = std::to_string(record.output_tokens);
    std::string ic = std::to_string(record.input_cost);
    std::string oc = std::to_string(record.output_cost);
    std::string tc = std::to_string(record.total_cost);
    std::string bc = std::to_string(record.baseline_cost);

    const char* params[13] = {
        record.request_id.c_str(),
        record.timestamp.c_str(),
        record.tenant_id.c_str(),
        record.app_id.c_str(),
        record.model.c_str(),
        it.c_str(), ot.c_str(), ic.c_str(), oc.c_str(), tc.c_str(),
        record.modality.c_str(), bc.c_str(),
        record.routing_decision_reason.c_str()
    };

    PGresult* res = PQexecParams(handle->get(), sql, 13, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
}

std::vector<CostRecord> PgPersistentStore::queryCosts(
    const std::string& model, int limit, int offset, const std::string& tenant_id) {
    std::vector<CostRecord> results;
    if (!initialized_) return results;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return results;

    // TASK-20260604-01 P0-E/D1=A：model + tenant 联合过滤（tenant 下沉 DB / 折叠 P1-11）。
    std::string sql =
        "SELECT request_id, timestamp, tenant_id, app_id, model, "
        "input_tokens, output_tokens, input_cost, output_cost, total_cost, "
        "modality, baseline_cost, routing_decision_reason "
        "FROM cost_records";
    std::vector<std::string> param_store;
    std::string where;
    if (!model.empty()) {
        param_store.push_back(model);
        where += (where.empty() ? " WHERE " : " AND ") + std::string("model = $") +
                 std::to_string(param_store.size());
    }
    if (!tenant_id.empty()) {
        param_store.push_back(tenant_id);
        where += (where.empty() ? " WHERE " : " AND ") + std::string("tenant_id = $") +
                 std::to_string(param_store.size());
    }
    sql += where + " ORDER BY id DESC LIMIT $" + std::to_string(param_store.size() + 1) +
           " OFFSET $" + std::to_string(param_store.size() + 2);
    param_store.push_back(std::to_string(limit));
    param_store.push_back(std::to_string(offset));

    std::vector<const char*> params;
    params.reserve(param_store.size());
    for (const auto& p : param_store) params.push_back(p.c_str());

    PGresult* res = PQexecParams(handle->get(), sql.c_str(),
                                 static_cast<int>(params.size()), nullptr,
                                 params.data(), nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i) {
            CostRecord r;
            r.request_id = PQgetvalue(res, i, 0);
            r.timestamp = PQgetvalue(res, i, 1);
            r.tenant_id = PQgetvalue(res, i, 2);
            r.app_id = PQgetvalue(res, i, 3);
            r.model = PQgetvalue(res, i, 4);
            r.input_tokens = std::stoi(PQgetvalue(res, i, 5));
            r.output_tokens = std::stoi(PQgetvalue(res, i, 6));
            r.input_cost = std::stod(PQgetvalue(res, i, 7));
            r.output_cost = std::stod(PQgetvalue(res, i, 8));
            r.total_cost = std::stod(PQgetvalue(res, i, 9));
            r.modality = PQgetvalue(res, i, 10);
            r.baseline_cost = std::stod(PQgetvalue(res, i, 11));
            r.routing_decision_reason = PQgetvalue(res, i, 12);
            results.push_back(std::move(r));
        }
    }
    PQclear(res);
    return results;
}

int64_t PgPersistentStore::costRecordCount(const std::string& model,
                                           const std::string& tenant_id) {
    if (!initialized_) return 0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0;

    std::string sql = "SELECT COUNT(*) FROM cost_records";
    std::vector<std::string> param_store;
    std::string where;
    if (!model.empty()) {
        param_store.push_back(model);
        where += (where.empty() ? " WHERE " : " AND ") + std::string("model = $") +
                 std::to_string(param_store.size());
    }
    if (!tenant_id.empty()) {
        param_store.push_back(tenant_id);
        where += (where.empty() ? " WHERE " : " AND ") + std::string("tenant_id = $") +
                 std::to_string(param_store.size());
    }
    sql += where;

    std::vector<const char*> params;
    for (const auto& p : param_store) params.push_back(p.c_str());

    PGresult* res = PQexecParams(handle->get(), sql.c_str(),
                                 static_cast<int>(params.size()), nullptr,
                                 params.empty() ? nullptr : params.data(),
                                 nullptr, nullptr, 0);

    int64_t count = 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        count = std::stoll(PQgetvalue(res, 0, 0));
    }
    PQclear(res);
    return count;
}

// TASK-20260702-01 P1-2 — total_cost DB 级 SUM（无 10k 截断）。tenant 空 = 全局。
double PgPersistentStore::costTotal(const std::string& tenant_id) {
    if (!initialized_) return 0.0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0.0;

    std::string sql = "SELECT COALESCE(SUM(total_cost), 0) FROM cost_records";
    PGresult* res;
    if (tenant_id.empty()) {
        res = PQexec(handle->get(), sql.c_str());
    } else {
        sql += " WHERE tenant_id = $1";
        const char* params[1] = {tenant_id.c_str()};
        res = PQexecParams(handle->get(), sql.c_str(), 1, nullptr, params,
                           nullptr, nullptr, 0);
    }

    double total = 0.0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        total = std::stod(PQgetvalue(res, 0, 0));
    }
    PQclear(res);
    return total;
}

// TASK-20260604-01 P0-E/D1=A — 分页总数。tenant 为空 = 全局（SR-3）。
namespace {
int64_t pgCountWhereTenant(PGconn* conn, const char* table,
                           const std::string& tenant_id) {
    std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
    PGresult* res;
    if (tenant_id.empty()) {
        res = PQexec(conn, sql.c_str());
    } else {
        sql += " WHERE tenant_id = $1";
        const char* params[1] = { tenant_id.c_str() };
        res = PQexecParams(conn, sql.c_str(), 1, nullptr, params, nullptr, nullptr, 0);
    }
    int64_t count = 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
        count = std::stoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    return count;
}
} // namespace

int64_t PgPersistentStore::tenantCount() {
    if (!initialized_) return 0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0;
    return pgCountWhereTenant(handle->get(), "tenants", "");
}

int64_t PgPersistentStore::userCount(const std::string& tenant_id) {
    if (!initialized_) return 0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0;
    return pgCountWhereTenant(handle->get(), "users", tenant_id);
}

int64_t PgPersistentStore::apiKeyCount(const std::string& tenant_id) {
    if (!initialized_) return 0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0;
    return pgCountWhereTenant(handle->get(), "api_keys", tenant_id);
}

int64_t PgPersistentStore::promptTemplateCount(const std::string& tenant_id) {
    if (!initialized_) return 0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0;
    return pgCountWhereTenant(handle->get(), "prompt_templates", tenant_id);
}

int64_t PgPersistentStore::ruleSetCount(const std::string& tenant_id) {
    if (!initialized_) return 0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0;
    return pgCountWhereTenant(handle->get(), "rule_sets", tenant_id);
}

int64_t PgPersistentStore::pruneAudits(int retention_days) {
    if (!initialized_ || retention_days <= 0) return 0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0;

    std::string cutoff = pruneCutoffIsoUtc(retention_days);
    const char* sql = "DELETE FROM audits WHERE timestamp < $1";
    const char* params[1] = { cutoff.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    int64_t deleted = 0;
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
        const char* affected = PQcmdTuples(res);
        if (affected && affected[0]) deleted = std::stoll(affected);
    }
    PQclear(res);
    return deleted;
}

int64_t PgPersistentStore::pruneCostRecords(int retention_days) {
    if (!initialized_ || retention_days <= 0) return 0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0;

    std::string cutoff = pruneCutoffIsoUtc(retention_days);
    const char* sql = "DELETE FROM cost_records WHERE timestamp < $1";
    const char* params[1] = { cutoff.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    int64_t deleted = 0;
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
        const char* affected = PQcmdTuples(res);
        if (affected && affected[0]) deleted = std::stoll(affected);
    }
    PQclear(res);
    return deleted;
}

// --- Savings events (TASK-20260702-01 P1-5，镜像 SQLite) ---

bool PgPersistentStore::insertSavingsEvent(const SavingsEventRecord& ev) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql =
        "INSERT INTO savings_events (type, model, tenant_id, tokens_saved, "
        "cost_saved, fallback_pricing, timestamp) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7)";

    std::string type_s = std::to_string(ev.type);
    std::string tokens_s = std::to_string(ev.tokens_saved);
    std::string cost_s = std::to_string(ev.cost_saved);
    std::string fb_s = ev.fallback_pricing ? "1" : "0";

    const char* params[7] = {
        type_s.c_str(), ev.model.c_str(), ev.tenant_id.c_str(),
        tokens_s.c_str(), cost_s.c_str(), fb_s.c_str(), ev.timestamp.c_str()
    };

    PGresult* res = PQexecParams(handle->get(), sql, 7, nullptr,
                                 params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
}

std::vector<PersistentStore::SavingsEventRecord>
PgPersistentStore::querySavingsEventsByDateRange(
    const std::string& from_iso, const std::string& to_iso, int limit) {
    std::vector<SavingsEventRecord> result;
    if (!initialized_) return result;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return result;

    // 空 bound = 该侧开区间（与 PersistentStore 契约 / SQLite 一致）。
    // limit < 0 → 无上限（PG LIMIT ALL）。
    std::string sql =
        "SELECT type, model, tenant_id, tokens_saved, cost_saved, "
        "fallback_pricing, timestamp FROM savings_events "
        "WHERE ($1 = '' OR timestamp >= $1) AND ($2 = '' OR timestamp <= $2) "
        "ORDER BY timestamp ASC LIMIT ";
    sql += (limit < 0 ? std::string("ALL") : std::to_string(limit));

    const char* params[2] = { from_iso.c_str(), to_iso.c_str() };
    PGresult* res = PQexecParams(handle->get(), sql.c_str(), 2, nullptr,
                                 params, nullptr, nullptr, 0);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i) {
            SavingsEventRecord ev;
            ev.type = std::stoi(PQgetvalue(res, i, 0));
            ev.model = PQgetvalue(res, i, 1);
            ev.tenant_id = PQgetvalue(res, i, 2);
            ev.tokens_saved = std::stoi(PQgetvalue(res, i, 3));
            ev.cost_saved = std::stod(PQgetvalue(res, i, 4));
            ev.fallback_pricing = std::stoi(PQgetvalue(res, i, 5)) != 0;
            ev.timestamp = PQgetvalue(res, i, 6);
            result.push_back(std::move(ev));
        }
    }
    PQclear(res);
    return result;
}

int64_t PgPersistentStore::savingsEventCount() {
    if (!initialized_) return 0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0;

    PGresult* res = PQexec(handle->get(), "SELECT COUNT(*) FROM savings_events");
    int64_t count = 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        count = std::stoll(PQgetvalue(res, 0, 0));
    }
    PQclear(res);
    return count;
}

int64_t PgPersistentStore::pruneSavingsEvents(int retention_days) {
    if (!initialized_ || retention_days <= 0) return 0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0;

    std::string cutoff = pruneCutoffIsoUtc(retention_days);
    const char* sql = "DELETE FROM savings_events WHERE timestamp < $1";
    const char* params[1] = { cutoff.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                 params, nullptr, nullptr, 0);
    int64_t deleted = 0;
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
        const char* affected = PQcmdTuples(res);
        if (affected && affected[0]) deleted = std::stoll(affected);
    }
    PQclear(res);
    return deleted;
}

namespace {

Tenant tenantFromPgResult(PGresult* res, int row) {
    Tenant t;
    t.id                 = PQgetvalue(res, row, 0);
    t.name               = PQgetvalue(res, row, 1);
    t.status             = PQgetvalue(res, row, 2);
    t.model_whitelist    = parseStringList(PQgetvalue(res, row, 3));
    t.daily_cost_limit   = std::stod(PQgetvalue(res, row, 4));
    t.monthly_cost_limit = std::stod(PQgetvalue(res, row, 5));
    t.rate_limit_tokens  = std::stoi(PQgetvalue(res, row, 6));
    t.rate_limit_refill  = std::stod(PQgetvalue(res, row, 7));
    t.created_at         = PQgetvalue(res, row, 8);
    t.updated_at         = PQgetvalue(res, row, 9);
    return t;
}

User userFromPgResult(PGresult* res, int row) {
    User u;
    u.id           = PQgetvalue(res, row, 0);
    u.tenant_id    = PQgetvalue(res, row, 1);
    u.username     = PQgetvalue(res, row, 2);
    u.display_name = PQgetvalue(res, row, 3);
    auto role_opt  = roleFromString(PQgetvalue(res, row, 4));
    u.role         = role_opt.value_or(Role::Viewer);
    u.status       = PQgetvalue(res, row, 5);
    u.created_at   = PQgetvalue(res, row, 6);
    u.updated_at   = PQgetvalue(res, row, 7);
    return u;
}

ApiKeyRecord apiKeyFromPgResult(PGresult* res, int row) {
    ApiKeyRecord k;
    k.id           = PQgetvalue(res, row, 0);
    k.user_id      = PQgetvalue(res, row, 1);
    k.tenant_id    = PQgetvalue(res, row, 2);
    k.name         = PQgetvalue(res, row, 3);
    k.key_prefix   = PQgetvalue(res, row, 4);
    k.key_hash     = PQgetvalue(res, row, 5);
    auto role_opt  = roleFromString(PQgetvalue(res, row, 6));
    k.role         = role_opt.value_or(Role::Developer);
    k.status       = PQgetvalue(res, row, 7);
    k.expires_at   = PQgetisnull(res, row, 8) ? "" : PQgetvalue(res, row, 8);
    k.last_used_at = PQgetisnull(res, row, 9) ? "" : PQgetvalue(res, row, 9);
    k.created_at   = PQgetvalue(res, row, 10);
    k.updated_at   = PQgetvalue(res, row, 11);
    return k;
}

} // namespace

// --- RBAC: Tenant ---

bool PgPersistentStore::insertTenant(const Tenant& tenant) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql =
        "INSERT INTO tenants (id, name, status, model_whitelist, daily_cost_limit, "
        "monthly_cost_limit, rate_limit_tokens, rate_limit_refill, created_at, updated_at) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)";

    std::string wl = serializeStringList(tenant.model_whitelist);
    std::string dcl = std::to_string(tenant.daily_cost_limit);
    std::string mcl = std::to_string(tenant.monthly_cost_limit);
    std::string rlt = std::to_string(tenant.rate_limit_tokens);
    std::string rlr = std::to_string(tenant.rate_limit_refill);

    const char* params[10] = {
        tenant.id.c_str(), tenant.name.c_str(), tenant.status.c_str(),
        wl.c_str(), dcl.c_str(), mcl.c_str(), rlt.c_str(), rlr.c_str(),
        tenant.created_at.c_str(), tenant.updated_at.c_str()
    };

    PGresult* res = PQexecParams(handle->get(), sql, 10, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
}

std::optional<Tenant> PgPersistentStore::getTenant(const std::string& id) {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;

    const char* sql =
        "SELECT id, name, status, model_whitelist, daily_cost_limit, "
        "monthly_cost_limit, rate_limit_tokens, rate_limit_refill, created_at, updated_at "
        "FROM tenants WHERE id = $1";
    const char* params[1] = { id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    std::optional<Tenant> result;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        result = tenantFromPgResult(res, 0);
    }
    PQclear(res);
    return result;
}

std::vector<Tenant> PgPersistentStore::listTenants(int limit, int offset) {
    std::vector<Tenant> results;
    if (!initialized_) return results;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return results;

    const char* sql =
        "SELECT id, name, status, model_whitelist, daily_cost_limit, "
        "monthly_cost_limit, rate_limit_tokens, rate_limit_refill, created_at, updated_at "
        "FROM tenants ORDER BY created_at ASC LIMIT $1 OFFSET $2";
    std::string limit_str = std::to_string(limit);
    std::string offset_str = std::to_string(offset);
    const char* params[2] = { limit_str.c_str(), offset_str.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 2, nullptr,
                                  params, nullptr, nullptr, 0);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i) {
            results.push_back(tenantFromPgResult(res, i));
        }
    }
    PQclear(res);
    return results;
}

bool PgPersistentStore::updateTenant(const Tenant& tenant) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql =
        "UPDATE tenants SET name=$1, status=$2, model_whitelist=$3, daily_cost_limit=$4, "
        "monthly_cost_limit=$5, rate_limit_tokens=$6, rate_limit_refill=$7, updated_at=$8 "
        "WHERE id=$9";

    std::string wl = serializeStringList(tenant.model_whitelist);
    std::string dcl = std::to_string(tenant.daily_cost_limit);
    std::string mcl = std::to_string(tenant.monthly_cost_limit);
    std::string rlt = std::to_string(tenant.rate_limit_tokens);
    std::string rlr = std::to_string(tenant.rate_limit_refill);

    const char* params[9] = {
        tenant.name.c_str(), tenant.status.c_str(), wl.c_str(),
        dcl.c_str(), mcl.c_str(), rlt.c_str(), rlr.c_str(),
        tenant.updated_at.c_str(), tenant.id.c_str()
    };

    PGresult* res = PQexecParams(handle->get(), sql, 9, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    const char* affected = ok ? PQcmdTuples(res) : "0";
    bool changed = (affected && affected[0] && std::stoll(affected) > 0);
    PQclear(res);
    return ok && changed;
}

bool PgPersistentStore::deleteTenant(const std::string& id) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql = "DELETE FROM tenants WHERE id = $1";
    const char* params[1] = { id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    const char* affected = ok ? PQcmdTuples(res) : "0";
    bool changed = (affected && affected[0] && std::stoll(affected) > 0);
    PQclear(res);
    return ok && changed;
}

// --- RBAC: User ---

bool PgPersistentStore::insertUser(const User& user) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql =
        "INSERT INTO users (id, tenant_id, username, display_name, role, status, "
        "created_at, updated_at) VALUES ($1, $2, $3, $4, $5, $6, $7, $8)";

    const char* role_str = roleToString(user.role);
    const char* params[8] = {
        user.id.c_str(), user.tenant_id.c_str(), user.username.c_str(),
        user.display_name.c_str(), role_str, user.status.c_str(),
        user.created_at.c_str(), user.updated_at.c_str()
    };

    PGresult* res = PQexecParams(handle->get(), sql, 8, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
}

std::optional<User> PgPersistentStore::getUser(const std::string& id) {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;

    const char* sql =
        "SELECT id, tenant_id, username, display_name, role, status, "
        "created_at, updated_at FROM users WHERE id = $1";
    const char* params[1] = { id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    std::optional<User> result;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        result = userFromPgResult(res, 0);
    }
    PQclear(res);
    return result;
}

std::optional<User> PgPersistentStore::getUserByUsername(
    const std::string& tenant_id, const std::string& username) {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;

    const char* sql =
        "SELECT id, tenant_id, username, display_name, role, status, "
        "created_at, updated_at FROM users WHERE tenant_id = $1 AND username = $2";
    const char* params[2] = { tenant_id.c_str(), username.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 2, nullptr,
                                  params, nullptr, nullptr, 0);
    std::optional<User> result;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        result = userFromPgResult(res, 0);
    }
    PQclear(res);
    return result;
}

std::vector<User> PgPersistentStore::listUsers(
    const std::string& tenant_id, int limit, int offset) {
    std::vector<User> results;
    if (!initialized_) return results;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return results;

    std::string limit_str = std::to_string(limit);
    std::string offset_str = std::to_string(offset);

    PGresult* res;
    if (tenant_id.empty()) {
        const char* sql =
            "SELECT id, tenant_id, username, display_name, role, status, "
            "created_at, updated_at FROM users ORDER BY created_at ASC LIMIT $1 OFFSET $2";
        const char* params[2] = { limit_str.c_str(), offset_str.c_str() };
        res = PQexecParams(handle->get(), sql, 2, nullptr,
                            params, nullptr, nullptr, 0);
    } else {
        const char* sql =
            "SELECT id, tenant_id, username, display_name, role, status, "
            "created_at, updated_at FROM users WHERE tenant_id = $1 "
            "ORDER BY created_at ASC LIMIT $2 OFFSET $3";
        const char* params[3] = { tenant_id.c_str(), limit_str.c_str(), offset_str.c_str() };
        res = PQexecParams(handle->get(), sql, 3, nullptr,
                            params, nullptr, nullptr, 0);
    }

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i) {
            results.push_back(userFromPgResult(res, i));
        }
    }
    PQclear(res);
    return results;
}

bool PgPersistentStore::updateUser(const User& user) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql =
        "UPDATE users SET username=$1, display_name=$2, role=$3, status=$4, "
        "updated_at=$5 WHERE id=$6";

    const char* role_str = roleToString(user.role);
    const char* params[6] = {
        user.username.c_str(), user.display_name.c_str(), role_str,
        user.status.c_str(), user.updated_at.c_str(), user.id.c_str()
    };

    PGresult* res = PQexecParams(handle->get(), sql, 6, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    const char* affected = ok ? PQcmdTuples(res) : "0";
    bool changed = (affected && affected[0] && std::stoll(affected) > 0);
    PQclear(res);
    return ok && changed;
}

bool PgPersistentStore::deleteUser(const std::string& id) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql = "DELETE FROM users WHERE id = $1";
    const char* params[1] = { id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    const char* affected = ok ? PQcmdTuples(res) : "0";
    bool changed = (affected && affected[0] && std::stoll(affected) > 0);
    PQclear(res);
    return ok && changed;
}

// --- RBAC: API Key ---

bool PgPersistentStore::insertApiKey(const ApiKeyRecord& key) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql =
        "INSERT INTO api_keys (id, user_id, tenant_id, name, key_prefix, key_hash, "
        "role, status, expires_at, last_used_at, created_at, updated_at) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)";

    const char* role_str = roleToString(key.role);
    const char* expires = key.expires_at.empty() ? nullptr : key.expires_at.c_str();
    const char* last_used = key.last_used_at.empty() ? nullptr : key.last_used_at.c_str();

    const char* params[12] = {
        key.id.c_str(), key.user_id.c_str(), key.tenant_id.c_str(),
        key.name.c_str(), key.key_prefix.c_str(), key.key_hash.c_str(),
        role_str, key.status.c_str(), expires, last_used,
        key.created_at.c_str(), key.updated_at.c_str()
    };

    PGresult* res = PQexecParams(handle->get(), sql, 12, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
}

std::optional<ApiKeyRecord> PgPersistentStore::getApiKeyByHash(const std::string& key_hash) {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;

    const char* sql =
        "SELECT id, user_id, tenant_id, name, key_prefix, key_hash, "
        "role, status, expires_at, last_used_at, created_at, updated_at "
        "FROM api_keys WHERE key_hash = $1";
    const char* params[1] = { key_hash.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    std::optional<ApiKeyRecord> result;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        result = apiKeyFromPgResult(res, 0);
    }
    PQclear(res);
    return result;
}

std::vector<ApiKeyRecord> PgPersistentStore::getApiKeysByPrefix(const std::string& prefix) {
    std::vector<ApiKeyRecord> results;
    if (!initialized_) return results;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return results;

    const char* sql =
        "SELECT id, user_id, tenant_id, name, key_prefix, key_hash, "
        "role, status, expires_at, last_used_at, created_at, updated_at "
        "FROM api_keys WHERE key_prefix = $1";
    const char* params[1] = { prefix.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i) {
            results.push_back(apiKeyFromPgResult(res, i));
        }
    }
    PQclear(res);
    return results;
}

std::vector<ApiKeyRecord> PgPersistentStore::listApiKeys(
    const std::string& tenant_id, int limit, int offset) {
    std::vector<ApiKeyRecord> results;
    if (!initialized_) return results;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return results;

    std::string limit_str = std::to_string(limit);
    std::string offset_str = std::to_string(offset);

    PGresult* res;
    if (tenant_id.empty()) {
        const char* sql =
            "SELECT id, user_id, tenant_id, name, key_prefix, key_hash, "
            "role, status, expires_at, last_used_at, created_at, updated_at "
            "FROM api_keys ORDER BY created_at ASC LIMIT $1 OFFSET $2";
        const char* params[2] = { limit_str.c_str(), offset_str.c_str() };
        res = PQexecParams(handle->get(), sql, 2, nullptr,
                            params, nullptr, nullptr, 0);
    } else {
        const char* sql =
            "SELECT id, user_id, tenant_id, name, key_prefix, key_hash, "
            "role, status, expires_at, last_used_at, created_at, updated_at "
            "FROM api_keys WHERE tenant_id = $1 ORDER BY created_at ASC LIMIT $2 OFFSET $3";
        const char* params[3] = { tenant_id.c_str(), limit_str.c_str(), offset_str.c_str() };
        res = PQexecParams(handle->get(), sql, 3, nullptr,
                            params, nullptr, nullptr, 0);
    }

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i) {
            results.push_back(apiKeyFromPgResult(res, i));
        }
    }
    PQclear(res);
    return results;
}

bool PgPersistentStore::updateApiKey(const ApiKeyRecord& key) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql =
        "UPDATE api_keys SET name=$1, role=$2, status=$3, expires_at=$4, "
        "last_used_at=$5, updated_at=$6 WHERE id=$7";

    const char* role_str = roleToString(key.role);
    const char* expires = key.expires_at.empty() ? nullptr : key.expires_at.c_str();
    const char* last_used = key.last_used_at.empty() ? nullptr : key.last_used_at.c_str();

    const char* params[7] = {
        key.name.c_str(), role_str, key.status.c_str(),
        expires, last_used, key.updated_at.c_str(), key.id.c_str()
    };

    PGresult* res = PQexecParams(handle->get(), sql, 7, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    const char* affected = ok ? PQcmdTuples(res) : "0";
    bool changed = (affected && affected[0] && std::stoll(affected) > 0);
    PQclear(res);
    return ok && changed;
}

bool PgPersistentStore::revokeApiKey(const std::string& id) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql = "UPDATE api_keys SET status='revoked' WHERE id=$1";
    const char* params[1] = { id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    const char* affected = ok ? PQcmdTuples(res) : "0";
    bool changed = (affected && affected[0] && std::stoll(affected) > 0);
    PQclear(res);
    return ok && changed;
}

// --- Tenant cost aggregation ---

double PgPersistentStore::getTenantCostInPeriod(
    const std::string& tenant_id, const std::string& start, const std::string& end) {
    if (!initialized_) return 0.0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0.0;

    const char* sql =
        "SELECT COALESCE(SUM(total_cost), 0) FROM cost_records "
        "WHERE tenant_id = $1 AND timestamp >= $2 AND timestamp <= $3";
    const char* params[3] = { tenant_id.c_str(), start.c_str(), end.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 3, nullptr,
                                  params, nullptr, nullptr, 0);
    double total = 0.0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        total = std::stod(PQgetvalue(res, 0, 0));
    }
    PQclear(res);
    return total;
}

// --- SSO helper functions ---

namespace {

SsoProvider ssoProviderFromPgResult(PGresult* res, int row) {
    SsoProvider p;
    p.id                     = PQgetvalue(res, row, 0);
    p.tenant_id              = PQgetvalue(res, row, 1);
    p.name                   = PQgetvalue(res, row, 2);
    p.issuer_url             = PQgetvalue(res, row, 3);
    p.client_id              = PQgetvalue(res, row, 4);
    p.client_secret_enc      = PQgetvalue(res, row, 5);
    p.redirect_uri           = PQgetvalue(res, row, 6);
    p.scopes                 = parseStringList(PQgetvalue(res, row, 7));
    p.claim_mapping_json     = PQgetvalue(res, row, 8);
    p.group_role_mapping_json = PQgetvalue(res, row, 9);
    p.jit_provisioning       = std::string(PQgetvalue(res, row, 10)) == "t";
    p.default_role           = PQgetvalue(res, row, 11);
    p.enabled                = std::string(PQgetvalue(res, row, 12)) == "t";
    p.created_at             = PQgetvalue(res, row, 13);
    p.updated_at             = PQgetvalue(res, row, 14);
    return p;
}

IdentityMapping identityMappingFromPgResult(PGresult* res, int row) {
    IdentityMapping m;
    m.id               = PQgetvalue(res, row, 0);
    m.tenant_id        = PQgetvalue(res, row, 1);
    m.external_subject = PQgetvalue(res, row, 2);
    m.external_issuer  = PQgetvalue(res, row, 3);
    m.user_id          = PQgetvalue(res, row, 4);
    m.email            = PQgetvalue(res, row, 5);
    m.last_login_at    = PQgetvalue(res, row, 6);
    m.created_at       = PQgetvalue(res, row, 7);
    return m;
}

Session sessionFromPgResult(PGresult* res, int row) {
    Session s;
    s.id             = PQgetvalue(res, row, 0);
    s.user_id        = PQgetvalue(res, row, 1);
    s.tenant_id      = PQgetvalue(res, row, 2);
    s.ip_address     = PQgetvalue(res, row, 3);
    s.user_agent     = PQgetvalue(res, row, 4);
    s.auth_method    = PQgetvalue(res, row, 5);
    s.mfa_verified   = std::string(PQgetvalue(res, row, 6)) == "t";
    s.created_at     = PQgetvalue(res, row, 7);
    s.last_active_at = PQgetvalue(res, row, 8);
    s.expires_at     = PQgetvalue(res, row, 9);
    return s;
}

MfaSecret mfaSecretFromPgResult(PGresult* res, int row) {
    MfaSecret m;
    m.user_id             = PQgetvalue(res, row, 0);
    m.secret_enc          = PQgetvalue(res, row, 1);
    m.enabled             = std::string(PQgetvalue(res, row, 2)) == "t";
    m.recovery_codes_hash = parseStringList(PQgetvalue(res, row, 3));
    m.created_at          = PQgetvalue(res, row, 4);
    return m;
}

ScimToken scimTokenFromPgResult(PGresult* res, int row) {
    ScimToken t;
    t.id          = PQgetvalue(res, row, 0);
    t.tenant_id   = PQgetvalue(res, row, 1);
    t.token_hash  = PQgetvalue(res, row, 2);
    t.description = PQgetvalue(res, row, 3);
    t.created_at  = PQgetvalue(res, row, 4);
    t.expires_at  = PQgetvalue(res, row, 5);
    return t;
}

} // namespace

// --- SSO Provider ---

bool PgPersistentStore::insertSsoProvider(const SsoProvider& p) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql =
        "INSERT INTO sso_providers (id, tenant_id, name, issuer_url, client_id, "
        "client_secret_enc, redirect_uri, scopes_json, claim_mapping_json, "
        "group_role_mapping_json, jit_provisioning, default_role, enabled, "
        "created_at, updated_at) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15)";

    std::string scopes_json = serializeStringList(p.scopes);
    const char* jit = p.jit_provisioning ? "true" : "false";
    const char* ena = p.enabled ? "true" : "false";

    const char* params[15] = {
        p.id.c_str(), p.tenant_id.c_str(), p.name.c_str(),
        p.issuer_url.c_str(), p.client_id.c_str(), p.client_secret_enc.c_str(),
        p.redirect_uri.c_str(), scopes_json.c_str(), p.claim_mapping_json.c_str(),
        p.group_role_mapping_json.c_str(), jit, p.default_role.c_str(),
        ena, p.created_at.c_str(), p.updated_at.c_str()
    };

    PGresult* res = PQexecParams(handle->get(), sql, 15, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
}

std::optional<SsoProvider> PgPersistentStore::getSsoProvider(const std::string& id) {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;

    const char* sql =
        "SELECT id, tenant_id, name, issuer_url, client_id, client_secret_enc, "
        "redirect_uri, scopes_json, claim_mapping_json, group_role_mapping_json, "
        "jit_provisioning, default_role, enabled, created_at, updated_at "
        "FROM sso_providers WHERE id = $1";
    const char* params[1] = { id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    std::optional<SsoProvider> result;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        result = ssoProviderFromPgResult(res, 0);
    }
    PQclear(res);
    return result;
}

std::optional<SsoProvider> PgPersistentStore::getSsoProviderByTenant(const std::string& tenant_id) {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;

    const char* sql =
        "SELECT id, tenant_id, name, issuer_url, client_id, client_secret_enc, "
        "redirect_uri, scopes_json, claim_mapping_json, group_role_mapping_json, "
        "jit_provisioning, default_role, enabled, created_at, updated_at "
        "FROM sso_providers WHERE tenant_id = $1 AND enabled = true LIMIT 1";
    const char* params[1] = { tenant_id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    std::optional<SsoProvider> result;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        result = ssoProviderFromPgResult(res, 0);
    }
    PQclear(res);
    return result;
}

std::vector<SsoProvider> PgPersistentStore::listSsoProviders(int limit, int offset) {
    std::vector<SsoProvider> results;
    if (!initialized_) return results;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return results;

    const char* sql =
        "SELECT id, tenant_id, name, issuer_url, client_id, client_secret_enc, "
        "redirect_uri, scopes_json, claim_mapping_json, group_role_mapping_json, "
        "jit_provisioning, default_role, enabled, created_at, updated_at "
        "FROM sso_providers LIMIT $1 OFFSET $2";
    std::string limit_str = std::to_string(limit);
    std::string offset_str = std::to_string(offset);
    const char* params[2] = { limit_str.c_str(), offset_str.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 2, nullptr,
                                  params, nullptr, nullptr, 0);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i) {
            results.push_back(ssoProviderFromPgResult(res, i));
        }
    }
    PQclear(res);
    return results;
}

int64_t PgPersistentStore::ssoProviderCount() {
    if (!initialized_) return 0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0;
    return pgCountWhereTenant(handle->get(), "sso_providers", "");
}

bool PgPersistentStore::updateSsoProvider(const SsoProvider& p) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql =
        "UPDATE sso_providers SET tenant_id=$1, name=$2, issuer_url=$3, client_id=$4, "
        "client_secret_enc=$5, redirect_uri=$6, scopes_json=$7, claim_mapping_json=$8, "
        "group_role_mapping_json=$9, jit_provisioning=$10, default_role=$11, enabled=$12, "
        "updated_at=$13 WHERE id=$14";

    std::string scopes_json = serializeStringList(p.scopes);
    const char* jit = p.jit_provisioning ? "true" : "false";
    const char* ena = p.enabled ? "true" : "false";

    const char* params[14] = {
        p.tenant_id.c_str(), p.name.c_str(), p.issuer_url.c_str(),
        p.client_id.c_str(), p.client_secret_enc.c_str(), p.redirect_uri.c_str(),
        scopes_json.c_str(), p.claim_mapping_json.c_str(),
        p.group_role_mapping_json.c_str(), jit, p.default_role.c_str(),
        ena, p.updated_at.c_str(), p.id.c_str()
    };

    PGresult* res = PQexecParams(handle->get(), sql, 14, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    const char* affected = ok ? PQcmdTuples(res) : "0";
    bool changed = (affected && affected[0] && std::stoll(affected) > 0);
    PQclear(res);
    return ok && changed;
}

bool PgPersistentStore::deleteSsoProvider(const std::string& id) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql = "DELETE FROM sso_providers WHERE id = $1";
    const char* params[1] = { id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    const char* affected = ok ? PQcmdTuples(res) : "0";
    bool changed = (affected && affected[0] && std::stoll(affected) > 0);
    PQclear(res);
    return ok && changed;
}

// --- Identity Mapping ---

bool PgPersistentStore::insertIdentityMapping(const IdentityMapping& m) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql =
        "INSERT INTO identity_mappings (id, tenant_id, external_subject, external_issuer, "
        "user_id, email, last_login_at, created_at) VALUES ($1, $2, $3, $4, $5, $6, $7, $8)";

    const char* params[8] = {
        m.id.c_str(), m.tenant_id.c_str(), m.external_subject.c_str(),
        m.external_issuer.c_str(), m.user_id.c_str(), m.email.c_str(),
        m.last_login_at.c_str(), m.created_at.c_str()
    };

    PGresult* res = PQexecParams(handle->get(), sql, 8, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
}

std::optional<IdentityMapping> PgPersistentStore::getIdentityMapping(
    const std::string& external_subject, const std::string& external_issuer) {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;

    const char* sql =
        "SELECT id, tenant_id, external_subject, external_issuer, user_id, email, "
        "last_login_at, created_at FROM identity_mappings "
        "WHERE external_subject = $1 AND external_issuer = $2";
    const char* params[2] = { external_subject.c_str(), external_issuer.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 2, nullptr,
                                  params, nullptr, nullptr, 0);
    std::optional<IdentityMapping> result;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        result = identityMappingFromPgResult(res, 0);
    }
    PQclear(res);
    return result;
}

std::vector<IdentityMapping> PgPersistentStore::listIdentityMappings(
    const std::string& tenant_id, int limit, int offset) {
    std::vector<IdentityMapping> results;
    if (!initialized_) return results;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return results;

    const char* sql =
        "SELECT id, tenant_id, external_subject, external_issuer, user_id, email, "
        "last_login_at, created_at FROM identity_mappings "
        "WHERE tenant_id = $1 LIMIT $2 OFFSET $3";
    std::string limit_str = std::to_string(limit);
    std::string offset_str = std::to_string(offset);
    const char* params[3] = { tenant_id.c_str(), limit_str.c_str(), offset_str.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 3, nullptr,
                                  params, nullptr, nullptr, 0);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i) {
            results.push_back(identityMappingFromPgResult(res, i));
        }
    }
    PQclear(res);
    return results;
}

bool PgPersistentStore::deleteIdentityMapping(const std::string& id) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql = "DELETE FROM identity_mappings WHERE id = $1";
    const char* params[1] = { id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    const char* affected = ok ? PQcmdTuples(res) : "0";
    bool changed = (affected && affected[0] && std::stoll(affected) > 0);
    PQclear(res);
    return ok && changed;
}

bool PgPersistentStore::updateIdentityMappingLastLogin(const std::string& id, const std::string& ts) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql = "UPDATE identity_mappings SET last_login_at = $1 WHERE id = $2";
    const char* params[2] = { ts.c_str(), id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 2, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    const char* affected = ok ? PQcmdTuples(res) : "0";
    bool changed = (affected && affected[0] && std::stoll(affected) > 0);
    PQclear(res);
    return ok && changed;
}

// --- Session ---

bool PgPersistentStore::insertSession(const Session& s) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql =
        "INSERT INTO sessions (id, user_id, tenant_id, ip_address, user_agent, "
        "auth_method, mfa_verified, created_at, last_active_at, expires_at) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)";

    const char* mfa = s.mfa_verified ? "true" : "false";

    const char* params[10] = {
        s.id.c_str(), s.user_id.c_str(), s.tenant_id.c_str(),
        s.ip_address.c_str(), s.user_agent.c_str(), s.auth_method.c_str(),
        mfa, s.created_at.c_str(), s.last_active_at.c_str(), s.expires_at.c_str()
    };

    PGresult* res = PQexecParams(handle->get(), sql, 10, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
}

std::optional<Session> PgPersistentStore::getSession(const std::string& id) {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;

    const char* sql =
        "SELECT id, user_id, tenant_id, ip_address, user_agent, auth_method, "
        "mfa_verified, created_at, last_active_at, expires_at FROM sessions WHERE id = $1";
    const char* params[1] = { id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    std::optional<Session> result;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        result = sessionFromPgResult(res, 0);
    }
    PQclear(res);
    return result;
}

std::vector<Session> PgPersistentStore::listSessionsByUser(const std::string& user_id) {
    std::vector<Session> results;
    if (!initialized_) return results;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return results;

    const char* sql =
        "SELECT id, user_id, tenant_id, ip_address, user_agent, auth_method, "
        "mfa_verified, created_at, last_active_at, expires_at "
        "FROM sessions WHERE user_id = $1 ORDER BY created_at DESC";
    const char* params[1] = { user_id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i) {
            results.push_back(sessionFromPgResult(res, i));
        }
    }
    PQclear(res);
    return results;
}

bool PgPersistentStore::updateSessionActivity(const std::string& id, const std::string& ts) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql = "UPDATE sessions SET last_active_at = $1 WHERE id = $2";
    const char* params[2] = { ts.c_str(), id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 2, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    const char* affected = ok ? PQcmdTuples(res) : "0";
    bool changed = (affected && affected[0] && std::stoll(affected) > 0);
    PQclear(res);
    return ok && changed;
}

bool PgPersistentStore::deleteSession(const std::string& id) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql = "DELETE FROM sessions WHERE id = $1";
    const char* params[1] = { id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    const char* affected = ok ? PQcmdTuples(res) : "0";
    bool changed = (affected && affected[0] && std::stoll(affected) > 0);
    PQclear(res);
    return ok && changed;
}

int64_t PgPersistentStore::deleteExpiredSessions() {
    if (!initialized_) return 0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0;

    const char* sql = "DELETE FROM sessions WHERE expires_at < NOW()::TEXT";

    PGresult* res = PQexec(handle->get(), sql);
    int64_t deleted = 0;
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
        const char* affected = PQcmdTuples(res);
        if (affected && affected[0]) deleted = std::stoll(affected);
    }
    PQclear(res);
    return deleted;
}

int64_t PgPersistentStore::countSessionsByUser(const std::string& user_id) {
    if (!initialized_) return 0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0;

    const char* sql = "SELECT COUNT(*) FROM sessions WHERE user_id = $1";
    const char* params[1] = { user_id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    int64_t count = 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        count = std::stoll(PQgetvalue(res, 0, 0));
    }
    PQclear(res);
    return count;
}

bool PgPersistentStore::updateSessionMfaVerified(const std::string& id, bool verified) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql = "UPDATE sessions SET mfa_verified = $1 WHERE id = $2";
    const char* mfa = verified ? "true" : "false";
    const char* params[2] = { mfa, id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 2, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    const char* affected = ok ? PQcmdTuples(res) : "0";
    bool changed = (affected && affected[0] && std::stoll(affected) > 0);
    PQclear(res);
    return ok && changed;
}

// --- MFA ---

bool PgPersistentStore::upsertMfaSecret(const MfaSecret& m) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql =
        "INSERT INTO mfa_secrets (user_id, secret_enc, enabled, recovery_codes_enc, created_at) "
        "VALUES ($1, $2, $3, $4, $5) "
        "ON CONFLICT (user_id) DO UPDATE SET secret_enc=$2, enabled=$3, "
        "recovery_codes_enc=$4, created_at=$5";

    std::string codes_json = serializeStringList(m.recovery_codes_hash);
    const char* ena = m.enabled ? "true" : "false";

    const char* params[5] = {
        m.user_id.c_str(), m.secret_enc.c_str(), ena,
        codes_json.c_str(), m.created_at.c_str()
    };

    PGresult* res = PQexecParams(handle->get(), sql, 5, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
}

std::optional<MfaSecret> PgPersistentStore::getMfaSecret(const std::string& user_id) {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;

    const char* sql =
        "SELECT user_id, secret_enc, enabled, recovery_codes_enc, created_at "
        "FROM mfa_secrets WHERE user_id = $1";
    const char* params[1] = { user_id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    std::optional<MfaSecret> result;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        result = mfaSecretFromPgResult(res, 0);
    }
    PQclear(res);
    return result;
}

bool PgPersistentStore::deleteMfaSecret(const std::string& user_id) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql = "DELETE FROM mfa_secrets WHERE user_id = $1";
    const char* params[1] = { user_id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    const char* affected = ok ? PQcmdTuples(res) : "0";
    bool changed = (affected && affected[0] && std::stoll(affected) > 0);
    PQclear(res);
    return ok && changed;
}

// --- MFA 失败锁定（TASK-20260702-02 P2-2 / SR-2）---

bool PgPersistentStore::recordMfaFailure(const std::string& user_id,
                                         int max_failures, int window_sec) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string now_s = std::to_string(now);
    std::string win_s = std::to_string(window_sec);
    std::string max_s = std::to_string(max_failures);

    // 单语句原子 UPSERT：窗口过期则重置计数为 1，否则自增；达阈值置 locked_until。
    const char* sql =
        "INSERT INTO mfa_failures (user_id, fail_count, first_fail_at, locked_until) "
        "VALUES ($1, 1, $2, CASE WHEN 1 >= $4 THEN $2 + $3 ELSE 0 END) "
        "ON CONFLICT (user_id) DO UPDATE SET "
        "fail_count = CASE WHEN $2 - mfa_failures.first_fail_at > $3 THEN 1 "
        "  ELSE mfa_failures.fail_count + 1 END, "
        "first_fail_at = CASE WHEN $2 - mfa_failures.first_fail_at > $3 THEN $2 "
        "  ELSE mfa_failures.first_fail_at END, "
        "locked_until = CASE WHEN (CASE WHEN $2 - mfa_failures.first_fail_at > $3 "
        "    THEN 1 ELSE mfa_failures.fail_count + 1 END) >= $4 THEN $2 + $3 "
        "  ELSE mfa_failures.locked_until END "
        "RETURNING locked_until";
    const char* params[4] = { user_id.c_str(), now_s.c_str(), win_s.c_str(),
                              max_s.c_str() };
    PGresult* res = PQexecParams(handle->get(), sql, 4, nullptr,
                                  params, nullptr, nullptr, 0);
    int64_t locked_until = 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        const char* v = PQgetvalue(res, 0, 0);
        if (v && v[0]) locked_until = std::stoll(v);
    }
    PQclear(res);
    return locked_until > now;
}

int64_t PgPersistentStore::getMfaLockedUntil(const std::string& user_id) {
    if (!initialized_) return 0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0;

    const char* sql = "SELECT locked_until FROM mfa_failures WHERE user_id = $1";
    const char* params[1] = { user_id.c_str() };
    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    int64_t locked_until = 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        const char* v = PQgetvalue(res, 0, 0);
        if (v && v[0]) locked_until = std::stoll(v);
    }
    PQclear(res);
    return locked_until;
}

bool PgPersistentStore::clearMfaFailures(const std::string& user_id) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql = "DELETE FROM mfa_failures WHERE user_id = $1";
    const char* params[1] = { user_id.c_str() };
    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
}

// --- SCIM Token ---

bool PgPersistentStore::insertScimToken(const ScimToken& t) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql =
        "INSERT INTO scim_tokens (id, tenant_id, token_hash, description, "
        "created_at, expires_at) VALUES ($1, $2, $3, $4, $5, $6)";

    const char* params[6] = {
        t.id.c_str(), t.tenant_id.c_str(), t.token_hash.c_str(),
        t.description.c_str(), t.created_at.c_str(), t.expires_at.c_str()
    };

    PGresult* res = PQexecParams(handle->get(), sql, 6, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
}

std::optional<ScimToken> PgPersistentStore::getScimTokenByHash(const std::string& hash) {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;

    const char* sql =
        "SELECT id, tenant_id, token_hash, description, created_at, expires_at "
        "FROM scim_tokens WHERE token_hash = $1";
    const char* params[1] = { hash.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    std::optional<ScimToken> result;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        result = scimTokenFromPgResult(res, 0);
    }
    PQclear(res);
    return result;
}

std::vector<ScimToken> PgPersistentStore::listScimTokens(const std::string& tenant_id) {
    std::vector<ScimToken> results;
    if (!initialized_) return results;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return results;

    const char* sql =
        "SELECT id, tenant_id, token_hash, description, created_at, expires_at "
        "FROM scim_tokens WHERE tenant_id = $1";
    const char* params[1] = { tenant_id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i) {
            results.push_back(scimTokenFromPgResult(res, i));
        }
    }
    PQclear(res);
    return results;
}

bool PgPersistentStore::deleteScimToken(const std::string& id) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql = "DELETE FROM scim_tokens WHERE id = $1";
    const char* params[1] = { id.c_str() };

    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    const char* affected = ok ? PQcmdTuples(res) : "0";
    bool changed = (affected && affected[0] && std::stoll(affected) > 0);
    PQclear(res);
    return ok && changed;
}

// --- SCIM Group ---

bool PgPersistentStore::insertScimGroup(const std::string& id,
                                         const std::string& tenant_id,
                                         const std::string& display_name) {
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;
    const char* sql =
        "INSERT INTO scim_groups (id, tenant_id, display_name) VALUES ($1, $2, $3)";
    const char* params[] = {id.c_str(), tenant_id.c_str(), display_name.c_str()};
    auto* res = PQexecParams(handle->get(), sql, 3, nullptr, params, nullptr, nullptr, 0);
    bool ok = PQresultStatus(res) == PGRES_COMMAND_OK;
    PQclear(res);
    return ok;
}

std::optional<PersistentStore::ScimGroupRecord>
PgPersistentStore::getScimGroup(const std::string& id) {
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;
    const char* sql =
        "SELECT id, tenant_id, display_name, created_at::text, updated_at::text "
        "FROM scim_groups WHERE id = $1";
    const char* params[] = {id.c_str()};
    auto* res = PQexecParams(handle->get(), sql, 1, nullptr, params, nullptr, nullptr, 0);
    std::optional<ScimGroupRecord> result;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        ScimGroupRecord r;
        r.id = PQgetvalue(res, 0, 0);
        r.tenant_id = PQgetvalue(res, 0, 1);
        r.display_name = PQgetvalue(res, 0, 2);
        r.created_at = PQgetvalue(res, 0, 3);
        r.updated_at = PQgetvalue(res, 0, 4);
        result = std::move(r);
    }
    PQclear(res);
    return result;
}

bool PgPersistentStore::updateScimGroup(const std::string& id,
                                         const std::string& display_name,
                                         const std::vector<std::string>& member_ids) {
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql_upd =
        "UPDATE scim_groups SET display_name = $1, updated_at = NOW() WHERE id = $2";
    const char* p1[] = {display_name.c_str(), id.c_str()};
    auto* r1 = PQexecParams(handle->get(), sql_upd, 2, nullptr, p1, nullptr, nullptr, 0);
    bool ok = PQresultStatus(r1) == PGRES_COMMAND_OK && atoi(PQcmdTuples(r1)) > 0;
    PQclear(r1);
    if (!ok) return false;

    const char* sql_del = "DELETE FROM scim_group_members WHERE group_id = $1";
    const char* p2[] = {id.c_str()};
    auto* r2 = PQexecParams(handle->get(), sql_del, 1, nullptr, p2, nullptr, nullptr, 0);
    PQclear(r2);

    const char* sql_ins =
        "INSERT INTO scim_group_members (group_id, user_id) VALUES ($1, $2)";
    for (const auto& uid : member_ids) {
        const char* p3[] = {id.c_str(), uid.c_str()};
        auto* r3 = PQexecParams(handle->get(), sql_ins, 2, nullptr, p3, nullptr, nullptr, 0);
        PQclear(r3);
    }
    return true;
}

bool PgPersistentStore::deleteScimGroup(const std::string& id) {
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;
    const char* sql = "DELETE FROM scim_groups WHERE id = $1";
    const char* params[] = {id.c_str()};
    auto* res = PQexecParams(handle->get(), sql, 1, nullptr, params, nullptr, nullptr, 0);
    bool ok = PQresultStatus(res) == PGRES_COMMAND_OK && atoi(PQcmdTuples(res)) > 0;
    PQclear(res);
    return ok;
}

std::vector<PersistentStore::ScimGroupRecord>
PgPersistentStore::listScimGroups(const std::string& tenant_id) {
    std::vector<ScimGroupRecord> result;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return result;
    const char* sql =
        "SELECT id, tenant_id, display_name, created_at::text, updated_at::text "
        "FROM scim_groups WHERE tenant_id = $1";
    const char* params[] = {tenant_id.c_str()};
    auto* res = PQexecParams(handle->get(), sql, 1, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(res); ++i) {
            ScimGroupRecord r;
            r.id = PQgetvalue(res, i, 0);
            r.tenant_id = PQgetvalue(res, i, 1);
            r.display_name = PQgetvalue(res, i, 2);
            r.created_at = PQgetvalue(res, i, 3);
            r.updated_at = PQgetvalue(res, i, 4);
            result.push_back(std::move(r));
        }
    }
    PQclear(res);
    return result;
}

std::vector<std::string>
PgPersistentStore::getScimGroupMembers(const std::string& group_id) {
    std::vector<std::string> result;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return result;
    const char* sql = "SELECT user_id FROM scim_group_members WHERE group_id = $1";
    const char* params[] = {group_id.c_str()};
    auto* res = PQexecParams(handle->get(), sql, 1, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(res); ++i) {
            result.emplace_back(PQgetvalue(res, i, 0));
        }
    }
    PQclear(res);
    return result;
}

// --- ConfigBundle Versioning (Phase 9.3) ---
//
// Follows the standard PgPersistentStore pattern: one pooled connection per
// call, parameterized queries via PQexecParams, YAML content stored as
// BYTEA (binary-safe). Activation runs inside a single-connection
// BEGIN/COMMIT so the ACTIVE invariant cannot drift even under contention.

namespace {

// Kept for future binary-format consumers; pgBytesText() handles the
// text-format decoding actually used by pgRowToConfigVersion today.
[[maybe_unused]] std::string pgBytes(PGresult* res, int row, int col) {
    int n = PQgetlength(res, row, col);
    const char* p = PQgetvalue(res, row, col);
    return std::string(p, p + n);
}

// Decode a BYTEA column returned in text format ("\\x<hex>" per the default
// bytea_output=hex). For binary format this function returns the bytes
// verbatim. Assumes well-formed hex; silently truncates on malformed input.
std::string pgBytesText(PGresult* res, int row, int col) {
    const char* raw = PQgetvalue(res, row, col);
    if (!raw) return {};
    size_t len = PQgetlength(res, row, col);
    // text format BYTEA starts with \x
    if (len >= 2 && raw[0] == '\\' && raw[1] == 'x') {
        std::string out;
        out.reserve((len - 2) / 2);
        auto hexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };
        for (size_t i = 2; i + 1 < len; i += 2) {
            int hi = hexVal(raw[i]);
            int lo = hexVal(raw[i + 1]);
            if (hi < 0 || lo < 0) break;
            out.push_back(static_cast<char>((hi << 4) | lo));
        }
        return out;
    }
    // Binary format (no \x prefix) — pass through.
    return std::string(raw, raw + len);
}

// Safe stoll for text-format integer columns: returns 0 on non-numeric
// content. Binary-format columns must not be passed in; config_versions
// reads now request text format explicitly.
std::int64_t pgStollSafe(const char* s) {
    if (!s || !s[0]) return 0;
    try {
        return std::stoll(s);
    } catch (...) {
        return 0;
    }
}

ConfigVersionRecord pgRowToConfigVersion(PGresult* res, int row) {
    // This helper expects results requested with result_format=0 (text).
    // Text-format BYTEA is returned as "\\x<hex>" by default; decode it
    // back to raw bytes via pgBytesText.
    ConfigVersionRecord r{};
    r.version_id        = PQgetvalue(res, row, 0);
    r.content_sha256    = PQgetvalue(res, row, 1);
    r.yaml_content      = pgBytesText(res, row, 2);
    r.size_bytes        = pgStollSafe(PQgetvalue(res, row, 3));
    auto parsed         = configStatusFromString(PQgetvalue(res, row, 4));
    r.status            = parsed.value_or(ConfigStatus::PENDING);
    r.submitter         = PQgetvalue(res, row, 5);
    r.submitter_comment = PQgetvalue(res, row, 6);
    r.submitted_at      = pgStollSafe(PQgetvalue(res, row, 7));
    r.reviewer          = PQgetvalue(res, row, 8);
    r.reviewer_comment  = PQgetvalue(res, row, 9);
    r.reviewed_at       = pgStollSafe(PQgetvalue(res, row, 10));
    r.activator         = PQgetvalue(res, row, 11);
    r.activated_at      = pgStollSafe(PQgetvalue(res, row, 12));
    r.deactivated_at    = pgStollSafe(PQgetvalue(res, row, 13));
    r.chain_hash        = PQgetvalue(res, row, 14);
    return r;
}

const char* kPgConfigVersionCols =
    "version_id, content_sha256, yaml_content, size_bytes, status, "
    "submitter, submitter_comment, submitted_at, reviewer, reviewer_comment, "
    "reviewed_at, activator, activated_at, deactivated_at, chain_hash";

// Phase 9.3.4 RolloutController (TASK-20260507-01) — column lists + row
// decoders for rollout / rollout_stage_events. Order MUST stay in sync with
// the PG schema in createTables() above and the SELECT/INSERT statements in
// the 7 virtual methods below.
//
// [[maybe_unused]] until Epic 2.3 (getRollout) and Epic 2.5 (listRollouts)
// wire them in; SQL text alone (Epic 2.1 insertRollout) does not yet
// reference these names, but they're co-located with the schema for clarity.
const char* kPgRolloutCols =
    "rollout_id, target_version_id, previous_active_version_id, "
    "spec_json, status, current_stage_index, started_at, stage_started_at, "
    "paused_at, pause_reason, pause_detail, creator, last_actor, "
    "completed_at, chain_hash";

// Decode a single PG row into a RolloutRecord. Expects text format
// (result_format=0); BYTEA spec_json is decoded via pgBytesText() and then
// parseRolloutSpec() turns the JSON back into POCO.
RolloutRecord pgRowToRollout(PGresult* res, int row) {
    RolloutRecord r{};
    r.rollout_id                 = PQgetvalue(res, row, 0);
    r.target_version_id          = PQgetvalue(res, row, 1);
    r.previous_active_version_id = PQgetvalue(res, row, 2);
    r.spec = parseRolloutSpec(pgBytesText(res, row, 3));
    r.status              = rolloutStatusFromWire(
        static_cast<int>(pgStollSafe(PQgetvalue(res, row, 4))));
    r.current_stage_index = static_cast<int>(pgStollSafe(PQgetvalue(res, row, 5)));
    r.started_at          = pgStollSafe(PQgetvalue(res, row, 6));
    r.stage_started_at    = pgStollSafe(PQgetvalue(res, row, 7));
    r.paused_at           = pgStollSafe(PQgetvalue(res, row, 8));
    r.pause_reason        = pauseReasonFromWire(
        static_cast<int>(pgStollSafe(PQgetvalue(res, row, 9))));
    r.pause_detail = PQgetvalue(res, row, 10);
    r.creator      = PQgetvalue(res, row, 11);
    r.last_actor   = PQgetvalue(res, row, 12);
    r.completed_at = pgStollSafe(PQgetvalue(res, row, 13));
    r.chain_hash   = PQgetvalue(res, row, 14);
    return r;
}

// Decode rollout_stage_events row. metrics_json may be NULL.
RolloutStageEvent pgRowToStageEvent(PGresult* res, int row) {
    RolloutStageEvent e{};
    e.event_id    = PQgetvalue(res, row, 0);
    e.rollout_id  = PQgetvalue(res, row, 1);
    e.stage_index = static_cast<int>(pgStollSafe(PQgetvalue(res, row, 2)));
    e.event_type  = PQgetvalue(res, row, 3);
    e.reason      = PQgetvalue(res, row, 4);
    if (!PQgetisnull(res, row, 5)) {
        e.metrics_json = pgBytesText(res, row, 5);
    }
    e.at_millis = pgStollSafe(PQgetvalue(res, row, 6));
    e.actor     = PQgetvalue(res, row, 7);
    return e;
}

} // namespace

bool PgPersistentStore::insertConfigVersion(const ConfigVersionRecord& rec) {
    if (!initialized_) return false;
    if (rec.version_id.empty()) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* sql =
        "INSERT INTO config_versions (" /* cols */
        "version_id, content_sha256, yaml_content, size_bytes, status, "
        "submitter, submitter_comment, submitted_at, reviewer, reviewer_comment, "
        "reviewed_at, activator, activated_at, deactivated_at, chain_hash) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15)";

    std::string size_str     = std::to_string(rec.size_bytes);
    std::string submitted_s  = std::to_string(rec.submitted_at);
    std::string reviewed_s   = std::to_string(rec.reviewed_at);
    std::string activated_s  = std::to_string(rec.activated_at);
    std::string deactivate_s = std::to_string(rec.deactivated_at);
    const char* status_s     = configStatusToString(rec.status);

    // Binary YAML content bound as BYTEA via paramFormats[2] = 1.
    const char* params[15] = {
        rec.version_id.c_str(),
        rec.content_sha256.c_str(),
        rec.yaml_content.data(),
        size_str.c_str(),
        status_s,
        rec.submitter.c_str(),
        rec.submitter_comment.c_str(),
        submitted_s.c_str(),
        rec.reviewer.c_str(),
        rec.reviewer_comment.c_str(),
        reviewed_s.c_str(),
        rec.activator.c_str(),
        activated_s.c_str(),
        deactivate_s.c_str(),
        rec.chain_hash.c_str()
    };
    int lengths[15] = {
        0, 0,
        static_cast<int>(rec.yaml_content.size()), 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    int formats[15] = {
        0, 0,
        1, // yaml_content is binary
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    PGresult* res = PQexecParams(handle->get(), sql, 15, nullptr,
                                  params, lengths, formats, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
}

bool PgPersistentStore::updateConfigStatus(
    const std::string& version_id,
    ConfigStatus new_status,
    const std::string& actor,
    const std::string& comment,
    std::int64_t timestamp_ms) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const bool is_review = (new_status == ConfigStatus::APPROVED ||
                            new_status == ConfigStatus::REJECTED);
    const char* status_s = configStatusToString(new_status);
    std::string ts_str   = std::to_string(timestamp_ms);

    PGresult* res;
    if (is_review) {
        const char* sql =
            "UPDATE config_versions SET status = $1, reviewer = $2, "
            "reviewer_comment = $3, reviewed_at = $4 WHERE version_id = $5";
        const char* params[5] = {
            status_s, actor.c_str(), comment.c_str(),
            ts_str.c_str(), version_id.c_str()
        };
        res = PQexecParams(handle->get(), sql, 5, nullptr,
                            params, nullptr, nullptr, 0);
    } else {
        const char* sql =
            "UPDATE config_versions SET status = $1 WHERE version_id = $2";
        const char* params[2] = { status_s, version_id.c_str() };
        res = PQexecParams(handle->get(), sql, 2, nullptr,
                            params, nullptr, nullptr, 0);
    }
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    // Distinguish "no such row" (PG returns COMMAND_OK with 0 rows affected).
    if (ok) {
        const char* affected = PQcmdTuples(res);
        if (affected && affected[0] != '\0') {
            ok = std::atoi(affected) > 0;
        }
    }
    PQclear(res);
    return ok;
}

std::optional<ConfigVersionRecord>
PgPersistentStore::getConfigVersion(const std::string& version_id) {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;

    std::string sql = std::string("SELECT ") + kPgConfigVersionCols +
                      " FROM config_versions WHERE version_id = $1 LIMIT 1";
    const char* params[1] = { version_id.c_str() };
    // Use text result format (0): pgRowToConfigVersion calls std::stoll on
    // integer columns which requires text-form digits. BYTEA yaml_content
    // comes back as "\\x<hex>" and is decoded by pgBytesText().
    PGresult* res = PQexecParams(handle->get(), sql.c_str(), 1, nullptr,
                                  params, nullptr, nullptr, 0);
    std::optional<ConfigVersionRecord> out;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1) {
        out = pgRowToConfigVersion(res, 0);
    }
    PQclear(res);
    return out;
}

std::vector<ConfigVersionRecord>
PgPersistentStore::listConfigVersions(const ConfigVersionQuery& q) {
    std::vector<ConfigVersionRecord> out;
    if (!initialized_) return out;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return out;

    std::string sql = std::string("SELECT ") + kPgConfigVersionCols +
                      " FROM config_versions WHERE TRUE";
    std::vector<std::string> params;
    params.reserve(q.statuses.size() + 4);
    auto bind = [&](const std::string& v) {
        params.push_back(v);
        return std::string("$") + std::to_string(params.size());
    };

    if (!q.statuses.empty()) {
        sql += " AND status IN (";
        for (size_t i = 0; i < q.statuses.size(); ++i) {
            if (i > 0) sql += ",";
            sql += bind(configStatusToString(q.statuses[i]));
        }
        sql += ")";
    }
    if (q.since_millis > 0) {
        sql += " AND submitted_at >= " + bind(std::to_string(q.since_millis));
    }
    // Cursor lookup: if page_token exists, find its submitted_at then apply
    // the composite filter. Two round-trips on the same pooled connection
    // keep the code path simple and the condition sargable.
    std::int64_t tok_submitted = -1;
    if (!q.page_token.empty()) {
        const char* lookup_sql =
            "SELECT submitted_at FROM config_versions WHERE version_id = $1";
        const char* lookup_params[1] = { q.page_token.c_str() };
        PGresult* ls = PQexecParams(handle->get(), lookup_sql, 1, nullptr,
                                     lookup_params, nullptr, nullptr, 0);
        if (PQresultStatus(ls) == PGRES_TUPLES_OK && PQntuples(ls) == 1) {
            tok_submitted = std::stoll(PQgetvalue(ls, 0, 0));
        }
        PQclear(ls);
        if (tok_submitted >= 0) {
            auto a = bind(std::to_string(tok_submitted));
            auto b = bind(std::to_string(tok_submitted));
            auto c = bind(q.page_token);
            sql += " AND (submitted_at < " + a +
                   " OR (submitted_at = " + b +
                   " AND version_id < " + c + "))";
        }
    }
    const int cap = q.limit > 0 ? (q.limit > 500 ? 500 : q.limit) : 50;
    sql += " ORDER BY submitted_at DESC, version_id DESC LIMIT " +
           bind(std::to_string(cap));

    std::vector<const char*> param_ptrs;
    param_ptrs.reserve(params.size());
    for (const auto& p : params) param_ptrs.push_back(p.c_str());

    // Request binary result so yaml_content is faithful.
    PGresult* res = PQexecParams(handle->get(), sql.c_str(),
                                  static_cast<int>(params.size()), nullptr,
                                  param_ptrs.empty() ? nullptr : param_ptrs.data(),
                                  nullptr, nullptr, 0);  // text format — see getConfigVersion
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(res); ++i) {
            out.push_back(pgRowToConfigVersion(res, i));
        }
    }
    PQclear(res);
    return out;
}

std::optional<ConfigVersionRecord> PgPersistentStore::getActiveConfig() {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;

    std::string sql = std::string("SELECT ") + kPgConfigVersionCols +
                      " FROM config_versions WHERE status = 'ACTIVE' LIMIT 1";
    PGresult* res = PQexecParams(handle->get(), sql.c_str(), 0, nullptr,
                                  nullptr, nullptr, nullptr, 0);  // text format
    std::optional<ConfigVersionRecord> out;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1) {
        out = pgRowToConfigVersion(res, 0);
    }
    PQclear(res);
    return out;
}

bool PgPersistentStore::activateConfig(
    const std::string& version_id,
    const std::string& activator,
    std::int64_t activate_ms) {
    if (!initialized_) return false;
    if (version_id.empty()) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    PGconn* conn = handle->get();
    auto exec = [&](const char* sql) -> bool {
        PGresult* r = PQexec(conn, sql);
        bool ok = (PQresultStatus(r) == PGRES_COMMAND_OK);
        PQclear(r);
        return ok;
    };
    auto rollback = [&]() { exec("ROLLBACK"); };

    if (!exec("BEGIN")) return false;

    // 1) Load target row (inside tx, SELECT ... FOR UPDATE prevents races).
    const char* status_sql =
        "SELECT status FROM config_versions WHERE version_id = $1 FOR UPDATE";
    const char* status_params[1] = { version_id.c_str() };
    PGresult* sr = PQexecParams(conn, status_sql, 1, nullptr,
                                 status_params, nullptr, nullptr, 0);
    std::string current_status;
    if (PQresultStatus(sr) == PGRES_TUPLES_OK && PQntuples(sr) == 1) {
        current_status = PQgetvalue(sr, 0, 0);
    }
    PQclear(sr);
    if (current_status.empty()) { rollback(); return false; }

    // 2) Idempotent: target already ACTIVE -> commit no-op tx.
    if (current_status == "ACTIVE") {
        return exec("COMMIT");
    }
    // 3) Only APPROVED / SUPERSEDED are activatable (R2 rollback via latter).
    if (current_status != "APPROVED" && current_status != "SUPERSEDED") {
        rollback(); return false;
    }

    // 4) Demote previous ACTIVE (if any) to SUPERSEDED.
    std::string act_ms_str = std::to_string(activate_ms);
    const char* demote_sql =
        "UPDATE config_versions "
        "SET status = 'SUPERSEDED', deactivated_at = $1 "
        "WHERE status = 'ACTIVE' AND version_id <> $2";
    const char* demote_params[2] = { act_ms_str.c_str(), version_id.c_str() };
    PGresult* dr = PQexecParams(conn, demote_sql, 2, nullptr,
                                 demote_params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(dr) == PGRES_COMMAND_OK);
    PQclear(dr);
    if (!ok) { rollback(); return false; }

    // 5) Promote target -> ACTIVE. The unique partial index enforces the
    //    post-condition even if step 4 raced (the partial index is what
    //    makes this whole path safe).
    const char* promote_sql =
        "UPDATE config_versions "
        "SET status = 'ACTIVE', activator = $1, activated_at = $2, "
        "    deactivated_at = 0 "
        "WHERE version_id = $3";
    const char* promote_params[3] = {
        activator.c_str(), act_ms_str.c_str(), version_id.c_str()
    };
    PGresult* pr = PQexecParams(conn, promote_sql, 3, nullptr,
                                 promote_params, nullptr, nullptr, 0);
    ok = (PQresultStatus(pr) == PGRES_COMMAND_OK);
    PQclear(pr);
    if (!ok) { rollback(); return false; }

    return exec("COMMIT");
}

// ---------------------------------------------------------------------------
// Phase 9.3.4 RolloutController PG backend (TASK-20260507-01)
// 7 virtual methods land in Epic 2 of docs/plans/2026-05-07-...md.
// ---------------------------------------------------------------------------

bool PgPersistentStore::insertRollout(const RolloutRecord& rec) {
    if (!initialized_) return false;
    if (rec.rollout_id.empty()) return false;
    auto handle = pool_->acquire(
        std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    // Serialize POCO → JSON BLOB once; bind via paramFormats[3]=1 (binary).
    // Empty spec_json is rejected by the column NOT NULL constraint at the
    // schema layer (defense in depth — application path won't normally hit
    // this since RolloutSpec defaults are non-empty).
    const std::string spec_json    = serializeRolloutSpec(rec.spec);
    const std::string status_s     = std::to_string(rolloutStatusToWire(rec.status));
    const std::string stage_idx_s  = std::to_string(rec.current_stage_index);
    const std::string started_s    = std::to_string(rec.started_at);
    const std::string stage_at_s   = std::to_string(rec.stage_started_at);
    const std::string paused_s     = std::to_string(rec.paused_at);
    const std::string reason_s     = std::to_string(pauseReasonToWire(rec.pause_reason));
    const std::string completed_s  = std::to_string(rec.completed_at);

    const char* sql =
        "INSERT INTO rollouts ("
        "rollout_id, target_version_id, previous_active_version_id, "
        "spec_json, status, current_stage_index, started_at, "
        "stage_started_at, paused_at, pause_reason, pause_detail, "
        "creator, last_actor, completed_at, chain_hash) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, "
        "$14, $15)";

    const char* params[15] = {
        rec.rollout_id.c_str(),
        rec.target_version_id.c_str(),
        rec.previous_active_version_id.c_str(),
        spec_json.data(),
        status_s.c_str(),
        stage_idx_s.c_str(),
        started_s.c_str(),
        stage_at_s.c_str(),
        paused_s.c_str(),
        reason_s.c_str(),
        rec.pause_detail.c_str(),
        rec.creator.c_str(),
        rec.last_actor.c_str(),
        completed_s.c_str(),
        rec.chain_hash.c_str()};
    int lengths[15] = {
        0, 0, 0, static_cast<int>(spec_json.size()),
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int formats[15] = {
        0, 0, 0, 1,  // SR18: BYTEA spec_json bound as binary, no escaping
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    PGresult* res = PQexecParams(handle->get(), sql, 15, nullptr,
                                 params, lengths, formats, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        // SQLSTATE 23505 = unique_violation; expected when the partial
        // UNIQUE INDEX rolls back a duplicate active rollout. Don't log as
        // error — that's a normal application-level rejection (SR14
        // defense in depth).
        const char* sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
        if (!sqlstate || std::string(sqlstate) != "23505") {
            spdlog::error("PG insertRollout failed: {}",
                          PQresultErrorMessage(res));
        }
    }
    PQclear(res);
    return ok;
}

bool PgPersistentStore::updateRollout(const RolloutRecord& rec) {
    if (!initialized_) return false;
    if (rec.rollout_id.empty()) return false;
    auto handle = pool_->acquire(
        std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const std::string spec_json    = serializeRolloutSpec(rec.spec);
    const std::string status_s     = std::to_string(rolloutStatusToWire(rec.status));
    const std::string stage_idx_s  = std::to_string(rec.current_stage_index);
    const std::string started_s    = std::to_string(rec.started_at);
    const std::string stage_at_s   = std::to_string(rec.stage_started_at);
    const std::string paused_s     = std::to_string(rec.paused_at);
    const std::string reason_s     = std::to_string(pauseReasonToWire(rec.pause_reason));
    const std::string completed_s  = std::to_string(rec.completed_at);

    // RETURNING rollout_id collapses "row exists?" + "did UPDATE hit?" into
    // one round-trip — PQntuples == 1 iff the WHERE clause matched. SR14
    // is enforced by the partial UNIQUE INDEX on subsequent INSERTs; this
    // statement only mutates an already-stored row.
    const char* sql =
        "UPDATE rollouts SET "
        "target_version_id = $1, previous_active_version_id = $2, "
        "spec_json = $3, status = $4, current_stage_index = $5, "
        "started_at = $6, stage_started_at = $7, paused_at = $8, "
        "pause_reason = $9, pause_detail = $10, creator = $11, "
        "last_actor = $12, completed_at = $13, chain_hash = $14 "
        "WHERE rollout_id = $15 RETURNING rollout_id";

    const char* params[15] = {
        rec.target_version_id.c_str(),
        rec.previous_active_version_id.c_str(),
        spec_json.data(),
        status_s.c_str(),
        stage_idx_s.c_str(),
        started_s.c_str(),
        stage_at_s.c_str(),
        paused_s.c_str(),
        reason_s.c_str(),
        rec.pause_detail.c_str(),
        rec.creator.c_str(),
        rec.last_actor.c_str(),
        completed_s.c_str(),
        rec.chain_hash.c_str(),
        rec.rollout_id.c_str()};
    int lengths[15] = {
        0, 0, static_cast<int>(spec_json.size()),
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int formats[15] = {
        0, 0, 1,  // SR18: BYTEA spec_json bound as binary
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    PGresult* res = PQexecParams(handle->get(), sql, 15, nullptr,
                                 params, lengths, formats, 0);
    bool ok = (PQresultStatus(res) == PGRES_TUPLES_OK
               && PQntuples(res) == 1);
    if (PQresultStatus(res) != PGRES_TUPLES_OK
        && PQresultStatus(res) != PGRES_COMMAND_OK) {
        spdlog::error("PG updateRollout failed: {}",
                      PQresultErrorMessage(res));
    }
    PQclear(res);
    return ok;
}

std::vector<RolloutRecord>
PgPersistentStore::listRollouts(const RolloutQuery& q) {
    std::vector<RolloutRecord> out;
    if (!initialized_) return out;
    auto handle = pool_->acquire(
        std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return out;

    // Build SQL + parameter list dynamically. We use a vector<string> for
    // parameter lifetimes and hand PQexecParams() a parallel vector of
    // const char* pointers.
    std::string sql = std::string("SELECT ") + kPgRolloutCols +
                      " FROM rollouts WHERE TRUE";
    std::vector<std::string> params;
    auto bind = [&](const std::string& v) {
        params.push_back(v);
        return std::string("$") + std::to_string(params.size());
    };

    if (!q.statuses.empty()) {
        sql += " AND status IN (";
        for (size_t i = 0; i < q.statuses.size(); ++i) {
            if (i > 0) sql += ",";
            sql += bind(std::to_string(rolloutStatusToWire(q.statuses[i])));
        }
        sql += ")";
    }

    // Cursor: page_token is the last rollout_id of the previous page.
    // Two round-trips: lookup tok_started_at, then composite tuple compare.
    // Mirrors SQLitePersistentStore::listRollouts (kept in sync so the
    // typed test in test_rollout_storage.cpp produces identical results
    // across backends).
    std::int64_t tok_started = -1;
    std::string  tok_id;
    if (!q.page_token.empty()) {
        const char* lookup_sql =
            "SELECT started_at FROM rollouts WHERE rollout_id = $1";
        const char* lp[1] = {q.page_token.c_str()};
        PGresult* lr = PQexecParams(handle->get(), lookup_sql, 1, nullptr,
                                    lp, nullptr, nullptr, 0);
        if (PQresultStatus(lr) == PGRES_TUPLES_OK && PQntuples(lr) == 1) {
            tok_started = pgStollSafe(PQgetvalue(lr, 0, 0));
            tok_id = q.page_token;
        }
        PQclear(lr);
    }
    if (!tok_id.empty()) {
        auto a = bind(std::to_string(tok_started));
        auto b = bind(std::to_string(tok_started));
        auto c = bind(tok_id);
        sql += " AND (started_at < " + a +
               " OR (started_at = " + b +
               " AND rollout_id < " + c + "))";
    }

    sql += " ORDER BY started_at DESC, rollout_id DESC";
    if (q.limit > 0) {
        sql += " LIMIT " + bind(std::to_string(q.limit));
    }

    std::vector<const char*> param_ptrs;
    param_ptrs.reserve(params.size());
    for (const auto& p : params) param_ptrs.push_back(p.c_str());

    PGresult* res = PQexecParams(handle->get(), sql.c_str(),
                                 static_cast<int>(params.size()), nullptr,
                                 param_ptrs.empty() ? nullptr : param_ptrs.data(),
                                 nullptr, nullptr, 0);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        const int n = PQntuples(res);
        out.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            out.push_back(pgRowToRollout(res, i));
        }
    } else {
        spdlog::error("PG listRollouts failed: {}", PQresultErrorMessage(res));
    }
    PQclear(res);
    return out;
}

std::optional<RolloutRecord>
PgPersistentStore::findActiveRolloutByTarget(
    const std::string& target_version_id) {
    if (!initialized_) return std::nullopt;
    if (target_version_id.empty()) return std::nullopt;
    auto handle = pool_->acquire(
        std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;

    // status IN (1, 2, 3) mirrors the partial UNIQUE INDEX so the planner
    // can use the index for both lookup and the SR14 invariant. LIMIT 1
    // is redundant under the unique constraint but kept for defensive
    // clarity if the index is ever removed.
    const std::string sql = std::string("SELECT ") + kPgRolloutCols +
                            " FROM rollouts WHERE target_version_id = $1 "
                            "AND status IN (1, 2, 3) LIMIT 1";
    const char* params[1] = {target_version_id.c_str()};
    PGresult* res = PQexecParams(handle->get(), sql.c_str(), 1, nullptr,
                                 params, nullptr, nullptr, 0);
    std::optional<RolloutRecord> out;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1) {
        out = pgRowToRollout(res, 0);
    }
    PQclear(res);
    return out;
}

std::optional<RolloutRecord>
PgPersistentStore::getRollout(const std::string& rollout_id) {
    if (!initialized_) return std::nullopt;
    if (rollout_id.empty()) return std::nullopt;
    auto handle = pool_->acquire(
        std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;

    const std::string sql = std::string("SELECT ") + kPgRolloutCols +
                            " FROM rollouts WHERE rollout_id = $1 LIMIT 1";
    const char* params[1] = {rollout_id.c_str()};
    PGresult* res = PQexecParams(handle->get(), sql.c_str(), 1, nullptr,
                                 params, nullptr, nullptr, 0);
    std::optional<RolloutRecord> out;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1) {
        out = pgRowToRollout(res, 0);
    }
    PQclear(res);
    return out;
}

bool PgPersistentStore::appendRolloutStageEvent(const RolloutStageEvent& ev) {
    if (!initialized_) return false;
    if (ev.event_id.empty() || ev.rollout_id.empty()) return false;
    auto handle = pool_->acquire(
        std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const std::string stage_idx_s = std::to_string(ev.stage_index);
    const std::string at_millis_s = std::to_string(ev.at_millis);

    const char* sql =
        "INSERT INTO rollout_stage_events ("
        "event_id, rollout_id, stage_index, event_type, reason, "
        "metrics_json, at_millis, actor) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8)";

    // metrics_json is a nullable BYTEA; pass nullptr param + length 0 so
    // libpq sends SQL NULL when the application supplies no payload.
    const bool has_metrics = !ev.metrics_json.empty();
    const char* params[8] = {
        ev.event_id.c_str(),
        ev.rollout_id.c_str(),
        stage_idx_s.c_str(),
        ev.event_type.c_str(),
        ev.reason.c_str(),
        has_metrics ? ev.metrics_json.data() : nullptr,
        at_millis_s.c_str(),
        ev.actor.c_str()};
    int lengths[8] = {
        0, 0, 0, 0, 0,
        has_metrics ? static_cast<int>(ev.metrics_json.size()) : 0,
        0, 0};
    int formats[8] = {
        0, 0, 0, 0, 0,
        has_metrics ? 1 : 0,  // SR18: BYTEA bound binary when present
        0, 0};

    PGresult* res = PQexecParams(handle->get(), sql, 8, nullptr,
                                 params, lengths, formats, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        spdlog::error("PG appendRolloutStageEvent failed: {}",
                      PQresultErrorMessage(res));
    }
    PQclear(res);
    return ok;
}

std::vector<RolloutStageEvent>
PgPersistentStore::listRolloutStageEvents(const std::string& rollout_id) {
    std::vector<RolloutStageEvent> out;
    if (!initialized_) return out;
    if (rollout_id.empty()) return out;
    auto handle = pool_->acquire(
        std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return out;

    // Order by (at_millis, event_id) ASC — same as SQLite backend so the
    // typed test sees identical ordering. Index
    // rollout_stage_events_by_rollout(rollout_id, at_millis) covers it.
    const char* sql =
        "SELECT event_id, rollout_id, stage_index, event_type, reason, "
        "metrics_json, at_millis, actor FROM rollout_stage_events "
        "WHERE rollout_id = $1 ORDER BY at_millis ASC, event_id ASC";
    const char* params[1] = {rollout_id.c_str()};
    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                 params, nullptr, nullptr, 0);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        const int n = PQntuples(res);
        out.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            out.push_back(pgRowToStageEvent(res, i));
        }
    } else {
        spdlog::error("PG listRolloutStageEvents failed: {}",
                      PQresultErrorMessage(res));
    }
    PQclear(res);
    return out;
}

// =========================================================================
// Phase 11.5 AutonomyApprovalWorkflow PG backend — TASK-20260518-02 Epic 1.0
// =========================================================================

namespace {

ApprovalProposalRecord pgRowToApprovalProposal(PGresult* res, int row) {
    ApprovalProposalRecord r;
    auto col = [&](int c) -> std::string {
        const char* v = PQgetvalue(res, row, c);
        return v ? v : "";
    };
    r.id                  = col(0);
    r.source              = col(1);
    r.subject             = col(2);
    r.payload_json        = col(3);
    r.decision_trace_json = col(4);
    r.proposed_at_ms      = std::strtoll(col(5).c_str(), nullptr, 10);
    r.proposer_user_id    = col(6);
    r.state               = col(7);
    r.reviewer_user_id    = col(8);
    r.reviewed_at_ms      = std::strtoll(col(9).c_str(), nullptr, 10);
    r.reject_reason       = col(10);
    r.payload_sha256      = col(11);
    return r;
}

constexpr const char* kPgApprovalCols =
    "id, source, subject, payload_json, decision_trace_json, "
    "proposed_at_ms, proposer_user_id, state, reviewer_user_id, "
    "reviewed_at_ms, reject_reason, payload_sha256";

} // namespace

bool PgPersistentStore::insertApprovalProposal(
    const ApprovalProposalRecord& rec) {
    if (!initialized_) return false;
    if (rec.id.empty()) return false;
    auto handle = pool_->acquire(
        std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const std::string proposed_s = std::to_string(rec.proposed_at_ms);
    const std::string reviewed_s = std::to_string(rec.reviewed_at_ms);

    const char* sql =
        "INSERT INTO autonomy_proposals ("
        "id, source, subject, payload_json, decision_trace_json, "
        "proposed_at_ms, proposer_user_id, state, reviewer_user_id, "
        "reviewed_at_ms, reject_reason, payload_sha256) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)";

    const char* params[12] = {
        rec.id.c_str(),
        rec.source.c_str(),
        rec.subject.c_str(),
        rec.payload_json.c_str(),
        rec.decision_trace_json.c_str(),
        proposed_s.c_str(),
        rec.proposer_user_id.c_str(),
        rec.state.c_str(),
        rec.reviewer_user_id.c_str(),
        reviewed_s.c_str(),
        rec.reject_reason.c_str(),
        rec.payload_sha256.c_str()};

    PGresult* res = PQexecParams(handle->get(), sql, 12, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        // 23505 = unique_violation; expected for duplicate id (mirrors
        // Memory/SQLite behaviour). Don't log as error.
        const char* sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
        if (!sqlstate || std::string(sqlstate) != "23505") {
            spdlog::error("PG insertApprovalProposal failed: {}",
                          PQresultErrorMessage(res));
        }
    }
    PQclear(res);
    return ok;
}

std::optional<ApprovalProposalRecord>
PgPersistentStore::getApprovalProposal(const std::string& id) {
    if (!initialized_) return std::nullopt;
    if (id.empty()) return std::nullopt;
    auto handle = pool_->acquire(
        std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;

    const std::string sql = std::string("SELECT ") + kPgApprovalCols +
                            " FROM autonomy_proposals WHERE id = $1 LIMIT 1";
    const char* params[1] = {id.c_str()};
    PGresult* res = PQexecParams(handle->get(), sql.c_str(), 1, nullptr,
                                  params, nullptr, nullptr, 0);
    std::optional<ApprovalProposalRecord> out;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1) {
        out = pgRowToApprovalProposal(res, 0);
    }
    PQclear(res);
    return out;
}

bool PgPersistentStore::updateApprovalProposal(
    const ApprovalProposalRecord& rec) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(
        std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const std::string proposed_s = std::to_string(rec.proposed_at_ms);
    const std::string reviewed_s = std::to_string(rec.reviewed_at_ms);

    const char* sql =
        "UPDATE autonomy_proposals SET "
        "source = $1, subject = $2, payload_json = $3, "
        "decision_trace_json = $4, proposed_at_ms = $5, "
        "proposer_user_id = $6, state = $7, reviewer_user_id = $8, "
        "reviewed_at_ms = $9, reject_reason = $10, payload_sha256 = $11 "
        "WHERE id = $12";

    const char* params[12] = {
        rec.source.c_str(),
        rec.subject.c_str(),
        rec.payload_json.c_str(),
        rec.decision_trace_json.c_str(),
        proposed_s.c_str(),
        rec.proposer_user_id.c_str(),
        rec.state.c_str(),
        rec.reviewer_user_id.c_str(),
        reviewed_s.c_str(),
        rec.reject_reason.c_str(),
        rec.payload_sha256.c_str(),
        rec.id.c_str()};

    PGresult* res = PQexecParams(handle->get(), sql, 12, nullptr,
                                  params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (ok) {
        // PGRES_COMMAND_OK for 0 rows updated; check command tag for affected count.
        const char* tag = PQcmdTuples(res);
        if (!tag || std::string(tag) == "0") ok = false;
    } else {
        spdlog::error("PG updateApprovalProposal failed: {}",
                      PQresultErrorMessage(res));
    }
    PQclear(res);
    return ok;
}

std::vector<ApprovalProposalRecord>
PgPersistentStore::listApprovalProposals(const ApprovalProposalQuery& q) {
    std::vector<ApprovalProposalRecord> out;
    if (!initialized_) return out;
    auto handle = pool_->acquire(
        std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return out;

    std::string sql = std::string("SELECT ") + kPgApprovalCols +
                      " FROM autonomy_proposals WHERE 1=1";
    std::vector<std::string> param_buf;
    if (!q.state_filter.empty()) {
        param_buf.push_back(q.state_filter);
        sql += " AND state = $" + std::to_string(param_buf.size());
    }
    if (!q.source_filter.empty()) {
        param_buf.push_back(q.source_filter);
        sql += " AND source = $" + std::to_string(param_buf.size());
    }
    const int limit  = q.limit  > 0 ? q.limit  : 1000;
    const int offset = q.offset > 0 ? q.offset : 0;
    param_buf.push_back(std::to_string(limit));
    sql += " ORDER BY proposed_at_ms DESC, id DESC LIMIT $" +
           std::to_string(param_buf.size());
    param_buf.push_back(std::to_string(offset));
    sql += " OFFSET $" + std::to_string(param_buf.size());

    std::vector<const char*> params;
    params.reserve(param_buf.size());
    for (const auto& p : param_buf) params.push_back(p.c_str());

    PGresult* res = PQexecParams(handle->get(), sql.c_str(),
                                  static_cast<int>(params.size()), nullptr,
                                  params.data(), nullptr, nullptr, 0);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        const int n = PQntuples(res);
        out.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            out.push_back(pgRowToApprovalProposal(res, i));
        }
    } else {
        spdlog::error("PG listApprovalProposals failed: {}",
                      PQresultErrorMessage(res));
    }
    PQclear(res);
    return out;
}

std::int64_t PgPersistentStore::pruneApprovalProposals(int retention_days) {
    if (!initialized_ || retention_days <= 0) return 0;
    auto handle = pool_->acquire(
        std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return 0;

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::int64_t cutoff_ms = now_ms -
        static_cast<std::int64_t>(retention_days) * 86400LL * 1000LL;
    const std::string cutoff_s = std::to_string(cutoff_ms);

    const char* sql =
        "DELETE FROM autonomy_proposals WHERE proposed_at_ms < $1";
    const char* params[1] = {cutoff_s.c_str()};
    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr,
                                  params, nullptr, nullptr, 0);
    std::int64_t pruned = 0;
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
        const char* tag = PQcmdTuples(res);
        if (tag) pruned = std::strtoll(tag, nullptr, 10);
    } else {
        spdlog::error("PG pruneApprovalProposals failed: {}",
                      PQresultErrorMessage(res));
    }
    PQclear(res);
    return pruned;
}

// ============================================================================
// TASK-20260604-01 P0-B — queryCostsByDateRange（usage prediction / savings ROI）
// ============================================================================

std::vector<CostRecord> PgPersistentStore::queryCostsByDateRange(
    const std::string& tenant_id, const std::string& from, const std::string& to) {
    std::vector<CostRecord> result;
    if (!initialized_) return result;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return result;

    // 镜像 SQLite：空 tenant_id 不过滤；范围闭区间；上限 100000。
    const char* sql =
        "SELECT request_id, tenant_id, app_id, model, input_tokens, output_tokens, "
        "input_cost, output_cost, total_cost, timestamp, modality, baseline_cost, "
        "routing_decision_reason FROM cost_records "
        "WHERE timestamp >= $1 AND timestamp <= $2 "
        "AND ($3 = '' OR tenant_id = $3) "
        "ORDER BY timestamp ASC LIMIT 100000";
    const char* params[3] = { from.c_str(), to.c_str(), tenant_id.c_str() };
    PGresult* res = PQexecParams(handle->get(), sql, 3, nullptr,
                                 params, nullptr, nullptr, 0);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i) {
            CostRecord r;
            r.request_id = PQgetvalue(res, i, 0);
            r.tenant_id = PQgetvalue(res, i, 1);
            r.app_id = PQgetvalue(res, i, 2);
            r.model = PQgetvalue(res, i, 3);
            r.input_tokens = std::stoi(PQgetvalue(res, i, 4));
            r.output_tokens = std::stoi(PQgetvalue(res, i, 5));
            r.input_cost = std::stod(PQgetvalue(res, i, 6));
            r.output_cost = std::stod(PQgetvalue(res, i, 7));
            r.total_cost = std::stod(PQgetvalue(res, i, 8));
            r.timestamp = PQgetvalue(res, i, 9);
            r.modality = PQgetvalue(res, i, 10);
            r.baseline_cost = std::stod(PQgetvalue(res, i, 11));
            r.routing_decision_reason = PQgetvalue(res, i, 12);
            result.push_back(std::move(r));
        }
    }
    PQclear(res);
    return result;
}

// ============================================================================
// TASK-20260604-01 P0-A — Prompt Template（镜像 SQLite）
// ============================================================================

namespace {
PersistentStore::PromptTemplateRecord pgRowToPromptTemplate(PGresult* res, int row) {
    PersistentStore::PromptTemplateRecord r;
    r.id = PQgetvalue(res, row, 0);
    r.tenant_id = PQgetvalue(res, row, 1);
    r.name = PQgetvalue(res, row, 2);
    r.content = PQgetvalue(res, row, 3);
    r.version = std::stoi(PQgetvalue(res, row, 4));
    r.weight = std::stoi(PQgetvalue(res, row, 5));
    r.is_active = std::stoi(PQgetvalue(res, row, 6)) != 0;
    r.created_at = PQgetvalue(res, row, 7);
    r.updated_at = PQgetvalue(res, row, 8);
    return r;
}
} // namespace

bool PgPersistentStore::insertPromptTemplate(const PromptTemplateRecord& tpl) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;
    const char* sql =
        "INSERT INTO prompt_templates (id, tenant_id, name, content, version, "
        "weight, is_active, created_at, updated_at) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)";
    std::string version = std::to_string(tpl.version);
    std::string weight = std::to_string(tpl.weight);
    std::string active = tpl.is_active ? "1" : "0";
    const char* params[9] = {
        tpl.id.c_str(), tpl.tenant_id.c_str(), tpl.name.c_str(), tpl.content.c_str(),
        version.c_str(), weight.c_str(), active.c_str(),
        tpl.created_at.c_str(), tpl.updated_at.c_str()
    };
    PGresult* res = PQexecParams(handle->get(), sql, 9, nullptr, params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
}

std::optional<PersistentStore::PromptTemplateRecord>
PgPersistentStore::getPromptTemplate(const std::string& id) {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;
    const char* sql =
        "SELECT id, tenant_id, name, content, version, weight, is_active, "
        "created_at, updated_at FROM prompt_templates WHERE id = $1";
    const char* params[1] = { id.c_str() };
    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr, params, nullptr, nullptr, 0);
    std::optional<PromptTemplateRecord> out;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
        out = pgRowToPromptTemplate(res, 0);
    PQclear(res);
    return out;
}

bool PgPersistentStore::updatePromptTemplate(const PromptTemplateRecord& tpl) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;
    const char* sql =
        "UPDATE prompt_templates SET name=$1, content=$2, version=$3, weight=$4, "
        "is_active=$5, updated_at=$6 WHERE id=$7";
    std::string version = std::to_string(tpl.version);
    std::string weight = std::to_string(tpl.weight);
    std::string active = tpl.is_active ? "1" : "0";
    const char* params[7] = {
        tpl.name.c_str(), tpl.content.c_str(), version.c_str(), weight.c_str(),
        active.c_str(), tpl.updated_at.c_str(), tpl.id.c_str()
    };
    PGresult* res = PQexecParams(handle->get(), sql, 7, nullptr, params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK) &&
              std::string(PQcmdTuples(res)) != "0";
    PQclear(res);
    return ok;
}

bool PgPersistentStore::deletePromptTemplate(const std::string& id) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;
    const char* sql = "DELETE FROM prompt_templates WHERE id = $1";
    const char* params[1] = { id.c_str() };
    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr, params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK) &&
              std::string(PQcmdTuples(res)) != "0";
    PQclear(res);
    return ok;
}

std::vector<PersistentStore::PromptTemplateRecord>
PgPersistentStore::listPromptTemplates(const std::string& tenant_id, int limit, int offset) {
    std::vector<PromptTemplateRecord> result;
    if (!initialized_) return result;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return result;
    const char* sql =
        "SELECT id, tenant_id, name, content, version, weight, is_active, "
        "created_at, updated_at FROM prompt_templates WHERE tenant_id = $1 "
        "ORDER BY id LIMIT $2 OFFSET $3";
    std::string limit_str = std::to_string(limit);
    std::string offset_str = std::to_string(offset);
    const char* params[3] = { tenant_id.c_str(), limit_str.c_str(), offset_str.c_str() };
    PGresult* res = PQexecParams(handle->get(), sql, 3, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i) result.push_back(pgRowToPromptTemplate(res, i));
    }
    PQclear(res);
    return result;
}

std::vector<PersistentStore::PromptTemplateRecord>
PgPersistentStore::listPromptTemplatesByName(const std::string& tenant_id,
                                             const std::string& name) {
    std::vector<PromptTemplateRecord> result;
    if (!initialized_) return result;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return result;
    const char* sql =
        "SELECT id, tenant_id, name, content, version, weight, is_active, "
        "created_at, updated_at FROM prompt_templates "
        "WHERE tenant_id = $1 AND name = $2";
    const char* params[2] = { tenant_id.c_str(), name.c_str() };
    PGresult* res = PQexecParams(handle->get(), sql, 2, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i) result.push_back(pgRowToPromptTemplate(res, i));
    }
    PQclear(res);
    return result;
}

// ============================================================================
// TASK-20260604-01 P0-A — Rule Set（镜像 SQLite，含 at-most-one-active 语义）
// ============================================================================

bool PgPersistentStore::insertRuleSet(const std::string& tenant_id,
                                      const RuleSetRecord& record) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    if (record.is_active) {
        const char* deact = "UPDATE rule_sets SET is_active = 0 WHERE tenant_id = $1";
        const char* dp[1] = { tenant_id.c_str() };
        PGresult* dr = PQexecParams(handle->get(), deact, 1, nullptr, dp, nullptr, nullptr, 0);
        PQclear(dr);
    }

    const char* sql =
        "INSERT INTO rule_sets (tenant_id, version, rules_json, created_at, is_active) "
        "VALUES ($1, $2, $3, $4, $5)";
    std::string version = std::to_string(record.version);
    std::string active = record.is_active ? "1" : "0";
    const char* params[5] = {
        tenant_id.c_str(), version.c_str(), record.rules_json.c_str(),
        record.created_at.c_str(), active.c_str()
    };
    PGresult* res = PQexecParams(handle->get(), sql, 5, nullptr, params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
}

std::optional<PersistentStore::RuleSetRecord>
PgPersistentStore::getActiveRuleSet(const std::string& tenant_id) {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return std::nullopt;
    const char* sql =
        "SELECT version, rules_json, created_at, is_active "
        "FROM rule_sets WHERE tenant_id = $1 AND is_active = 1 LIMIT 1";
    const char* params[1] = { tenant_id.c_str() };
    PGresult* res = PQexecParams(handle->get(), sql, 1, nullptr, params, nullptr, nullptr, 0);
    std::optional<RuleSetRecord> out;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        RuleSetRecord r;
        r.tenant_id = tenant_id;
        r.version = std::stoll(PQgetvalue(res, 0, 0));
        r.rules_json = PQgetvalue(res, 0, 1);
        r.created_at = PQgetvalue(res, 0, 2);
        r.is_active = std::stoi(PQgetvalue(res, 0, 3)) != 0;
        out = std::move(r);
    }
    PQclear(res);
    return out;
}

std::vector<PersistentStore::RuleSetRecord>
PgPersistentStore::listRuleSetVersions(const std::string& tenant_id, int limit, int offset) {
    std::vector<RuleSetRecord> result;
    if (!initialized_) return result;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return result;
    const char* sql =
        "SELECT version, rules_json, created_at, is_active "
        "FROM rule_sets WHERE tenant_id = $1 ORDER BY version DESC LIMIT $2 OFFSET $3";
    std::string limit_str = std::to_string(limit);
    std::string offset_str = std::to_string(offset < 0 ? 0 : offset);
    const char* params[3] = { tenant_id.c_str(), limit_str.c_str(), offset_str.c_str() };
    PGresult* res = PQexecParams(handle->get(), sql, 3, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i) {
            RuleSetRecord r;
            r.tenant_id = tenant_id;
            r.version = std::stoll(PQgetvalue(res, i, 0));
            r.rules_json = PQgetvalue(res, i, 1);
            r.created_at = PQgetvalue(res, i, 2);
            r.is_active = std::stoi(PQgetvalue(res, i, 3)) != 0;
            result.push_back(std::move(r));
        }
    }
    PQclear(res);
    return result;
}

bool PgPersistentStore::activateRuleSetVersion(const std::string& tenant_id,
                                               int64_t version) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!handle) return false;

    const char* check_sql =
        "SELECT COUNT(*) FROM rule_sets WHERE tenant_id = $1 AND version = $2";
    std::string version_str = std::to_string(version);
    const char* cp[2] = { tenant_id.c_str(), version_str.c_str() };
    PGresult* cr = PQexecParams(handle->get(), check_sql, 2, nullptr, cp, nullptr, nullptr, 0);
    bool exists = (PQresultStatus(cr) == PGRES_TUPLES_OK && PQntuples(cr) > 0 &&
                   std::stoll(PQgetvalue(cr, 0, 0)) > 0);
    PQclear(cr);
    if (!exists) return false;

    const char* deact = "UPDATE rule_sets SET is_active = 0 WHERE tenant_id = $1";
    const char* dp[1] = { tenant_id.c_str() };
    PGresult* dr = PQexecParams(handle->get(), deact, 1, nullptr, dp, nullptr, nullptr, 0);
    PQclear(dr);

    const char* act =
        "UPDATE rule_sets SET is_active = 1 WHERE tenant_id = $1 AND version = $2";
    const char* ap[2] = { tenant_id.c_str(), version_str.c_str() };
    PGresult* ar = PQexecParams(handle->get(), act, 2, nullptr, ap, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(ar) == PGRES_COMMAND_OK);
    PQclear(ar);
    return ok;
}

} // namespace aegisgate
