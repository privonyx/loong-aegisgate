#pragma once

// Phase 11.5 AutonomyApprovalWorkflow (TASK-20260518-02) — Epic 2.2.
//
// CostAutonomyApplier is the first concrete IApprovalApplier wired into
// AutonomyApprovalWorkflow. It translates an APPROVED proposal coming
// from CostOptimizer v2 into a per-tenant MLRouter quality_tier override
// (and reverses the change on rollback).
//
// Payload schema (mirrors CostOptimizer::proposeRecommendations output):
//   {
//     "action":                      "override_quality_tier",
//     "tenant_id":                   "tenant-A",
//     "current_model":               "gpt-4o",
//     "recommended_model":           "gpt-4o-mini",
//     "from_quality_tier":           "premium" | "standard" | "economy",
//     "to_quality_tier":             "standard" | "economy",
//     "estimated_savings_usd_24h":   12.5,
//     "affected_requests_per_hour":  800
//   }
//
// isLowRisk is the C2 /creative decision encoded as 4 if-then-else rules
// (R1: only downgrade, R2: no double-step, R3: ≤$50 savings, R4: ≤1000 RPS).
// AutonomyApprovalWorkflow under auto_apply="auto_low_risk" mode invokes
// this to decide whether to auto-approve a freshly proposed item.
//
// TASK-20260523-03 — migrated to BaseAutonomyApplier (GoF Template Method).
// SR17 layer 2 + timing wrapper + dry_run short-circuit now live in the
// base. CostAutonomyApplier overrides applyImpl()/rollbackImpl() with
// the MLRouter quality-tier business logic only.

#include "observe/autonomy/base_autonomy_applier.h"

#include <memory>
#include <string>

namespace aegisgate {
class MLRouter;  // forward; full header pulled only into .cpp
}

namespace aegisgate::autonomy {

class CostAutonomyApplier : public BaseAutonomyApplier {
public:
    // router: required. nullptr disables apply/rollback (returns failure).
    explicit CostAutonomyApplier(std::shared_ptr<MLRouter> router);

    std::string applierName() const override { return "cost_autonomy"; }

    // C2 /creative decision — 4 rule whitelist:
    //   R1 only allow downgrade  (rank(to) < rank(from))
    //   R2 no two-tier jumps     (rank(from) - rank(to) <= 1)
    //   R3 ≤ $50 / 24h savings   (estimated_savings_usd_24h)
    //   R4 ≤ 1000 RPS impact     (affected_requests_per_hour)
    // Missing or malformed payload fields fail closed (HIGH risk = false).
    bool isLowRisk(const ApprovalProposal& p) const override;

protected:
    ApplyResult applyImpl(const ApprovalProposal& p, bool dry_run) override;
    ApplyResult rollbackImpl(const ApprovalProposal& p) override;

private:
    static int rankTier(const std::string& tier);
    // Pull standard schema fields out of payload with safe defaults.
    struct PayloadView {
        std::string tenant_id;
        std::string from_tier;
        std::string to_tier;
        double      savings_usd_24h        = 0.0;
        int         affected_rps           = 0;
        bool        schema_ok              = false;
        std::string error;
    };
    static PayloadView extract(const ApprovalProposal& p);

    std::shared_ptr<MLRouter> router_;
};

} // namespace aegisgate::autonomy
