#include "guardrail/admin/guard_admin_controller.h"

#include "guardrail/audit.h"
#include "guardrail/feedback/guard_feedback_anomaly_detector.h"
#include "guardrail/feedback/guard_feedback_payload.h"
#include "guardrail/feedback/guard_feedback_rate_limiter.h"
#include "guardrail/feedback/guard_feedback_sink.h"
#include "guardrail/model/i_guard_model_registry.h"
#include "observe/autonomy/approval_proposal.h"
#include "observe/autonomy/approval_workflow.h"

#include <chrono>
#include <utility>

namespace aegisgate::guard {

GuardAdminController::GuardAdminController(GuardAdminDeps deps)
    : deps_(std::move(deps)) {}

bool GuardAdminController::roleAuthorized(const std::string& role) {
    return isReviewerRoleAllowed(role);
}

GuardAdminResult GuardAdminController::postFeedback(
    const GuardAdminContext& ctx, const nlohmann::json& body) {
    if (!roleAuthorized(ctx.role)) {
        return GuardAdminResult::error(
            403, "unauthorized_role",
            "role '" + ctx.role + "' may not submit feedback");
    }

    auto valid = validateGuardFeedbackPayload(body);
    if (!valid.ok) {
        int status = 400;
        if (valid.error_code == "unauthorized_role") status = 403;
        return GuardAdminResult::error(status, valid.error_code, valid.detail);
    }

    if (deps_.rate_limiter) {
        auto rl = deps_.rate_limiter->checkAndConsume(
            ctx.tenant_id, valid.payload.reviewer_user_id);
        if (!rl.allowed) {
            return GuardAdminResult::error(429, rl.reject_reason, rl.reject_reason);
        }
    }

    if (deps_.anomaly_detector) {
        auto ad = deps_.anomaly_detector->inspect(valid.payload.reviewer_user_id,
                                                   valid.payload.label);
        if (ad.is_anomalous) {
            if (deps_.audit) {
                nlohmann::json detail = {
                    {"reviewer_user_id", valid.payload.reviewer_user_id},
                    {"reason", ad.reason},
                    {"observed", ad.observed},
                    {"threshold", ad.threshold},
                };
                deps_.audit->logAction(
                    /*request_id=*/valid.payload.request_id,
                    /*tenant_id=*/ctx.tenant_id,
                    /*stage=*/"AdaptiveGuard",
                    /*action=*/"feedback_anomaly_flag",
                    /*detail=*/detail.dump());
            }
            // Flagged feedback never reaches the sink; the trainer corpus
            // stays clean. Return 4xx so the client knows the input was
            // refused (vs. 202 which could be mistaken for "ingested").
            return GuardAdminResult::error(409, ad.reason,
                                            "feedback flagged by anomaly detector");
        }
    }

    if (!deps_.sink) {
        return GuardAdminResult::error(503, "sink_unavailable", "sink not wired");
    }
    auto sink_res = deps_.sink->ingest(valid.payload, ctx.tenant_id);
    if (!sink_res.ok) {
        return GuardAdminResult::error(500, sink_res.error_code, sink_res.detail);
    }

    nlohmann::json body_out = {
        {"accepted", true},
        {"request_id", valid.payload.request_id},
    };
    return GuardAdminResult::ok(std::move(body_out));
}

GuardAdminResult GuardAdminController::getExplanation(
    const GuardAdminContext& ctx, const std::string& request_id) {
    if (!roleAuthorized(ctx.role)) {
        return GuardAdminResult::error(403, "unauthorized_role");
    }
    std::lock_guard lock(explanations_mu_);
    auto it = explanations_.find(request_id);
    if (it == explanations_.end()) {
        return GuardAdminResult::error(404, "not_found",
                                        "no explanation for " + request_id);
    }
    return GuardAdminResult::ok(it->second.toJson());
}

GuardAdminResult GuardAdminController::promoteModel(
    const GuardAdminContext& ctx, const nlohmann::json& body) {
    if (!roleAuthorized(ctx.role)) {
        return GuardAdminResult::error(403, "unauthorized_role");
    }
    if (!deps_.workflow) {
        return GuardAdminResult::error(503, "workflow_unavailable",
                                        "AutonomyApprovalWorkflow not wired");
    }

    if (!body.is_object()) {
        return GuardAdminResult::error(400, "invalid_json");
    }
    if (!body.contains("action") || !body.contains("model_id") ||
        !body.contains("version")) {
        return GuardAdminResult::error(400, "missing_field",
                                        "action / model_id / version required");
    }

    aegisgate::autonomy::ApprovalProposal p;
    p.source = aegisgate::autonomy::AutonomySource::AdaptiveGuard;
    p.subject = body.value("model_id", std::string{});
    p.payload = body;
    p.proposer_user_id = ctx.user_id.empty() ? "system" : ctx.user_id;
    p.proposed_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    p.payload_sha256 =
        aegisgate::autonomy::computePayloadSha256(p.payload);
    p.decision_trace = {
        {"source_id", "guard_admin"},
        {"algorithm_name", "manual_promote"},
        {"input_hash_sha256", p.payload_sha256},
        {"proposed_at_ms", p.proposed_at_ms},
    };

    auto id = deps_.workflow->propose(std::move(p));
    if (id.empty()) {
        return GuardAdminResult::error(503, "autonomy_disabled",
                                        "workflow rejected propose");
    }
    return GuardAdminResult::ok({{"proposal_id", id}});
}

void GuardAdminController::recordExplanation(const std::string& request_id,
                                              const GuardExplanation& e) {
    std::lock_guard lock(explanations_mu_);
    // TASK-20260708-03 / REV20260707-C2 D3-A: first-write-wins.
    // Aligns with Pipeline::process early-stop semantics — the first
    // Reject-triggering stage wins the explanation slot for the
    // request_id, so the JSON returned to the admin UI is deterministic
    // (independent of stage traversal order). emplace is a no-op if the
    // key is already present.
    explanations_.emplace(request_id, e);
}

}  // namespace aegisgate::guard
