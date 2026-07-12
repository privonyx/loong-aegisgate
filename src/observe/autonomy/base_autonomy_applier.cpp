// Phase 11.6 BaseAutonomyApplier (TASK-20260523-03) — Epic 1 task 1.2 (TDD GREEN).
//
// See base_autonomy_applier.h for the contract. This file owns the
// SR17 layer 2 invariant (defense-in-depth) for all 5 Phase 11 appliers.
//
// SR drift guard anchors (BA1-BA5) reference exact tokens in this file
// and base_autonomy_applier.h. Do NOT rename helpers, change the
// error_code literal "autonomy_disabled", or weaken the SR17 check
// without also updating tests/rules/test-base-autonomy-applier-anchors.sh.

#include "observe/autonomy/base_autonomy_applier.h"

#include "observe/autonomy/approval_workflow.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <utility>

namespace aegisgate::autonomy {

bool BaseAutonomyApplier::isAutonomyEnabledOrLog(const std::string& name) {
    if (!AutonomyApprovalWorkflow::isAutonomyEnabled()) {
        spdlog::warn(
            "{}: refusing apply/rollback — SR17 layer 2 trip "
            "(AEGISGATE_DISABLE_AUTONOMY=1)",
            name);
        return false;
    }
    return true;
}

ApplyResult BaseAutonomyApplier::apply(const ApprovalProposal& p, bool dry_run) {
    const auto t0 = std::chrono::steady_clock::now();

    if (!isAutonomyEnabledOrLog(applierName())) {
        return ApplyResult::fail("autonomy_disabled",
                                  "AEGISGATE_DISABLE_AUTONOMY=1");
    }

    ApplyResult r = applyImpl(p, dry_run);
    auto t1 = std::chrono::steady_clock::now();
    r.duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
    return r;
}

ApplyResult BaseAutonomyApplier::rollback(const ApprovalProposal& p) {
    const auto t0 = std::chrono::steady_clock::now();

    if (!isAutonomyEnabledOrLog(applierName())) {
        return ApplyResult::fail("autonomy_disabled",
                                  "AEGISGATE_DISABLE_AUTONOMY=1");
    }

    ApplyResult r = rollbackImpl(p);
    auto t1 = std::chrono::steady_clock::now();
    r.duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
    return r;
}

ApplyResult BaseAutonomyApplier::makeDryRunOk(nlohmann::json details) {
    if (!details.is_object()) {
        details = nlohmann::json::object();
    }
    details["dry_run"] = true;
    ApplyResult r = ApplyResult::ok();
    r.details = std::move(details);
    return r;
}

ApplyResult BaseAutonomyApplier::makeFailSchemaInvalid(std::string err) {
    return ApplyResult::fail("schema_invalid", std::move(err));
}

ApplyResult BaseAutonomyApplier::makeFailMissingDep(std::string code,
                                                     std::string msg) {
    return ApplyResult::fail(std::move(code), std::move(msg));
}

} // namespace aegisgate::autonomy
