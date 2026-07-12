#include "observe/autonomy/approval_workflow.h"

#include "control_plane/ulid.h"
#include "guardrail/audit.h"
#include "guardrail/inbound/pii_filter.h"

#include <spdlog/spdlog.h>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace aegisgate::autonomy {

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

AutonomyApprovalWorkflow::AutonomyApprovalWorkflow(
    std::shared_ptr<ApprovalQueue> queue,
    std::shared_ptr<AuditLogger> audit,
    std::shared_ptr<PIIFilter> pii_filter)
    : queue_(std::move(queue)),
      audit_(std::move(audit)),
      pii_filter_(pii_filter ? std::move(pii_filter)
                             : std::make_shared<PIIFilter>()),
      ulid_(std::make_unique<Ulid>()) {}

AutonomyApprovalWorkflow::~AutonomyApprovalWorkflow() = default;

bool AutonomyApprovalWorkflow::isAutonomyEnabled() {
    const char* v = std::getenv("AEGISGATE_DISABLE_AUTONOMY");
    return !(v != nullptr && std::strcmp(v, "1") == 0);
}

bool AutonomyApprovalWorkflow::checkEnabled() const {
    if (enabled_override_.has_value()) return *enabled_override_;
    return isAutonomyEnabled();
}

void AutonomyApprovalWorkflow::registerApplier(
    AutonomySource source,
    std::shared_ptr<IApprovalApplier> applier) {
    std::lock_guard<std::mutex> lock(mutex_);
    appliers_[source] = std::move(applier);
}

std::shared_ptr<IApprovalApplier>
AutonomyApprovalWorkflow::findApplier(AutonomySource source) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = appliers_.find(source);
    return (it == appliers_.end()) ? nullptr : it->second;
}

void AutonomyApprovalWorkflow::auditTransition(const ApprovalProposal& p,
                                               const std::string& action) {
    if (!audit_) return;
    // detail = decision_trace + state summary so the audit row is self
    // contained when AuditLogger persists or re-emits it.
    nlohmann::json detail = p.decision_trace;
    detail["state"]            = toString(p.state);
    detail["source"]           = toString(p.source);
    detail["subject"]          = p.subject;
    detail["payload_sha256"]   = p.payload_sha256;
    detail["reviewer_user_id"] = p.reviewer_user_id;
    if (!p.reject_reason.empty()) detail["reject_reason"] = p.reject_reason;
    audit_->logAction(p.id, "system", "autonomy", action, detail.dump());
}

std::string AutonomyApprovalWorkflow::propose(ApprovalProposal p) {
    if (!checkEnabled()) {
        spdlog::warn(
            "AutonomyApprovalWorkflow::propose blocked by "
            "AEGISGATE_DISABLE_AUTONOMY (subject={})", p.subject);
        return std::string();
    }
    if (!queue_) {
        spdlog::error("AutonomyApprovalWorkflow::propose has no queue");
        return std::string();
    }

    if (p.id.empty()) p.id = ulid_->generate();
    if (p.proposed_at_ms == 0) p.proposed_at_ms = nowMs();
    if (p.proposer_user_id.empty()) p.proposer_user_id = "system";
    p.state = ApprovalState::PROPOSED;

    // C4: decision_trace must contain required fields with correct types.
    auto validation = validateDecisionTrace(p.decision_trace);
    if (!validation.ok) {
        std::string missing;
        for (const auto& m : validation.missing_fields) missing += m + " ";
        for (const auto& m : validation.wrong_type_fields)
            missing += m + "(type) ";
        spdlog::warn(
            "AutonomyApprovalWorkflow::propose rejected (decision_trace "
            "invalid): {}", missing);
        return std::string();
    }
    // T07: PII fallback over decision_trace string fields. PIIFilter is
    // injected via the constructor (default = standard regex set).
    maskDecisionTraceInPlace(p.decision_trace, pii_filter_.get());

    // T01 mitigation: pin payload integrity before persistence.
    p.payload_sha256 = computePayloadSha256(p.payload);

    auto id = queue_->insert(p);
    if (id.empty()) {
        spdlog::error("AutonomyApprovalWorkflow::propose queue insert failed "
                      "(id={})", p.id);
        return std::string();
    }
    auditTransition(p, "propose");
    return id;
}

