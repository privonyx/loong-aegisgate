#pragma once

// Phase 11.1 TASK-20260523-01 — Epic 1.3 SQLite schema for the guard model
// registry. Kept in its own header so test code can also reference the SQL
// string for raw-SQL constraint testing (A11 data-infra TDD nuance).

namespace aegisgate::guard {

inline constexpr const char* kGuardModelTableDDL = R"SQL(
CREATE TABLE IF NOT EXISTS guard_models (
    model_id TEXT NOT NULL,
    version TEXT NOT NULL,
    path TEXT NOT NULL,
    classifier_threshold REAL NOT NULL DEFAULT 0.5,
    status TEXT NOT NULL CHECK (status IN ('shadow','live','retired')),
    promoted_at_ms INTEGER NOT NULL DEFAULT 0,
    artifact_sha256 TEXT NOT NULL,
    metrics_summary TEXT NOT NULL DEFAULT '{}',
    PRIMARY KEY (model_id, version)
);
)SQL";

inline constexpr const char* kGuardModelLiveUniqueDDL = R"SQL(
CREATE UNIQUE INDEX IF NOT EXISTS guard_models_one_live
ON guard_models(model_id)
WHERE status = 'live';
)SQL";

}  // namespace aegisgate::guard
