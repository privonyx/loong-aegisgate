#include "control_plane/audit_bridge.h"

#include "control_plane/config_version_record.h"
#include "guardrail/audit.h"

#include <nlohmann/json.hpp>

namespace aegisgate {

namespace {

constexpr const char* kStage  = "control_plane";
constexpr const char* kTenant = "system";

std::string dumpJson(const nlohmann::json& j) {
    try { return j.dump(); }
    catch (...) { return "{}"; }
}

} // namespace

AuditBridge::AuditBridge(AuditLogger* audit) : audit_(audit) {}

void AuditBridge::recordSubmit(const ConfigVersionRecord& rec) {
    if (!audit_) return;
    nlohmann::json detail;
    detail["version_id"] = rec.version_id;
    detail["sha256"] = rec.content_sha256;
    detail["size"] = rec.size_bytes;
    detail["submitter"] = rec.submitter;
    detail["comment"] = rec.submitter_comment;
    // NOTE: yaml_content intentionally omitted (SR11). Auditors trace via
    // sha256 or the signed compliance export that ships encrypted bundles.

    AuditEntry e;
    e.request_id = request_id_gen_.generate();
    e.stage_name = kStage;
    e.tenant_id = kTenant;
    e.action = "config.submit";
    e.detail = dumpJson(detail);
    audit_->log(e);
}

void AuditBridge::recordApprove(const std::string& version_id,
                                 const std::string& reviewer,
                                 const std::string& comment) {
    if (!audit_) return;
    nlohmann::json detail;
    detail["version_id"] = version_id;
    detail["reviewer"] = reviewer;
    detail["reviewer_comment"] = comment;

    AuditEntry e;
    e.request_id = request_id_gen_.generate();
    e.stage_name = kStage;
    e.tenant_id = kTenant;
    e.action = "config.approve";
    e.detail = dumpJson(detail);
    audit_->log(e);
}

void AuditBridge::recordReject(const std::string& version_id,
                                const std::string& reviewer,
                                const std::string& comment) {
    if (!audit_) return;
    nlohmann::json detail;
    detail["version_id"] = version_id;
    detail["reviewer"] = reviewer;
    detail["reviewer_comment"] = comment;

    AuditEntry e;
    e.request_id = request_id_gen_.generate();
    e.stage_name = kStage;
    e.tenant_id = kTenant;
    e.action = "config.reject";
    e.detail = dumpJson(detail);
    audit_->log(e);
}

void AuditBridge::recordActivate(const std::string& version_id,
                                  const std::string& activator,
                                  const std::string& previous_active) {
    if (!audit_) return;
    nlohmann::json detail;
    detail["version_id"] = version_id;
    detail["activator"] = activator;
    detail["previous_active"] = previous_active;  // may be ""

    AuditEntry e;
    e.request_id = request_id_gen_.generate();
    e.stage_name = kStage;
    e.tenant_id = kTenant;
    e.action = "config.activate";
    e.detail = dumpJson(detail);
    audit_->log(e);
}

void AuditBridge::recordRollback(const std::string& target_id,
                                  const std::string& actor,
                                  const std::string& previous_active,
                                  const std::string& comment) {
    if (!audit_) return;
    nlohmann::json detail;
    detail["target_version_id"] = target_id;
    detail["actor"] = actor;
    detail["previous_active"] = previous_active;
    detail["comment"] = comment;

    AuditEntry e;
    e.request_id = request_id_gen_.generate();
    e.stage_name = kStage;
    e.tenant_id = kTenant;
    e.action = "config.rollback";
    e.detail = dumpJson(detail);
    audit_->log(e);
}

} // namespace aegisgate
