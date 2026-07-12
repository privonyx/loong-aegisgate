#pragma once

// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic A.5.
//
// Shared helpers for every persistent backend that natively stores
// rollouts: SQL schema, proto wire-value mapping, and RolloutSpec
// JSON serialization (BLOB column `spec_json`). The Memory backend
// does not use these helpers (it keeps POCO refs directly) but
// intentionally lives in the same namespace so a future PG backend
// can reuse both the SQL text (adapted) and the serializer verbatim.
//
// Integer wire values MUST match proto enum values in
// api/control-plane/proto/control_plane/v1/control_plane.proto so that downstream
// gRPC paths can trust them; the partial UNIQUE INDEX relies on
// these numbers being stable.

#include "control_plane/rollout/rollout_record.h"
#include <string>

namespace aegisgate {

// --------- Schema text -----------------------------------------------------
// The schema mirrors design spec §5.1 verbatim. The FK on config_versions
// is advisory only — SQLite does not enforce foreign keys unless
// PRAGMA foreign_keys=ON, which we deliberately leave off to preserve
// existing write paths (Epic A backends coexist with tests that insert
// rollouts without a companion config_versions row).
inline constexpr const char* kRolloutsSchemaSql = R"(
    CREATE TABLE IF NOT EXISTS rollouts (
        rollout_id                 TEXT    PRIMARY KEY,
        target_version_id          TEXT    NOT NULL,
        previous_active_version_id TEXT    NOT NULL DEFAULT '',
        spec_json                  BLOB    NOT NULL,
        status                     INTEGER NOT NULL,
        current_stage_index        INTEGER NOT NULL DEFAULT 0,
        started_at                 INTEGER NOT NULL DEFAULT 0,
        stage_started_at           INTEGER NOT NULL DEFAULT 0,
        paused_at                  INTEGER NOT NULL DEFAULT 0,
        pause_reason               INTEGER NOT NULL DEFAULT 0,
        pause_detail               TEXT    NOT NULL DEFAULT '',
        creator                    TEXT    NOT NULL,
        last_actor                 TEXT    NOT NULL DEFAULT '',
        completed_at               INTEGER NOT NULL DEFAULT 0,
        chain_hash                 TEXT    NOT NULL DEFAULT ''
    );

    -- SR14 defense-in-depth: at-most-one active rollout per target.
    CREATE UNIQUE INDEX IF NOT EXISTS rollouts_one_active_per_target
        ON rollouts(target_version_id)
        WHERE status IN (1, 2, 3);  -- PENDING, PROGRESSING, PAUSED

    CREATE INDEX IF NOT EXISTS rollouts_started_at_idx
        ON rollouts(started_at DESC);

    CREATE TABLE IF NOT EXISTS rollout_stage_events (
        event_id     TEXT    PRIMARY KEY,
        rollout_id   TEXT    NOT NULL,
        stage_index  INTEGER NOT NULL,
        event_type   TEXT    NOT NULL,
        reason       TEXT    NOT NULL DEFAULT '',
        metrics_json BLOB,
        at_millis    INTEGER NOT NULL,
        actor        TEXT    NOT NULL
    );

    CREATE INDEX IF NOT EXISTS rollout_stage_events_by_rollout
        ON rollout_stage_events(rollout_id, at_millis);
)";

// --------- Enum wire-value conversions -------------------------------------

inline int rolloutStatusToWire(RolloutStatus s) {
    switch (s) {
        case RolloutStatus::PENDING:     return 1;
        case RolloutStatus::PROGRESSING: return 2;
        case RolloutStatus::PAUSED:      return 3;
        case RolloutStatus::COMPLETED:   return 4;
        case RolloutStatus::FAILED:      return 5;
        case RolloutStatus::ABORTED:     return 6;
    }
    return 0;
}

inline RolloutStatus rolloutStatusFromWire(int v) {
    switch (v) {
        case 1: return RolloutStatus::PENDING;
        case 2: return RolloutStatus::PROGRESSING;
        case 3: return RolloutStatus::PAUSED;
        case 4: return RolloutStatus::COMPLETED;
        case 5: return RolloutStatus::FAILED;
        case 6: return RolloutStatus::ABORTED;
        default: return RolloutStatus::PENDING;
    }
}

inline int pauseReasonToWire(PauseReason r) {
    switch (r) {
        case PauseReason::UNSPECIFIED:   return 0;
        case PauseReason::MANUAL:        return 1;
        case PauseReason::ERROR_RATE:    return 2;
        case PauseReason::LATENCY_RATIO: return 3;
        case PauseReason::AUTO_ROLLBACK: return 4;
    }
    return 0;
}

inline PauseReason pauseReasonFromWire(int v) {
    switch (v) {
        case 1: return PauseReason::MANUAL;
        case 2: return PauseReason::ERROR_RATE;
        case 3: return PauseReason::LATENCY_RATIO;
        case 4: return PauseReason::AUTO_ROLLBACK;
        default: return PauseReason::UNSPECIFIED;
    }
}

// --------- RolloutSpec JSON serialization ----------------------------------
// Implemented in rollout_schema.cpp (header stays nlohmann-free so the
// POCO header doesn't leak a JSON dep into the control-plane hot path).
std::string  serializeRolloutSpec(const RolloutSpec& spec);
RolloutSpec  parseRolloutSpec(const std::string& json_str);

} // namespace aegisgate
