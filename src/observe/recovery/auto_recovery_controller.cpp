// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 1 task 1.2.

#include "observe/recovery/auto_recovery_controller.h"

namespace aegisgate {

AutoRecoveryController::AutoRecoveryController(Deps d) : deps_(std::move(d)) {}

bool AutoRecoveryController::isAutoRecoveryEnabled() const {
    if (!deps_.autonomy_enabled) return true;
    return deps_.autonomy_enabled();
}

bool AutoRecoveryController::isCooldownActive(std::string_view subject,
                                                std::int64_t now_ms) const {
    std::lock_guard<std::mutex> g(cooldown_mu_);
    auto it = last_trigger_ms_.find(std::string(subject));
    if (it == last_trigger_ms_.end()) return false;
    return (now_ms - it->second) < deps_.cooldown_ms;
}

void AutoRecoveryController::evaluate(std::string_view subject,
                                        std::int64_t steady_now_ms,
                                        std::int64_t /*wall_now_ms*/) {
    // SR17 short-circuit (M2 mutation target).
    if (!isAutoRecoveryEnabled()) {
        return;
    }

    // Per-subject cooldown (M1 mutation target).
    if (isCooldownActive(subject, steady_now_ms)) {
        return;
    }

    SignalSnapshot baseline; // derived class may ignore; left default.
    SignalSnapshot snap = collectMetrics(subject, std::chrono::seconds{60});

    BreachVerdict verdict = evaluateBreach(subject, snap, baseline);
    if (!verdict.breached) {
        return;
    }

    {
        std::lock_guard<std::mutex> g(cooldown_mu_);
        last_trigger_ms_[std::string(subject)] = steady_now_ms;
    }

    triggerRecovery(subject, verdict);
}

} // namespace aegisgate
