#pragma once

// Phase 11.5 AutonomyApprovalWorkflow (TASK-20260518-02) — Epic 1.5.
//
// AutonomyApprovalWorkflow is the cross-cutting control plane that every
// Phase 11.x autonomy module (CostOptimizer, AutoRecovery, BanditRouter,
// AdaptiveGuard, Workflow 2.0) funnels through.  It owns the 5-state
// proposal lifecycle, the apply/rollback dispatch table, and the audit
// trail.  Per design spec §4.2 and plan §D Task 1.5.
//
// State machine (design §3.1 C1):
//
//     PROPOSED ─approve()─► APPROVED ─apply()─► APPLIED ─rollback()─► ROLLED_BACK
//        │                     │                   ▲
//        │ reject()            │ reject()          │ apply() failure
//        ▼                     ▼                   └────► ROLLED_BACK (auto)
//     REJECTED              REJECTED
//
// Concurrency:
//   - mutex_ guards the per-id reentrancy bookkeeping (none today; kept for
//     forward compatibility with Phase 11.3 async apply).
//   - All proposal state is owned by ApprovalQueue (Layer 3) and AuditLogger
//     (Layer 3); we call them lock-free here so the workflow itself only
//     coordinates and does not introduce new locking depth.
//
// Global kill switch (SR17 reuse): static isAutonomyEnabled() reads
// AEGISGATE_DISABLE_AUTONOMY env var (mirrors autoRollbackEnabledFromEnv
// shape in src/control_plane/rollout/rollout_wiring.h:41).

#include "observe/autonomy/approval_proposal.h"
#include "observe/autonomy/approval_queue.h"
#include "observe/autonomy/approval_state.h"
#include "observe/autonomy/i_approval_applier.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aegisgate {
class AuditLogger;  // forward
class PIIFilter;    // forward — E1.6 injection point
class Ulid;         // forward
} // namespace aegisgate

namespace aegisgate::autonomy {

class AutonomyApprovalWorkflow {
public:
    // queue: required (in-memory cache + PersistentStore mirror)
    // audit: required (all state transitions are audited per T03 mitigation)
    // pii_filter: optional; nullptr → workflow constructs its own default
    //   instance (loaded with the standard regex set: email/phone/api_key/
    //   id_card/jwt/bank_card) per E1.6 — "default = 新建一个实例".
    AutonomyApprovalWorkflow(std::shared_ptr<ApprovalQueue> queue,
                             std::shared_ptr<AuditLogger> audit,
                             std::shared_ptr<PIIFilter>   pii_filter = nullptr);
    ~AutonomyApprovalWorkflow();

    AutonomyApprovalWorkflow(const AutonomyApprovalWorkflow&) = delete;
    AutonomyApprovalWorkflow& operator=(const AutonomyApprovalWorkflow&) = delete;

    // --- proposal lifecycle ---------------------------------------------

    // Submit a fresh proposal. Mints a ULID, stamps p.proposed_at_ms if
    // unset, computes payload_sha256, writes audit ("propose"), persists
    // to the queue. Returns the assigned ULID, or empty string on failure
    // (autonomy disabled, queue rejected, validation failed).
    std::string propose(ApprovalProposal p);

    // PROPOSED → APPROVED transition. reviewer_user_id is recorded in
    // the proposal + audit detail. Pass "system" for auto-approval.
    bool approve(const std::string& id, const std::string& reviewer_user_id);

    // PROPOSED|APPROVED → REJECTED transition.
    bool reject(const std::string& id,
                const std::string& reviewer_user_id,
                const std::string& reason);

    // APPROVED → APPLIED transition. Performs:
    //   (a) state == APPROVED guard,
    //   (b) payload_sha256 integrity check (T01 mitigation),
    //   (c) applier->apply(p, dry_run=false),
    //   (d) on failure: applier->rollback(p) + state=ROLLED_BACK +
    //       audit("apply_failed_rolled_back").
    // Returns true iff the proposal reaches APPLIED.
    bool apply(const std::string& id);

    // APPLIED → ROLLED_BACK transition (manual rollback path).
    bool rollback(const std::string& id);

    // --- read paths ------------------------------------------------------

    std::optional<ApprovalProposal> get(const std::string& id) const;
    std::vector<ApprovalProposal> list(std::optional<ApprovalState> state_filter,
                                       std::optional<AutonomySource> source_filter,
                                       int limit, int offset) const;

    // Total proposals matching the same filters as list(), independent of
    // pagination — feeds the Admin UI "total" field for FinOps paging.
    std::int64_t count(std::optional<ApprovalState> state_filter,
                       std::optional<AutonomySource> source_filter) const;

    // --- applier registration (C3 dispatch table) ------------------------

    void registerApplier(AutonomySource source,
                         std::shared_ptr<IApprovalApplier> applier);

    // --- global kill switch (SR17 reuse) ---------------------------------

    // Returns false iff env AEGISGATE_DISABLE_AUTONOMY=="1".
    // Mirrors autoRollbackEnabledFromEnv (rollout_wiring.h:41).
    static bool isAutonomyEnabled();

    // For tests: bypass env check (does NOT bypass per-call state guards).
    void setAutonomyEnabledOverride(std::optional<bool> on) {
        enabled_override_ = on;
    }

private:
    bool checkEnabled() const;

    // Resolve the applier for a given source. Returns nullptr when none
    // registered — apply() then audits "apply_no_applier" and fails.
    std::shared_ptr<IApprovalApplier> findApplier(AutonomySource source) const;

    void auditTransition(const ApprovalProposal& p, const std::string& action);

    std::shared_ptr<ApprovalQueue> queue_;
    std::shared_ptr<AuditLogger>   audit_;
    std::shared_ptr<PIIFilter>     pii_filter_;
    std::unique_ptr<Ulid>          ulid_;

    mutable std::mutex mutex_;  // Lock Layer 3 — guards appliers_ and override
    std::unordered_map<AutonomySource, std::shared_ptr<IApprovalApplier>>
        appliers_;
    std::optional<bool> enabled_override_;
};

} // namespace aegisgate::autonomy