bool AutonomyApprovalWorkflow::approve(const std::string& id,
                                       const std::string& reviewer_user_id) {
    if (!checkEnabled()) return false;
    if (!queue_) return false;

    auto cur = queue_->get(id);
    if (!cur) return false;
    if (cur->state != ApprovalState::PROPOSED) {
        spdlog::warn("approve({}) rejected: state={} (expected PROPOSED)",
                     id, toString(cur->state));
        return false;
    }
    cur->state            = ApprovalState::APPROVED;
    cur->reviewer_user_id = reviewer_user_id;
    cur->reviewed_at_ms   = nowMs();
    if (!queue_->update(*cur)) return false;
    auditTransition(*cur, "approve");
    return true;
}

bool AutonomyApprovalWorkflow::reject(const std::string& id,
                                      const std::string& reviewer_user_id,
                                      const std::string& reason) {
    if (!checkEnabled()) return false;
    if (!queue_) return false;

    auto cur = queue_->get(id);
    if (!cur) return false;
    if (cur->state != ApprovalState::PROPOSED &&
        cur->state != ApprovalState::APPROVED) {
        spdlog::warn("reject({}) rejected: state={}", id, toString(cur->state));
        return false;
    }
    cur->state            = ApprovalState::REJECTED;
    cur->reviewer_user_id = reviewer_user_id;
    cur->reviewed_at_ms   = nowMs();
    cur->reject_reason    = reason;
    if (!queue_->update(*cur)) return false;
    auditTransition(*cur, "reject");
    return true;
}

bool AutonomyApprovalWorkflow::apply(const std::string& id) {
    if (!checkEnabled()) return false;
    if (!queue_) return false;

    auto cur = queue_->get(id);
    if (!cur) return false;
    if (cur->state != ApprovalState::APPROVED) {
        spdlog::warn("apply({}) rejected: state={} (expected APPROVED)",
                     id, toString(cur->state));
        return false;
    }
    // T01: payload tamper guard before invoking the applier.
    if (!verifyPayloadSha256(*cur)) {
        spdlog::error("apply({}) rejected: payload_sha256 mismatch (T01)", id);
        auditTransition(*cur, "apply_tampered_rejected");
        return false;
    }

    auto applier = findApplier(cur->source);
    if (!applier) {
        spdlog::error("apply({}) rejected: no applier for source={}",
                      id, toString(cur->source));
        auditTransition(*cur, "apply_no_applier");
        return false;
    }

    auto result = applier->apply(*cur, /*dry_run=*/false);
    if (result.success) {
        cur->state = ApprovalState::APPLIED;
        if (!queue_->update(*cur)) return false;
        auditTransition(*cur, "apply");
        return true;
    }

    // C1 decision: failure → auto rollback → ROLLED_BACK.
    spdlog::warn("apply({}) failed: code={} msg={} — auto rollback",
                 id, result.error_code, result.error_message);
    (void)applier->rollback(*cur);
    cur->state = ApprovalState::ROLLED_BACK;
    (void)queue_->update(*cur);
    auditTransition(*cur, "apply_failed_rolled_back");
    return false;
}

bool AutonomyApprovalWorkflow::rollback(const std::string& id) {
    if (!checkEnabled()) return false;
    if (!queue_) return false;

    auto cur = queue_->get(id);
    if (!cur) return false;
    if (cur->state != ApprovalState::APPLIED) {
        spdlog::warn("rollback({}) rejected: state={} (expected APPLIED)",
                     id, toString(cur->state));
        return false;
    }

    auto applier = findApplier(cur->source);
    if (!applier) {
        spdlog::error("rollback({}) rejected: no applier for source={}",
                      id, toString(cur->source));
        auditTransition(*cur, "rollback_no_applier");
        return false;
    }
    (void)applier->rollback(*cur);  // best-effort; we still mark state
    cur->state = ApprovalState::ROLLED_BACK;
    if (!queue_->update(*cur)) return false;
    auditTransition(*cur, "rollback");
    return true;
}

std::optional<ApprovalProposal>
AutonomyApprovalWorkflow::get(const std::string& id) const {
    if (!queue_) return std::nullopt;
    return queue_->get(id);
}

std::vector<ApprovalProposal>
AutonomyApprovalWorkflow::list(std::optional<ApprovalState> state_filter,
                               std::optional<AutonomySource> source_filter,
                               int limit, int offset) const {
    if (!queue_) return {};
    ApprovalQueueQuery q;
    q.state_filter  = state_filter;
    q.source_filter = source_filter;
    q.limit         = limit;
    q.offset        = offset;
    return queue_->list(q);
}

std::int64_t
AutonomyApprovalWorkflow::count(std::optional<ApprovalState> state_filter,
                                std::optional<AutonomySource> source_filter) const {
    if (!queue_) return 0;
    ApprovalQueueQuery q;
    q.state_filter  = state_filter;
    q.source_filter = source_filter;
    return queue_->count(q);
}

} // namespace aegisgate::autonomy
