#pragma once

// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic B.3.
//
// Pure-function rollout state machine. The transition table below is the
// authoritative source of truth for legal state progressions. All callers
// (RolloutController handlers + Ticker auto-pause/rollback path) MUST go
// through attemptTransition() so no call-site can forge a transition that
// bypasses the table.
//
// Design reference: docs/specs/2026-04-22-phase9.3.4-rollout-controller-design.md §3.4.

#include "control_plane/rollout/rollout_record.h"
#include <optional>

namespace aegisgate {

// User- or Ticker-triggered stimulus applied to a rollout.
enum class RolloutAction {
    Start,         // PENDING        → PROGRESSING (operator explicit start)
    Promote,       // PROGRESSING    → PROGRESSING (advance) / COMPLETED (last)
    PauseManual,   // PROGRESSING    → PAUSED (operator pause)
    PauseAuto,     // PROGRESSING    → PAUSED (metric breach, SR threshold)
    Resume,        // PAUSED         → PROGRESSING (operator resume)
    AutoRollback,  // PAUSED         → FAILED     (grace expired, auto_rollback)
    Abort,         // PENDING/PROGRESSING/PAUSED → ABORTED (operator abort)
    Fail,          // PROGRESSING/PAUSED         → FAILED  (config_core rollback error)
};

// Input context for a single transition attempt.
struct RolloutTransitionInput {
    RolloutStatus from = RolloutStatus::PENDING;
    RolloutAction action = RolloutAction::Start;
    bool is_last_stage = false;  // only consulted when action == Promote
};

// Returns the destination status if the transition is legal, or std::nullopt
// if rejected. noexcept and free of side effects — audit logging / store
// writes are the caller's responsibility.
std::optional<RolloutStatus>
attemptRolloutTransition(const RolloutTransitionInput& in) noexcept;

// String view of the action (for audit events / logs / tests).
const char* rolloutActionToString(RolloutAction a) noexcept;

} // namespace aegisgate
