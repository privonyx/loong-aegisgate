#include "control_plane/rollout/rollout_audit_bridge.h"

#include "guardrail/audit.h"

#include <nlohmann/json.hpp>

namespace aegisgate {
namespace {

constexpr const char* kStage  = "control_plane";
constexpr const char* kTenant = "system";

std::string dumpJson(const nlohmann::json& j) {
    try { return j.dump(); } catch (...) { return "{}"; }
}

// Canonical skeleton for every rollout event — rollout_id, current status,
// current stage index. Callers add their action-specific fields on top.
nlohmann::json baseDetail(const RolloutRecord& r) {
    nlohmann::json d;
    d["rollout_id"]         = r.rollout_id;
    d["target_version_id"]  = r.target_version_id;
    d["status"]             = rolloutStatusToString(r.status);
    d["current_stage_index"] = r.current_stage_index;
    return d;
}

} // namespace

RolloutAuditBridge::RolloutAuditBridge(AuditLogger* audit) : audit_(audit) {}

void RolloutAuditBridge::recordCreated(const RolloutRecord& r,
                                        const std::string& actor) {
    if (!audit_) return;
    auto detail = baseDetail(r);
    detail["actor"] = actor;

    AuditEntry e;
    e.request_id = request_id_gen_.generate();
    e.stage_name = kStage;
    e.tenant_id  = kTenant;
    e.action     = "rollout.created";
    e.detail     = dumpJson(detail);
    audit_->log(e);
}

void RolloutAuditBridge::recordStarted(const RolloutRecord& r,
                                        const std::string& actor) {
    if (!audit_) return;
    auto detail = baseDetail(r);
    detail["actor"]                     = actor;
    detail["previous_active_version_id"] = r.previous_active_version_id;
    detail["started_at"]                 = r.started_at;

    AuditEntry e;
    e.request_id = request_id_gen_.generate();
    e.stage_name = kStage;
    e.tenant_id  = kTenant;
    e.action     = "rollout.started";
    e.detail     = dumpJson(detail);
    audit_->log(e);
}

void RolloutAuditBridge::recordStagePromoted(const RolloutRecord& r,
                                              int from_stage_index,
                                              int to_stage_index,
                                              const std::string& actor) {
    if (!audit_) return;
    auto detail = baseDetail(r);
    detail["actor"]          = actor;
    detail["from_stage_index"] = from_stage_index;
    detail["to_stage_index"]   = to_stage_index;
    detail["stage_started_at"] = r.stage_started_at;

    AuditEntry e;
    e.request_id = request_id_gen_.generate();
    e.stage_name = kStage;
    e.tenant_id  = kTenant;
    e.action     = "rollout.stage_promoted";
    e.detail     = dumpJson(detail);
    audit_->log(e);
}

void RolloutAuditBridge::recordPausedManual(const RolloutRecord& r,
                                             const std::string& actor,
                                             const std::string& comment) {
    if (!audit_) return;
    auto detail = baseDetail(r);
    detail["actor"]     = actor;
    detail["comment"]   = comment;
    detail["paused_at"] = r.paused_at;
    detail["reason"]    = pauseReasonToString(PauseReason::MANUAL);

    AuditEntry e;
    e.request_id = request_id_gen_.generate();
    e.stage_name = kStage;
    e.tenant_id  = kTenant;
    e.action     = "rollout.paused_manual";
    e.detail     = dumpJson(detail);
    audit_->log(e);
}

void RolloutAuditBridge::recordPausedAuto(const RolloutRecord& r,
                                           PauseReason reason,
                                           const std::string& detail_text) {
    if (!audit_) return;
    auto detail = baseDetail(r);
    detail["reason"]    = pauseReasonToString(reason);
    detail["detail"]    = detail_text;
    detail["paused_at"] = r.paused_at;
    // NOTE (SR14): RolloutSpec / yaml_content intentionally NOT logged.

    AuditEntry e;
    e.request_id = request_id_gen_.generate();
    e.stage_name = kStage;
    e.tenant_id  = kTenant;
    e.action     = "rollout.paused_auto";
    e.detail     = dumpJson(detail);
    audit_->log(e);
}

void RolloutAuditBridge::recordResumed(const RolloutRecord& r,
                                        const std::string& actor) {
    if (!audit_) return;
    auto detail = baseDetail(r);
    detail["actor"] = actor;

    AuditEntry e;
    e.request_id = request_id_gen_.generate();
    e.stage_name = kStage;
    e.tenant_id  = kTenant;
    e.action     = "rollout.resumed";
    e.detail     = dumpJson(detail);
    audit_->log(e);
}

void RolloutAuditBridge::recordAutoRollbackTriggered(const RolloutRecord& r,
                                                      const std::string& detail_text) {
    if (!audit_) return;
    auto detail = baseDetail(r);
    detail["detail"] = detail_text;

    AuditEntry e;
    e.request_id = request_id_gen_.generate();
    e.stage_name = kStage;
    e.tenant_id  = kTenant;
    e.action     = "rollout.auto_rollback_triggered";
    e.detail     = dumpJson(detail);
    audit_->log(e);
}

void RolloutAuditBridge::recordAutoRollbackCompleted(const RolloutRecord& r,
                                                      const std::string& new_active_version_id) {
    if (!audit_) return;
    auto detail = baseDetail(r);
    detail["new_active_version_id"] = new_active_version_id;

    AuditEntry e;
    e.request_id = request_id_gen_.generate();
    e.stage_name = kStage;
    e.tenant_id  = kTenant;
    e.action     = "rollout.auto_rollback_completed";
    e.detail     = dumpJson(detail);
    audit_->log(e);
}

void RolloutAuditBridge::recordAborted(const RolloutRecord& r,
                                        const std::string& actor) {
    if (!audit_) return;
    auto detail = baseDetail(r);
    detail["actor"] = actor;

    AuditEntry e;
    e.request_id = request_id_gen_.generate();
    e.stage_name = kStage;
    e.tenant_id  = kTenant;
    e.action     = "rollout.aborted";
    e.detail     = dumpJson(detail);
    audit_->log(e);
}

void RolloutAuditBridge::recordCompleted(const RolloutRecord& r,
                                          const std::string& new_active_version_id) {
    if (!audit_) return;
    auto detail = baseDetail(r);
    detail["new_active_version_id"] = new_active_version_id;
    detail["completed_at"]          = r.completed_at;

    AuditEntry e;
    e.request_id = request_id_gen_.generate();
    e.stage_name = kStage;
    e.tenant_id  = kTenant;
    e.action     = "rollout.completed";
    e.detail     = dumpJson(detail);
    audit_->log(e);
}

void RolloutAuditBridge::recordFailed(const RolloutRecord& r,
                                       const std::string& reason) {
    if (!audit_) return;
    auto detail = baseDetail(r);
    detail["reason"] = reason;

    AuditEntry e;
    e.request_id = request_id_gen_.generate();
    e.stage_name = kStage;
    e.tenant_id  = kTenant;
    e.action     = "rollout.failed";
    e.detail     = dumpJson(detail);
    audit_->log(e);
}

} // namespace aegisgate
