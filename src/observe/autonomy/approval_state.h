#pragma once

// Phase 11.5 AutonomyApprovalWorkflow (TASK-20260518-02) — Epic 1.1.
//
// ApprovalState — 5-state machine driving every autonomy proposal lifecycle.
//   PROPOSED -> APPROVED -> APPLIED
//             -> REJECTED
//             -> ROLLED_BACK   (apply failed and rollback fired; design C1)
//
// AutonomySource — routing key that lets AutonomyApprovalWorkflow dispatch
// apply() to the right IApprovalApplier. Each Phase 11.x sub-phase registers
// one applier under its source enum:
//   - CostOptimizer   (Phase 11.5, this task)
//   - AutoRecovery    (Phase 11.4)
//   - BanditRouter    (Phase 11.2)
//   - AdaptiveGuard   (Phase 11.1)
//   - Workflow        (Phase 11.3)
//
// String forms are wire-stable: they appear verbatim in `state` / `source`
// columns of the autonomy_proposals table (see approval_proposal_schema.h)
// and in audit log entries. Renaming any of them is a breaking change.
//
// Implementation lives header-only with `inline` linkage so the enum
// utilities stay zero-cost to embed (no extra .cpp added to aegisgate_core).
// The header has no deps on JSON / spdlog / std::function — keeping it cheap
// enough to include from PersistentStore base / public CLI helpers.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace aegisgate::autonomy {

enum class ApprovalState {
    PROPOSED    = 0,
    APPROVED    = 1,
    APPLIED     = 2,
    REJECTED    = 3,
    ROLLED_BACK = 4,
};

enum class AutonomySource {
    CostOptimizer = 0,
    AutoRecovery  = 1,
    BanditRouter  = 2,
    AdaptiveGuard = 3,
    Workflow      = 4,
};

// --- ApprovalState <-> string ----------------------------------------------

inline std::string toString(ApprovalState s) {
    switch (s) {
        case ApprovalState::PROPOSED:    return "PROPOSED";
        case ApprovalState::APPROVED:    return "APPROVED";
        case ApprovalState::APPLIED:     return "APPLIED";
        case ApprovalState::REJECTED:    return "REJECTED";
        case ApprovalState::ROLLED_BACK: return "ROLLED_BACK";
    }
    return "UNKNOWN";  // defensive — unreachable for a well-formed enum
}

inline std::optional<ApprovalState> approvalStateFromString(std::string_view s) {
    if (s == "PROPOSED")    return ApprovalState::PROPOSED;
    if (s == "APPROVED")    return ApprovalState::APPROVED;
    if (s == "APPLIED")     return ApprovalState::APPLIED;
    if (s == "REJECTED")    return ApprovalState::REJECTED;
    if (s == "ROLLED_BACK") return ApprovalState::ROLLED_BACK;
    return std::nullopt;
}

// --- AutonomySource <-> string ---------------------------------------------

inline std::string toString(AutonomySource s) {
    switch (s) {
        case AutonomySource::CostOptimizer: return "CostOptimizer";
        case AutonomySource::AutoRecovery:  return "AutoRecovery";
        case AutonomySource::BanditRouter:  return "BanditRouter";
        case AutonomySource::AdaptiveGuard: return "AdaptiveGuard";
        case AutonomySource::Workflow:      return "Workflow";
    }
    return "UNKNOWN";
}

inline std::optional<AutonomySource> autonomySourceFromString(std::string_view s) {
    if (s == "CostOptimizer") return AutonomySource::CostOptimizer;
    if (s == "AutoRecovery")  return AutonomySource::AutoRecovery;
    if (s == "BanditRouter")  return AutonomySource::BanditRouter;
    if (s == "AdaptiveGuard") return AutonomySource::AdaptiveGuard;
    if (s == "Workflow")      return AutonomySource::Workflow;
    return std::nullopt;
}

} // namespace aegisgate::autonomy
