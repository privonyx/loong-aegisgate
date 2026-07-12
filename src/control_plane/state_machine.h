#pragma once

// Phase 9.3 Epic 3 Task 3.1 — W3 dual-approval state machine.
//
// Thin declarative transition table enforcing the §5.2 matrix from
// docs/specs/2026-04-20-phase9.3-control-plane-design.md. Lives in
// src/control_plane/ as pure C++ with no gRPC dependency so it is always
// compiled (both ENABLE_CONTROL_PLANE=OFF and =ON paths).
//
// The state machine is pure: actor identity, SR5 submitter!=reviewer, SR10
// rate limiting and audit logging are enforced by ConfigServiceCore which
// consults StateMachine::canTransition / next to decide the target state.

#include "control_plane/config_version_record.h"

#include <optional>

namespace aegisgate {

enum class ConfigAction {
    APPROVE,
    REJECT,
    ACTIVATE,
    ROLLBACK_TO,
};

class StateMachine {
public:
    // Returns the resulting status when the transition is legal, std::nullopt
    // otherwise. Does NOT check actor identity or timing constraints.
    static std::optional<ConfigStatus> next(ConfigStatus from, ConfigAction action);

    static bool canTransition(ConfigStatus from, ConfigAction action) {
        return next(from, action).has_value();
    }
};

} // namespace aegisgate
