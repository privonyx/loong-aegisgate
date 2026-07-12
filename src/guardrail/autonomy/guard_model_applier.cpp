#include "guardrail/autonomy/guard_model_applier.h"

#include "observe/autonomy/approval_proposal.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <utility>

namespace aegisgate::guard {

using aegisgate::autonomy::ApplyResult;
using aegisgate::autonomy::ApprovalProposal;
using aegisgate::autonomy::BaseAutonomyApplier;

GuardModelApplier::GuardModelApplier(
    std::shared_ptr<IGuardModelRegistry> registry)
    : registry_(std::move(registry)) {}

GuardModelApplier::PayloadView GuardModelApplier::extract(
    const ApprovalProposal& p) {
    PayloadView v;
    if (!p.payload.is_object()) {
        v.error = "payload is not a JSON object";
        return v;
    }
    v.action = p.payload.value("action", std::string{});
    v.model_id = p.payload.value("model_id", std::string{});
    v.version = p.payload.value("version", std::string{});
    if (p.payload.contains("shadow_metrics") &&
        p.payload["shadow_metrics"].is_object()) {
        const auto& sm = p.payload["shadow_metrics"];
        v.win_rate = sm.value("win_rate", 0.0);
        v.shadow_duration_min = sm.value("shadow_duration_min", 0);
        v.fp_rate_delta = sm.value("fp_rate_delta", 0.0);
    }
    if (v.action.empty()) { v.error = "missing action"; return v; }
    if (v.model_id.empty()) { v.error = "missing model_id"; return v; }
    if (v.version.empty()) { v.error = "missing version"; return v; }
    v.schema_ok = true;
    return v;
}

ApplyResult GuardModelApplier::applyImpl(const ApprovalProposal& p, bool dry_run) {
    auto v = extract(p);
    if (!v.schema_ok) {
        return BaseAutonomyApplier::makeFailSchemaInvalid(v.error);
    }
    if (!registry_) {
        return BaseAutonomyApplier::makeFailMissingDep(
            "registry_missing", "no IGuardModelRegistry");
    }

    nlohmann::json details = {
        {"action", v.action},
        {"model_id", v.model_id},
        {"version", v.version},
        {"win_rate", v.win_rate},
        {"shadow_duration_min", v.shadow_duration_min},
        {"fp_rate_delta", v.fp_rate_delta},
    };

    if (dry_run) {
        return BaseAutonomyApplier::makeDryRunOk(std::move(details));
    }

    RegistryOpResult op;
    if (v.action == "promote_shadow_to_live") {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        op = registry_->promote(v.model_id, v.version, now_ms);
    } else if (v.action == "revert_to_previous") {
        op = registry_->revert(v.model_id, v.version);
    } else if (v.action == "register_shadow") {
        ModelRegistryRecord rec;
        rec.model_id = v.model_id;
        rec.version = v.version;
        rec.path = p.payload.value("path", std::string{});
        rec.status = GuardModelStatus::Shadow;
        rec.classifier_threshold = static_cast<float>(
            p.payload.value("classifier_threshold", 0.5));
        rec.artifact_sha256 = p.payload.value("artifact_sha256", std::string{});
        rec.metrics_summary = p.payload.value(
            "metrics_summary", nlohmann::json::object()).dump();
        op = registry_->insert(rec);
    } else {
        op = RegistryOpResult::fail("unknown_action", v.action);
    }

    if (!op.ok) {
        details["error_code"] = op.error_code;
        return ApplyResult::fail(op.error_code, op.detail, details);
    }
    auto r = ApplyResult::ok();
    r.details = std::move(details);
    return r;  // base wrapper fills duration_ms
}

ApplyResult GuardModelApplier::rollbackImpl(const ApprovalProposal& p) {
    // Rollback path: regardless of what apply() did, the safest answer is to
    // revert the version mentioned by the proposal back to Retired.
    auto v = extract(p);
    if (!v.schema_ok) {
        return BaseAutonomyApplier::makeFailSchemaInvalid(v.error);
    }
    if (!registry_) {
        return BaseAutonomyApplier::makeFailMissingDep(
            "registry_missing", "");
    }
    auto op = registry_->revert(v.model_id, v.version);
    nlohmann::json details = {
        {"rollback_target", v.version}, {"model_id", v.model_id}};
    if (!op.ok) {
        return ApplyResult::fail(op.error_code, op.detail, details);
    }
    auto r = ApplyResult::ok();
    r.details = std::move(details);
    return r;  // base wrapper fills duration_ms
}

bool GuardModelApplier::isLowRisk(const ApprovalProposal& p) const {
    auto v = extract(p);
    if (!v.schema_ok) return false;
    // R1: revert is destructive — never auto-approve.
    if (v.action == "revert_to_previous") return false;
    // R2: win_rate must beat baseline.
    if (v.win_rate < 0.55) return false;
    // R3: minimum shadow duration.
    if (v.shadow_duration_min < 60) return false;
    // R4: no >10pp false-positive regression.
    if (v.fp_rate_delta < -0.10) return false;
    return true;
}

}  // namespace aegisgate::guard
