#pragma once

// Phase 11.6 BaseAutonomyApplier (TASK-20260523-03) — Epic 1 task 1.2 (TDD GREEN).
//
// GoF Template Method base class encapsulating the public skeleton of
// IApprovalApplier::apply/rollback shared across all 5 Phase 11 appliers
// (Cost / Bandit / Guard / Recovery / Workflow). The single base
// invariant is the SR17 layer 2 defense-in-depth check; everything else
// (timing wrapper, dry_run short-circuit, ApplyResult helpers) is
// convenience packaging.
//
// Subclasses override applyImpl() / rollbackImpl() with business logic
// (payload schema extraction + side effects + before/after snapshots).
// Subclasses MUST NOT re-implement SR17 layer 2 or timing themselves —
// those responsibilities live in the base only. This is the single-source
// SR17 invariant for drift-guard anchoring; see
// `tests/rules/test-base-autonomy-applier-anchors.sh` (BA1-BA5).
//
// Design references:
//   docs/specs/2026-05-23-base-autonomy-applier-template-design.md §4.2/§4.3
//   docs/plans/2026-05-23-base-autonomy-applier-template.md Epic 1
//   memory-bank/systemPatterns.md "防御深度 layer-specific assertion 模式"

#include "observe/autonomy/i_approval_applier.h"

#include <nlohmann/json.hpp>
#include <string>

namespace aegisgate::autonomy {

class BaseAutonomyApplier : public IApprovalApplier {
public:
    // Final apply()/rollback() — subclasses cannot override. SR17 layer 2
    // is enforced here (the single base invariant), timing is computed
    // here, dry_run is short-circuited here (when subclasses opt-in via
    // makeDryRunOk). Subclasses contribute business logic via
    // applyImpl()/rollbackImpl() below.
    ApplyResult apply(const ApprovalProposal& p, bool dry_run) final;
    ApplyResult rollback(const ApprovalProposal& p) final;

    // applierName() and isLowRisk() remain pure virtual / virtual on
    // IApprovalApplier — base does NOT take ownership. Subclasses keep
    // expressing their identity and risk policy.

    // Convenience helpers exposed as `public static` so subclasses can
    // call them without `this`, tests can grep error_code shapes, and
    // future Phase-12 appliers can reuse the canonical fail()
    // constructors. These are NOT virtual: tests + SR drift guards
    // anchor on the exact error_code literals, so do NOT change
    // "schema_invalid" without also updating BA2 (anchor) + the 5
    // applier tests that grep for it.
    //
    //   makeDryRunOk(details)            — set details["dry_run"]=true and
    //                                       return ApplyResult::ok().
    //   makeFailSchemaInvalid(err)       — fail("schema_invalid", err).
    //   makeFailMissingDep(code, msg)    — fail(code, msg) for the
    //                                       "*_missing" family
    //                                       (router_missing, etc.).
    //
    // Note: duration_ms is filled by the base wrapper, NOT by these
    // helpers. Subclasses just construct the details and let the
    // wrapper finish the job.
    static ApplyResult makeDryRunOk(nlohmann::json details);
    static ApplyResult makeFailSchemaInvalid(std::string err);
    static ApplyResult makeFailMissingDep(std::string code, std::string msg);

protected:
    // Subclass extension point — invoked only after SR17 layer 2 PASS and
    // with t0 captured by the base wrapper. Subclasses construct details
    // themselves (no enforced container) and may return either ok() or
    // fail(). The base will fill duration_ms before returning to caller.
    virtual ApplyResult applyImpl(const ApprovalProposal& p, bool dry_run) = 0;
    virtual ApplyResult rollbackImpl(const ApprovalProposal& p) = 0;

private:
    // SR17 layer 2 single source of truth — referenced by
    // test-base-autonomy-applier-anchors.sh BA1 reverse-anchor.
    static bool isAutonomyEnabledOrLog(const std::string& applier_name);
};

} // namespace aegisgate::autonomy
