#pragma once

// Phase 11.1 TASK-20260523-01 — Epic 4 GuardModelApplier.
//
// Concrete IApprovalApplier wired under AutonomySource::AdaptiveGuard.
// Translates an APPROVED proposal into a ModelRegistry promote / revert /
// register operation.
//
// Payload schema (D1=C / D8=A):
//   {
//     "action":      "promote_shadow_to_live"|"revert_to_previous"|"register_shadow",
//     "model_id":    "guardrail",
//     "version":     "v2.0.1",
//     "shadow_metrics": {
//       "win_rate":            0.62,
//       "shadow_duration_min": 60,
//       "fp_rate_delta":      -0.03
//     }
//   }
//
// isLowRisk enforces 4 rules (mirrors BanditAutonomyApplier pattern):
//   R1: action ∈ {promote_shadow_to_live, register_shadow}  (revert never auto)
//   R2: win_rate >= 0.55
//   R3: shadow_duration_min >= 60
//   R4: fp_rate_delta >= -0.10

// TASK-20260523-03 — migrated to BaseAutonomyApplier (GoF Template Method).
// SR17 layer 2 + timing wrapper + dry_run helper now live in the base.
// Note: pre-migration rollback() did NOT check SR17; post-migration it
// DOES (defense strengthening, no breaking-change since rollback was
// previously unreachable from production when SR17 was tripped).
#include "guardrail/model/i_guard_model_registry.h"
#include "observe/autonomy/base_autonomy_applier.h"

#include <memory>
#include <string>

namespace aegisgate::guard {

class GuardModelApplier : public aegisgate::autonomy::BaseAutonomyApplier {
public:
    explicit GuardModelApplier(std::shared_ptr<IGuardModelRegistry> registry);

    std::string applierName() const override { return "guard_model"; }

    bool isLowRisk(
        const aegisgate::autonomy::ApprovalProposal& p) const override;

protected:
    aegisgate::autonomy::ApplyResult applyImpl(
        const aegisgate::autonomy::ApprovalProposal& p,
        bool dry_run) override;
    aegisgate::autonomy::ApplyResult rollbackImpl(
        const aegisgate::autonomy::ApprovalProposal& p) override;

private:
    struct PayloadView {
        std::string action;
        std::string model_id;
        std::string version;
        double      win_rate            = 0.0;
        int         shadow_duration_min = 0;
        double      fp_rate_delta       = 0.0;
        bool        schema_ok           = false;
        std::string error;
    };
    static PayloadView extract(
        const aegisgate::autonomy::ApprovalProposal& p);

    std::shared_ptr<IGuardModelRegistry> registry_;
};

}  // namespace aegisgate::guard
