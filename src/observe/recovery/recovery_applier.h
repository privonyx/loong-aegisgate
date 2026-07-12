#pragma once

// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 1 task 1.5.
//
// RecoveryApplier — IApprovalApplier implementation for
// AutonomySource::AutoRecovery proposals. Dispatches on payload["action"]
// to one of 7 handlers:
//
//   active mutation:
//     * override_quality_tier  — MLRouter::overrideQualityTier(tenant,tier)
//     * apply_budget_cap       — BudgetGuardStage::setConfig(new cap)
//
//   advisory (audit + spdlog::warn only — no production-state mutation):
//     * switch_router_fallback — operator should run failover playbook
//     * switch_connector       — operator should patch ConnectorRegistry
//     * propose_hpa_scale      — operator should kubectl scale (YAGNI:
//                                  v1 does not embed a K8s client)
//     * send_webhook           — v1 stub; v2 will pipe to CurlUpstreamClient
//     * audit_only             — no-op effect, only writes audit trail
//
// All 7 paths share three guarantees:
//   1. dry_run mode never mutates production state (SR2).
//   2. Every apply()/rollback() writes an audit entry with a deterministic
//      action label ("auto_recovery.apply.<action>").
//   3. isLowRisk() returns true only for the two active-mutation actions
//      whose payload sits within the conservative bounds (T08 defence).
//
// Design references:
//   memory-bank/creative/creative-phase11.4-rca-design.md §4.5
//   docs/plans/2026-05-19-phase11.4-self-healing-ops.md §3 Task 1.5

// TASK-20260523-03 — migrated to BaseAutonomyApplier (GoF Template Method).
// SR17 layer 2 + timing wrapper now live in the base. Pre-migration
// RecoveryApplier had NO SR17 layer 2 check; post-migration it does,
// which is a net defense-strengthening (5/5 appliers now uniformly
// protected; Recovery was the last hold-out alongside Cost).
#include "observe/autonomy/base_autonomy_applier.h"

#include <memory>
#include <string>
#include <vector>

namespace aegisgate {
class MLRouter;
class BudgetGuardStage;
class AuditLogger;
} // namespace aegisgate

namespace aegisgate::autonomy {

class RecoveryApplier : public BaseAutonomyApplier {
public:
    struct Deps {
        std::shared_ptr<MLRouter>         router;        // required for override_quality_tier
        std::shared_ptr<BudgetGuardStage> budget_guard;  // required for apply_budget_cap
        std::shared_ptr<AuditLogger>      audit;         // required (SR2 audit)
    };

    explicit RecoveryApplier(Deps deps);

    std::string applierName() const override { return "recovery"; }

    // Conservative whitelist:
    //   override_quality_tier: same rules as CostAutonomyApplier
    //   apply_budget_cap:      only when new_cap >= 50% of current_cap
    //   else                   high risk
    bool isLowRisk(const ApprovalProposal& p) const override;

    // Visible for /admin metrics: list of action strings RecoveryApplier
    // knows about. Used by the action_type allow-list check.
    static const std::vector<std::string>& knownActions();

protected:
    ApplyResult applyImpl(const ApprovalProposal& p, bool dry_run) override;
    ApplyResult rollbackImpl(const ApprovalProposal& p) override;

private:
    static std::string extractAction(const ApprovalProposal& p);

    // Handlers; each returns an ApplyResult and writes its own audit entry
    // (or returns failure before audit when schema fails).
    ApplyResult handleOverrideQualityTier(const ApprovalProposal& p, bool dry_run);
    ApplyResult rollbackOverrideQualityTier(const ApprovalProposal& p);
    ApplyResult handleApplyBudgetCap(const ApprovalProposal& p, bool dry_run);
    ApplyResult rollbackApplyBudgetCap(const ApprovalProposal& p);
    ApplyResult handleAdvisory(const ApprovalProposal& p,
                                 const std::string& action,
                                 bool dry_run);

    void writeAudit(const ApprovalProposal& p,
                     const std::string& action,
                     const std::string& detail);

    Deps deps_;
};

} // namespace aegisgate::autonomy
