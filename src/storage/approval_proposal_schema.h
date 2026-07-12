#pragma once

// TASK-20260518-02 Phase 11.5 — Epic 1.0
//
// Shared SQL helpers for backends that natively store approval proposals
// (SqlitePersistentStore + PgPersistentStore). The Memory backend does
// not use these helpers (it keeps POCO refs directly) but the constants
// live in the same namespace so the PG backend can adapt the SQL text
// verbatim.

#include <string>

namespace aegisgate {

// --------- Schema text (SQLite syntax) -------------------------------------
//
// Mirrors design spec §10 + creative C5 §1.3. Single table tracks the full
// lifecycle of an autonomy proposal. payload / decision_trace are stored
// as TEXT (JSON-encoded); SQLite is JSON-aware enough for our needs.
//
// Indexes optimize the dominant query (FinOps UI filtering by state /
// source ordered by proposed_at_ms DESC).
inline constexpr const char* kApprovalProposalsSchemaSql = R"(
    CREATE TABLE IF NOT EXISTS autonomy_proposals (
        id                    TEXT    PRIMARY KEY,
        source                TEXT    NOT NULL,
        subject               TEXT    NOT NULL DEFAULT '',
        payload_json          TEXT    NOT NULL DEFAULT '{}',
        decision_trace_json   TEXT    NOT NULL DEFAULT '{}',
        proposed_at_ms        INTEGER NOT NULL,
        proposer_user_id      TEXT    NOT NULL DEFAULT 'system',
        state                 TEXT    NOT NULL DEFAULT 'PROPOSED',
        reviewer_user_id      TEXT    NOT NULL DEFAULT '',
        reviewed_at_ms        INTEGER NOT NULL DEFAULT 0,
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

// --------- Schema text (PostgreSQL syntax) ---------------------------------
//
// Differs from SQLite only in: BIGINT vs INTEGER for *_at_ms columns, and
// JSONB type for payload / decision_trace to enable native JSON ops if
// downstream tooling cares.
inline constexpr const char* kApprovalProposalsSchemaSqlPg = R"(
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

} // namespace aegisgate
