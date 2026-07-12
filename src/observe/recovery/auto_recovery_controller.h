#pragma once

// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 1 task 1.2.
//
// AutoRecoveryController — generic base for "collect signals → evaluate
// breach → trigger recovery" loops. Distilled from RolloutController
// auto-rollback path so future autonomy modules (Runbook actions, capacity
// triggers, ML router fallbacks, …) can share cooldown debouncing + SR17
// kill-switch enforcement without duplicating boilerplate.
//
// Design rationale: creative phase D1=A (inheritance refactor).
//   * Base owns cooldown + SR17 (cross-cutting, security-critical).
//   * Derived owns metric collection + breach criteria + recovery action
//     (domain-specific).
//
// Lock layering (systemPatterns):
//   cooldown_mu_ is Layer 3. Held only around the small map mutation —
//   callbacks are invoked outside the lock.

#include "common/clock.h"
#include "observe/recovery/signal_snapshot.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace aegisgate {

class AutoRecoveryController {
public:
    struct Deps {
        common::Clock* clock = nullptr;

        // SR17: returns true when autonomy decisions may execute. nullptr
        // means autonomy is enabled (default-on, matches RolloutController
        // auto_rollback_enabled semantics).
        std::function<bool()> autonomy_enabled;

        // Per-subject pause cooldown. Default 5 min mirrors
        // RolloutController::Deps::pause_cooldown_ms.
        std::int64_t cooldown_ms = 5 * 60 * 1000;
    };

    explicit AutoRecoveryController(Deps d);
    virtual ~AutoRecoveryController() = default;

    AutoRecoveryController(const AutoRecoveryController&) = delete;
    AutoRecoveryController& operator=(const AutoRecoveryController&) = delete;

    // Visible-for-testing helpers (also useful for Admin diagnostics).
    bool isCooldownActive(std::string_view subject, std::int64_t now_ms) const;
    bool isAutoRecoveryEnabled() const;

protected:
    // Domain hook 1 — gather the current signal window for a subject.
    // Window length is up to the derived class.
    virtual SignalSnapshot collectMetrics(std::string_view subject,
                                           std::chrono::seconds window) = 0;

    // Domain hook 2 — decide whether the gathered signals constitute a
    // breach. Derived classes own the threshold semantics (absolute /
    // relative / multi-source / …).
    virtual BreachVerdict evaluateBreach(std::string_view subject,
                                          const SignalSnapshot& sig,
                                          const SignalSnapshot& baseline) = 0;

    // Domain hook 3 — perform the recovery (rollout PauseAuto, Runbook
    // apply, ApprovalProposal propose, …). Invoked outside the cooldown
    // lock.
    virtual void triggerRecovery(std::string_view subject,
                                   const BreachVerdict& verdict) = 0;

    // Drive the loop for a single subject. Skips work when SR17 disables
    // autonomy or the subject is in cooldown. On breach, records the
    // cooldown timestamp before delegating to triggerRecovery.
    void evaluate(std::string_view subject,
                   std::int64_t steady_now_ms,
                   std::int64_t wall_now_ms);

    Deps deps_;

private:
    mutable std::mutex cooldown_mu_;  // Layer 3
    std::unordered_map<std::string, std::int64_t> last_trigger_ms_;
};

} // namespace aegisgate
