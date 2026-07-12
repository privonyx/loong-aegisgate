#pragma once

// Phase 11.2 TASK-20260521-03 — BanditAutonomyApplier.
//
// Concrete IApprovalApplier that registers under AutonomySource::BanditRouter.
// Translates an APPROVED proposal into a BanditRouter mode transition
// (shadow_to_live / expand_canary / revert_to_shadow).
//
// Payload schema:
//   {
//     "action":              "shadow_to_live"|"expand_canary"|"revert_to_shadow",
//     "strategy":            "cost-first"|"quality-first"|"hybrid"|...,
//     "canary_pct":          0.05 | 0.50 | 1.0,
//     "shadow_metrics": {
//       "win_rate":            0.62,
//       "shadow_duration_min": 60,
//       "cost_delta_pct":     -8
//     }
//   }
//
// isLowRisk implements the 4 R rules per design §4.5 / spec §5:
//   R1: win_rate >= 0.55           (statistically beats baseline)
//   R2: shadow_duration_min >= 60  (≥1h shadow required)
//   R3: canary_pct <= 0.05          (start small)
//   R4: cost_delta_pct >= -20       (no single >20% cost swing)

#include "observe/autonomy/base_autonomy_applier.h"

#include <memory>
#include <string>

namespace aegisgate {
class BanditRouter;
}

namespace aegisgate::autonomy {

// TASK-20260523-03 — migrated to BaseAutonomyApplier (GoF Template Method).
// SR17 layer 2 + timing wrapper + dry_run helper now live in the base.
// Note: the base calls AutonomyApprovalWorkflow::isAutonomyEnabled(); the
// pre-migration BanditRouter::isAutonomyEnabled() was an alias for the
// same env (AEGISGATE_DISABLE_AUTONOMY) — semantically equivalent.
class BanditAutonomyApplier : public BaseAutonomyApplier {
public:
    explicit BanditAutonomyApplier(std::shared_ptr<BanditRouter> router);

    std::string applierName() const override { return "bandit_autonomy"; }

    // SR2 — 4-rule whitelist mirrored from CostAutonomyApplier pattern.
    bool isLowRisk(const ApprovalProposal& p) const override;

protected:
    ApplyResult applyImpl(const ApprovalProposal& p, bool dry_run) override;
    ApplyResult rollbackImpl(const ApprovalProposal& p) override;

private:
    struct PayloadView {
        std::string action;
        std::string strategy;
        double      canary_pct          = 0.0;
        double      win_rate            = 0.0;
        int         shadow_duration_min = 0;
        double      cost_delta_pct      = 0.0;
        bool        schema_ok           = false;
        std::string error;
    };
    static PayloadView extract(const ApprovalProposal& p);

    std::shared_ptr<BanditRouter> router_;
};

}  // namespace aegisgate::autonomy
