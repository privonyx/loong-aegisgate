#pragma once

// Phase 11.1 TASK-20260523-01 — Epic 5 GuardAdminController.
//
// Lightweight orchestrator over the existing primitives (sink, rate
// limiter, registry, workflow, audit). Returns a stable GuardAdminResult
// (status + JSON body + error_code) so the HTTP layer can map 1:1 without
// re-implementing the policy logic.
//
// Endpoints (mirrors design §6):
//   POST /admin/api/guard/feedback          -> postFeedback
//   GET  /admin/api/guard/explanation/{id}  -> getExplanation
//   POST /admin/api/guard/model/promote     -> promoteModel
//
// Auth-context is kept tiny here (role + user + tenant) so unit tests don't
// have to drag in the full RBAC machinery; production wiring fills these
// from AuthService.

#include "aegisgate/guard_explanation.h"
#include "guardrail/feedback/guard_feedback_payload.h"

#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

namespace aegisgate {
class AuditLogger;
}  // namespace aegisgate

namespace aegisgate::autonomy {
class AutonomyApprovalWorkflow;
}  // namespace aegisgate::autonomy

namespace aegisgate::guard {

class GuardFeedbackSink;
class GuardFeedbackRateLimiter;
class GuardFeedbackAnomalyDetector;
class IGuardModelRegistry;

struct GuardAdminContext {
    std::string role;
    std::string user_id;
    std::string tenant_id;
};

struct GuardAdminResult {
    int status = 200;
    nlohmann::json body = nlohmann::json::object();
    std::string error_code;
    bool is_error = false;

    static GuardAdminResult ok(nlohmann::json b) {
        return {200, std::move(b), {}, false};
    }
    static GuardAdminResult error(int s, std::string code, std::string msg = {}) {
        GuardAdminResult r;
        r.status = s; r.error_code = code; r.is_error = true;
        r.body = {{"error", {{"code", code}, {"message", std::move(msg)}}}};
        return r;
    }
};

struct GuardAdminDeps {
    std::shared_ptr<GuardFeedbackSink> sink;
    std::shared_ptr<GuardFeedbackRateLimiter> rate_limiter;
    std::shared_ptr<GuardFeedbackAnomalyDetector> anomaly_detector;
    std::shared_ptr<IGuardModelRegistry> registry;
    std::shared_ptr<aegisgate::autonomy::AutonomyApprovalWorkflow> workflow;
    std::shared_ptr<aegisgate::AuditLogger> audit;
};

class GuardAdminController {
public:
    explicit GuardAdminController(GuardAdminDeps deps);

    GuardAdminResult postFeedback(const GuardAdminContext& ctx,
                                   const nlohmann::json& body);

    GuardAdminResult getExplanation(const GuardAdminContext& ctx,
                                     const std::string& request_id);

    GuardAdminResult promoteModel(const GuardAdminContext& ctx,
                                   const nlohmann::json& body);

    // Producer-side hook used by the guardrail stages to publish the
    // structured "why was this blocked" record. Stored in an in-memory
    // ring buffer indexed by request_id (v1 simple store; v2 will move
    // to PersistentStore once the explanation volume is understood).
    void recordExplanation(const std::string& request_id,
                            const GuardExplanation& e);

private:
    static bool roleAuthorized(const std::string& role);

    GuardAdminDeps deps_;

    mutable std::mutex explanations_mu_;
    std::unordered_map<std::string, GuardExplanation> explanations_;
};

}  // namespace aegisgate::guard
