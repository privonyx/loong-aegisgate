#pragma once

// Phase 11.5 AutonomyApprovalWorkflow (TASK-20260518-02) — Epic 1.3.
//
// IApprovalApplier — the per-source effector contract that
// AutonomyApprovalWorkflow dispatches to once a proposal reaches APPROVED.
//
// Creative C3 decision: "Plan B — synchronous + dry-run + structured
// ApplyResult". v1 keeps the surface small (3 pure-virtuals) and exposes
// dry_run as a first-class argument so the Admin UI / shadow-mode tests
// can preview impact without touching production state. An optional
// applyAsync() will be appended in Phase 11.3 (Workflow 2.0) when the DAG
// engine needs long-running jobs — added as a default-implemented virtual
// so existing appliers stay zero-touch (A12 progressive-exposure pattern).
//
// Design references:
//   memory-bank/creative/creative-phase11.5-cost-autonomy.md §2 (C3)
//   docs/specs/2026-05-18-phase11.5-cost-autonomy-design.md §4.2

#include "observe/autonomy/approval_proposal.h"

#include <chrono>
#include <nlohmann/json.hpp>
#include <string>

namespace aegisgate::autonomy {

// Structured outcome of a single apply()/rollback() call. Fields are
// written verbatim into AuditLogger.detail so they double as the
// human-readable trace of what each applier did.
struct ApplyResult {
    bool                       success = false;
    std::string                error_code;     // dictionary value, may be empty on success
    std::string                error_message;  // human-readable, optional even on failure
    nlohmann::json             details = nlohmann::json::object();
    std::chrono::milliseconds  duration_ms{0};

    static ApplyResult ok(nlohmann::json details = nlohmann::json::object(),
                          std::chrono::milliseconds duration = std::chrono::milliseconds{0}) {
        ApplyResult r;
        r.success     = true;
        r.details     = std::move(details);
        r.duration_ms = duration;
        return r;
    }

    static ApplyResult fail(std::string code, std::string message = {},
                            nlohmann::json details = nlohmann::json::object(),
                            std::chrono::milliseconds duration = std::chrono::milliseconds{0}) {
        ApplyResult r;
        r.success       = false;
        r.error_code    = std::move(code);
        r.error_message = std::move(message);
        r.details       = std::move(details);
        r.duration_ms   = duration;
        return r;
    }
};

class IApprovalApplier {
public:
    virtual ~IApprovalApplier() = default;

    // Apply the proposal's effect. When dry_run == true the applier MUST
    // NOT mutate production state but should still populate
    // `details` with the before/after snapshot the Admin UI surfaces in
    // its preview drawer.
    virtual ApplyResult apply(const ApprovalProposal& p,
                              bool dry_run = false) = 0;

    // Reverse a previously applied proposal. Called both manually (Admin UI
    // "Rollback" button) and automatically by AutonomyApprovalWorkflow when
    // apply() fails (creative C1 → state = ROLLED_BACK).
    virtual ApplyResult rollback(const ApprovalProposal& p) = 0;

    // Identifier for logs / audit / metrics labels. Conventionally returns
    // the runtime class name (e.g. "CostAutonomyApplier").
    virtual std::string applierName() const = 0;

    // C2 hook: per-applier definition of "low risk" used by the
    // auto_low_risk mode. Defaults to false so that subclasses must opt in
    // explicitly (creative §3.5 "default conservative" guidance).
    virtual bool isLowRisk(const ApprovalProposal& p) const {
        (void)p;
        return false;
    }
};

} // namespace aegisgate::autonomy
