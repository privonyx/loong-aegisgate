#pragma once

// RolloutRecord — POCO mirror of control_plane.v1.Rollout.
//
// Lives under src/control_plane/rollout/ (sibling of ConfigVersionRecord) and
// is intentionally free of any proto/gRPC dependency so the rollout business
// core and persistence layer can be compiled regardless of
// ENABLE_CONTROL_PLANE. Only src/control_plane/grpc/ requires the generated
// proto code.
//
// Phase 9.3.4 MVP (TASK-20260422-01). Field ordering and enum values must
// stay in lock-step with:
//   - api/control-plane/proto/control_plane/v1/control_plane.proto::Rollout
//   - docs/specs/2026-04-22-phase9.3.4-rollout-controller-design.md §4-5
//   - src/storage/rollout_schema.h (SQLite schema, delivered in Epic A.5)

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aegisgate {

// Lifecycle of a rollout. Mirrors proto RolloutStatus.
//
// Transitions (enforced by RolloutStateMachine, Epic B.3):
//   PENDING     -- start    --> PROGRESSING
//   PROGRESSING -- promote (non-last) --> PROGRESSING (stage advance)
//   PROGRESSING -- promote (last)     --> COMPLETED   (terminal)
//   PROGRESSING -- pause    --> PAUSED
//   PROGRESSING -- abort    --> ABORTED               (terminal)
//   PAUSED      -- resume   --> PROGRESSING
//   PAUSED      -- auto_rollback --> FAILED           (terminal; SR17 gated)
//   PAUSED      -- abort    --> ABORTED
enum class RolloutStatus {
    PENDING,
    PROGRESSING,
    PAUSED,
    COMPLETED,
    FAILED,
    ABORTED,
};

// Reason a rollout entered PAUSED. UNSPECIFIED while PROGRESSING/terminal.
enum class PauseReason {
    UNSPECIFIED,
    MANUAL,
    ERROR_RATE,
    LATENCY_RATIO,
    AUTO_ROLLBACK,
};

// Selector describing which requests a stage applies to.
// Semantics: tenant_globs AND regions AND percentage-bucket.
// An empty selector matches every request ("100%" within the stage).
struct ScopeSelector {
    std::vector<std::string> tenant_globs;  // fnmatch-style globs
    std::vector<std::string> regions;       // exact-match region names
    int                      percentage = 0; // 0..100
};

// Gate to advance past the current stage.
struct ObservationPolicy {
    int min_duration_seconds = 0;
    int min_sample_count     = 0;
};

// Thresholds that, once exceeded, trigger automatic pause.
struct AutoPausePolicy {
    double error_rate_gt              = 0.0;  // relative: target - baseline
    double p99_latency_ratio_gt       = 0.0;  // relative: target / baseline
    double absolute_error_rate_gt     = 0.0;  // safety net
    double absolute_p99_latency_ms_gt = 0.0;  // safety net
};

struct RolloutStageRecord {
    std::string       name;
    ScopeSelector     scope;
    ObservationPolicy observation;
    AutoPausePolicy   auto_pause;
};

// Immutable user-supplied rollout specification. Defaults mirror design §4.2.
struct RolloutSpec {
    std::string                     target_version_id;
    std::vector<RolloutStageRecord> stages;
    std::string                     sticky_key = "tenant_id";
    bool                            auto_rollback_on_pause = true;
    int                             auto_rollback_grace_seconds = 1800;
    std::string                     creator_comment;
};

// Server-maintained rollout state.
struct RolloutRecord {
    std::string  rollout_id;                  // ULID (26 chars)
    std::string  target_version_id;
    std::string  previous_active_version_id;  // captured on Start
    RolloutSpec  spec;

    RolloutStatus status = RolloutStatus::PENDING;
    int          current_stage_index = 0;
    std::int64_t started_at = 0;              // unix millis; 0 until Started
    std::int64_t stage_started_at = 0;        // resets on advance/Resume
    std::int64_t paused_at = 0;
    PauseReason  pause_reason = PauseReason::UNSPECIFIED;
    std::string  pause_detail;                // human-readable (SR8)

    std::string  creator;
    std::string  last_actor;
    std::int64_t completed_at = 0;            // terminal states only

    std::string  chain_hash;                  // shared AuditLogger head
};

// Append-only event log row. Mirrors rollout_stage_events table.
struct RolloutStageEvent {
    std::string  event_id;         // ULID
    std::string  rollout_id;
    int          stage_index = 0;
    std::string  event_type;       // "entered"|"promoted"|"paused_auto"|"paused_manual"|...
    std::string  reason;
    std::string  metrics_json;     // opaque JSON (empty for non-metric events)
    std::int64_t at_millis = 0;
    std::string  actor;
};

// Query parameters for listRollouts. Empty fields mean "unbounded".
struct RolloutQuery {
    std::vector<RolloutStatus> statuses;    // empty = every status
    int                        limit = 50;  // server-side caps at 500
    std::string                page_token;  // opaque cursor
};

// -- Enum string conversions (kept inline alongside POCO, mirrors
// -- config_version_record.h style).

inline const char* rolloutStatusToString(RolloutStatus s) {
    switch (s) {
        case RolloutStatus::PENDING:     return "PENDING";
        case RolloutStatus::PROGRESSING: return "PROGRESSING";
        case RolloutStatus::PAUSED:      return "PAUSED";
        case RolloutStatus::COMPLETED:   return "COMPLETED";
        case RolloutStatus::FAILED:      return "FAILED";
        case RolloutStatus::ABORTED:     return "ABORTED";
    }
    return "UNKNOWN";
}

inline std::optional<RolloutStatus> rolloutStatusFromString(const std::string& s) {
    if (s == "PENDING")     return RolloutStatus::PENDING;
    if (s == "PROGRESSING") return RolloutStatus::PROGRESSING;
    if (s == "PAUSED")      return RolloutStatus::PAUSED;
    if (s == "COMPLETED")   return RolloutStatus::COMPLETED;
    if (s == "FAILED")      return RolloutStatus::FAILED;
    if (s == "ABORTED")     return RolloutStatus::ABORTED;
    return std::nullopt;
}

inline const char* pauseReasonToString(PauseReason r) {
    switch (r) {
        case PauseReason::UNSPECIFIED:   return "UNSPECIFIED";
        case PauseReason::MANUAL:        return "MANUAL";
        case PauseReason::ERROR_RATE:    return "ERROR_RATE";
        case PauseReason::LATENCY_RATIO: return "LATENCY_RATIO";
        case PauseReason::AUTO_ROLLBACK: return "AUTO_ROLLBACK";
    }
    return "UNKNOWN";
}

inline std::optional<PauseReason> pauseReasonFromString(const std::string& s) {
    if (s == "UNSPECIFIED")   return PauseReason::UNSPECIFIED;
    if (s == "MANUAL")        return PauseReason::MANUAL;
    if (s == "ERROR_RATE")    return PauseReason::ERROR_RATE;
    if (s == "LATENCY_RATIO") return PauseReason::LATENCY_RATIO;
    if (s == "AUTO_ROLLBACK") return PauseReason::AUTO_ROLLBACK;
    return std::nullopt;
}

} // namespace aegisgate
