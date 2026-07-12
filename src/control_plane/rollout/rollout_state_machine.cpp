#include "control_plane/rollout/rollout_state_machine.h"

namespace aegisgate {

const char* rolloutActionToString(RolloutAction a) noexcept {
    switch (a) {
        case RolloutAction::Start:        return "Start";
        case RolloutAction::Promote:      return "Promote";
        case RolloutAction::PauseManual:  return "PauseManual";
        case RolloutAction::PauseAuto:    return "PauseAuto";
        case RolloutAction::Resume:       return "Resume";
        case RolloutAction::AutoRollback: return "AutoRollback";
        case RolloutAction::Abort:        return "Abort";
        case RolloutAction::Fail:         return "Fail";
    }
    return "Unknown";
}

std::optional<RolloutStatus>
attemptRolloutTransition(const RolloutTransitionInput& in) noexcept {
    using S = RolloutStatus;
    using A = RolloutAction;

    switch (in.from) {
        case S::PENDING:
            switch (in.action) {
                case A::Start: return S::PROGRESSING;
                case A::Abort: return S::ABORTED;
                default:       return std::nullopt;
            }
        case S::PROGRESSING:
            switch (in.action) {
                case A::Promote:
                    return in.is_last_stage ? S::COMPLETED : S::PROGRESSING;
                case A::PauseManual: return S::PAUSED;
                case A::PauseAuto:   return S::PAUSED;
                case A::Abort:       return S::ABORTED;
                case A::Fail:        return S::FAILED;
                default:             return std::nullopt;
            }
        case S::PAUSED:
            switch (in.action) {
                case A::Resume:       return S::PROGRESSING;
                case A::AutoRollback: return S::FAILED;
                case A::Abort:        return S::ABORTED;
                case A::Fail:         return S::FAILED;
                default:              return std::nullopt;
            }
        case S::COMPLETED:
        case S::FAILED:
        case S::ABORTED:
            return std::nullopt;  // terminal — reject everything
    }
    return std::nullopt;
}

} // namespace aegisgate
