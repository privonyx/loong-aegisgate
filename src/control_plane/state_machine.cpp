#include "control_plane/state_machine.h"

namespace aegisgate {

namespace {

struct Transition {
    ConfigStatus from;
    ConfigAction action;
    ConfigStatus to;
};

// Declarative §5.2 matrix. Order does not matter; the table is tiny so linear
// scan is faster than any hashmap and gives compile-time verifiable coverage.
constexpr Transition kTransitions[] = {
    // PENDING
    {ConfigStatus::PENDING,    ConfigAction::APPROVE,     ConfigStatus::APPROVED},
    {ConfigStatus::PENDING,    ConfigAction::REJECT,      ConfigStatus::REJECTED},

    // APPROVED
    {ConfigStatus::APPROVED,   ConfigAction::ACTIVATE,    ConfigStatus::ACTIVE},
    {ConfigStatus::APPROVED,   ConfigAction::REJECT,      ConfigStatus::REJECTED},

    // ACTIVE — rollback to self is an idempotent no-op that ConfigServiceCore
    // returns success for without touching the store (since the invariant
    // already holds).
    {ConfigStatus::ACTIVE,     ConfigAction::ROLLBACK_TO, ConfigStatus::ACTIVE},

    // SUPERSEDED — R2 exemption reactivates a previously-active bundle.
    {ConfigStatus::SUPERSEDED, ConfigAction::ROLLBACK_TO, ConfigStatus::ACTIVE},
};

} // namespace

std::optional<ConfigStatus> StateMachine::next(ConfigStatus from,
                                               ConfigAction action) {
    for (const auto& t : kTransitions) {
        if (t.from == from && t.action == action) {
            return t.to;
        }
    }
    return std::nullopt;
}

} // namespace aegisgate
