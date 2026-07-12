// Phase 11.1 TASK-20260523-01 — Epic 5 GuardAdminController tests.
//
// Verifies the three admin endpoints:
//   POST /admin/api/guard/feedback           -> 200 / 400 / 401 / 403 / 429
//   GET  /admin/api/guard/explanation/{id}   -> 200 / 404
//   POST /admin/api/guard/model/promote      -> 200 (workflow proposal id) / 4xx
//   (TASK-20260706-01: routes migrated from /v1/guard/* into admin cookie scope)
//
// Each handler:
//   * audits the call (SR5)
//   * enforces role allowlist (D4=C)
//   * enforces rate limits (D4=C / SR-NEW2)
//   * masks PII before persisting

#include "guardrail/admin/guard_admin_controller.h"
#include "guardrail/audit.h"
#include "guardrail/autonomy/guard_model_applier.h"
#include "guardrail/feedback/guard_feedback_anomaly_detector.h"
#include "guardrail/feedback/guard_feedback_payload.h"
#include "guardrail/feedback/guard_feedback_rate_limiter.h"
#include "guardrail/feedback/guard_feedback_sink.h"
#include "guardrail/inbound/pii_filter.h"
#include "guardrail/model/memory_guard_model_registry.h"
#include "observe/autonomy/approval_queue.h"
#include "observe/autonomy/approval_workflow.h"
#include "observe/feedback_bus.h"

#include <gtest/gtest.h>

#include <memory>

using aegisgate::AuditLogger;
using aegisgate::FeedbackBus;
using aegisgate::FeedbackBusConfig;
using aegisgate::PIIFilter;
using aegisgate::autonomy::ApprovalQueue;
using aegisgate::autonomy::AutonomyApprovalWorkflow;
using aegisgate::guard::GuardAdminController;
using aegisgate::guard::GuardAdminContext;
using aegisgate::guard::GuardAdminResult;
using aegisgate::guard::GuardFeedbackRateLimitConfig;
using aegisgate::guard::GuardFeedbackRateLimiter;
using aegisgate::guard::GuardFeedbackSink;
using aegisgate::guard::GuardModelApplier;
using aegisgate::guard::GuardModelStatus;
using aegisgate::guard::MemoryGuardModelRegistry;
using aegisgate::guard::ModelRegistryRecord;

namespace {

struct TestStack {
    std::shared_ptr<PIIFilter> pii;
    std::shared_ptr<AuditLogger> audit;
    std::shared_ptr<FeedbackBus> bus;
    std::shared_ptr<GuardFeedbackSink> sink;
    std::shared_ptr<GuardFeedbackRateLimiter> rl;
    std::shared_ptr<aegisgate::guard::GuardFeedbackAnomalyDetector> anomaly;
    std::shared_ptr<MemoryGuardModelRegistry> registry;
    std::shared_ptr<ApprovalQueue> queue;
    std::shared_ptr<AutonomyApprovalWorkflow> workflow;
    std::shared_ptr<GuardModelApplier> applier;
    std::unique_ptr<GuardAdminController> controller;
};

TestStack buildStack(GuardFeedbackRateLimitConfig rl_cfg = {}) {
    TestStack s;
    s.pii = std::make_shared<PIIFilter>();
    s.audit = std::make_shared<AuditLogger>();
    FeedbackBusConfig cfg; cfg.enabled = true;
    s.bus = std::make_shared<FeedbackBus>(cfg);
    s.bus->start();
    s.sink = std::make_shared<GuardFeedbackSink>(
        aegisgate::guard::GuardFeedbackSinkDeps{s.pii, s.audit, s.bus});
    s.rl = std::make_shared<GuardFeedbackRateLimiter>(rl_cfg);

    s.registry = std::make_shared<MemoryGuardModelRegistry>();
    ModelRegistryRecord live;
    live.model_id = "guardrail"; live.version = "v1";
    live.status = GuardModelStatus::Live; live.artifact_sha256 = "sha-v1";
    s.registry->insert(live);
    ModelRegistryRecord shadow;
    shadow.model_id = "guardrail"; shadow.version = "v2";
    shadow.status = GuardModelStatus::Shadow; shadow.artifact_sha256 = "sha-v2";
    s.registry->insert(shadow);

    s.queue = std::make_shared<ApprovalQueue>(nullptr);
    s.workflow = std::make_shared<AutonomyApprovalWorkflow>(s.queue, s.audit);
    s.workflow->setAutonomyEnabledOverride(true);
    s.applier = std::make_shared<GuardModelApplier>(s.registry);
    s.workflow->registerApplier(
        aegisgate::autonomy::AutonomySource::AdaptiveGuard, s.applier);

    aegisgate::guard::GuardAdminDeps deps;
    deps.sink = s.sink;
    deps.rate_limiter = s.rl;
    deps.registry = s.registry;
    deps.workflow = s.workflow;
    deps.audit = s.audit;
    aegisgate::guard::GuardFeedbackAnomalyConfig acfg;
    acfg.reviewer_fp_threshold = 100;  // out of unit-test range by default
    s.anomaly = std::make_shared<aegisgate::guard::GuardFeedbackAnomalyDetector>(acfg);
    deps.anomaly_detector = s.anomaly;
    s.controller = std::make_unique<GuardAdminController>(std::move(deps));
    return s;
}

GuardAdminContext adminCtx(std::string role = "security_admin",
                            std::string user = "alice",
                            std::string tenant = "tenant-A") {
    return GuardAdminContext{std::move(role), std::move(user),
                              std::move(tenant)};
}

}  // namespace

TEST(GuardAdminControllerTest, PostFeedbackHappyPath) {
    auto s = buildStack();
    nlohmann::json body = {
        {"request_id", "req-1"},
        {"label", "false_positive"},
        {"reviewer_user_id", "alice"},
        {"reviewer_role", "security_admin"},
        {"comment", "Legit question about TLS"},
    };
    auto r = s.controller->postFeedback(adminCtx(), body);
    EXPECT_EQ(r.status, 200) << r.body.dump();
    EXPECT_FALSE(r.is_error);
    s.bus->shutdown();
}

TEST(GuardAdminControllerTest, PostFeedbackRejectsUnauthorizedRole) {
    auto s = buildStack();
    nlohmann::json body = {
        {"request_id", "req-1"},
        {"label", "false_positive"},
        {"reviewer_user_id", "u1"},
        {"reviewer_role", "end_user"},
    };
    auto r = s.controller->postFeedback(adminCtx("end_user", "u1"), body);
    EXPECT_EQ(r.status, 403);
    EXPECT_EQ(r.error_code, "unauthorized_role");
    s.bus->shutdown();
}

TEST(GuardAdminControllerTest, PostFeedbackMissingFieldReturns400) {
    auto s = buildStack();
    nlohmann::json body = {
        {"request_id", "req-1"},
        {"reviewer_user_id", "alice"},
        {"reviewer_role", "security_admin"},
        // missing label
    };
    auto r = s.controller->postFeedback(adminCtx(), body);
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(r.error_code, "missing_field");
    s.bus->shutdown();
}

TEST(GuardAdminControllerTest, PostFeedbackRateLimitReturns429) {
    GuardFeedbackRateLimitConfig cfg;
    cfg.per_tenant_per_min = 1;
    cfg.per_reviewer_per_min = 100;
    cfg.global_per_min = 100;
    auto s = buildStack(cfg);
    nlohmann::json body = {
        {"request_id", "req-1"},
        {"label", "false_positive"},
        {"reviewer_user_id", "alice"},
        {"reviewer_role", "security_admin"},
    };
    EXPECT_EQ(s.controller->postFeedback(adminCtx(), body).status, 200);
    auto r = s.controller->postFeedback(adminCtx(), body);
    EXPECT_EQ(r.status, 429);
    EXPECT_EQ(r.error_code, "tenant_quota_exceeded");
    s.bus->shutdown();
}

TEST(GuardAdminControllerTest, PostFeedbackAnomalyDetectorFlagsBurst) {
    // SR-NEW1 layer 3 — a single reviewer flooding false_positive labels must
    // be flagged BEFORE the sink, and an audit row recorded.
    GuardFeedbackRateLimitConfig rl;
    rl.per_tenant_per_min = 1000;
    rl.per_reviewer_per_min = 1000;
    rl.global_per_min = 1000;
    auto s = buildStack(rl);
    aegisgate::guard::GuardFeedbackAnomalyConfig acfg;
    acfg.reviewer_fp_threshold = 2;
    s.anomaly =
        std::make_shared<aegisgate::guard::GuardFeedbackAnomalyDetector>(acfg);
    aegisgate::guard::GuardAdminDeps deps;
    deps.sink = s.sink; deps.rate_limiter = s.rl; deps.registry = s.registry;
    deps.workflow = s.workflow; deps.audit = s.audit;
    deps.anomaly_detector = s.anomaly;
    s.controller = std::make_unique<GuardAdminController>(std::move(deps));

    auto submit = [&](const std::string& rid) {
        nlohmann::json body = {
            {"request_id", rid},
            {"label", "false_positive"},
            {"reviewer_user_id", "mallory"},
            {"reviewer_role", "security_admin"},
        };
        return s.controller->postFeedback(adminCtx("security_admin", "mallory"), body);
    };
    EXPECT_EQ(submit("req-a").status, 200);
    EXPECT_EQ(submit("req-b").status, 200);
    auto r = submit("req-c");
    EXPECT_EQ(r.status, 409);
    EXPECT_EQ(r.error_code, "reviewer_fp_burst");

    s.audit->flush();
    bool flagged = false;
    for (const auto& e : s.audit->entries()) {
        if (e.action == "feedback_anomaly_flag" && e.request_id == "req-c") {
            flagged = true; break;
        }
    }
    EXPECT_TRUE(flagged) << "expected feedback_anomaly_flag audit row";
    s.bus->shutdown();
}

TEST(GuardAdminControllerTest, PostFeedbackAuditsCall) {
    // SR5 — every accepted feedback must persist through AuditLogger.
    auto s = buildStack();
    nlohmann::json body = {
        {"request_id", "req-sr5"},
        {"label", "false_positive"},
        {"reviewer_user_id", "alice"},
        {"reviewer_role", "security_admin"},
    };
    auto r = s.controller->postFeedback(adminCtx(), body);
    ASSERT_EQ(r.status, 200);
    s.audit->flush();

    bool seen = false;
    for (const auto& e : s.audit->entries()) {
        if (e.action == "guard_feedback" && e.request_id == "req-sr5") {
            seen = true;
            break;
        }
    }
    EXPECT_TRUE(seen) << "missing audit entry for guard_feedback";
    s.bus->shutdown();
}

TEST(GuardAdminControllerTest, PromoteModelEnqueuesProposal) {
    auto s = buildStack();
    nlohmann::json body = {
        {"action", "promote_shadow_to_live"},
        {"model_id", "guardrail"},
        {"version", "v2"},
        {"shadow_metrics", {{"win_rate", 0.6},
                              {"shadow_duration_min", 90},
                              {"fp_rate_delta", -0.04}}},
    };
    auto r = s.controller->promoteModel(adminCtx(), body);
    EXPECT_EQ(r.status, 200) << r.body.dump();
    EXPECT_TRUE(r.body.contains("proposal_id"));
    auto id = r.body["proposal_id"].get<std::string>();
    EXPECT_FALSE(id.empty());
    s.bus->shutdown();
}

TEST(GuardAdminControllerTest, PromoteModelRejectsUnauthorizedRole) {
    auto s = buildStack();
    nlohmann::json body = {
        {"action", "promote_shadow_to_live"},
        {"model_id", "guardrail"},
        {"version", "v2"},
    };
    auto r = s.controller->promoteModel(adminCtx("end_user", "u1"), body);
    EXPECT_EQ(r.status, 403);
    s.bus->shutdown();
}

TEST(GuardAdminControllerTest, GetExplanationReturns404WhenAbsent) {
    auto s = buildStack();
    auto r = s.controller->getExplanation(adminCtx(), "missing-req");
    EXPECT_EQ(r.status, 404);
    s.bus->shutdown();
}

TEST(GuardAdminControllerTest, GetExplanationReturnsStoredEntry) {
    auto s = buildStack();
    // Seed by recording an explanation via the controller (or directly).
    aegisgate::GuardExplanation e;
    e.trigger_layer = "L3";
    e.trigger_rule_id = "model_classifier_v3";
    e.model_version = "v2";
    e.threshold = 0.6f;
    e.confidence = 0.91f;
    e.explanation_text = "Blocked by ML classifier";
    s.controller->recordExplanation("req-42", e);

    auto r = s.controller->getExplanation(adminCtx(), "req-42");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["trigger_layer"], "L3");
    EXPECT_EQ(r.body["model_version"], "v2");
    s.bus->shutdown();
}

// TASK-20260708-03 / REV20260707-C2 Epic 1 — D3-A first-write-wins.
// Adaptive Guard's pipeline early-stops on the first Reject: same
// request_id is realistically only written once. But if a future
// change ever surfaces multiple Reject-triggering stages per request,
// the second call MUST NOT overwrite the first stage's explanation —
// otherwise the JSON returned to the admin UI would be non-deterministic
// (dependent on stage traversal order). Contract locked here.
TEST(GuardAdminControllerTest, RecordExplanationFirstWriteWins) {
    auto s = buildStack();

    aegisgate::GuardExplanation first;
    first.trigger_layer = "L1";
    first.trigger_rule_id = "injection_pattern_A";
    first.confidence = 0.95f;
    first.explanation_text = "Injection signal matched at L1";
    s.controller->recordExplanation("req-dup", first);

    // Second write for the same request_id — different stage / layer.
    // D3-A: this MUST be a no-op; first-write-wins.
    aegisgate::GuardExplanation second;
    second.trigger_layer = "L3";
    second.trigger_rule_id = "model_classifier_v3";
    second.confidence = 0.72f;
    second.explanation_text = "ML classifier verdict=unsafe";
    s.controller->recordExplanation("req-dup", second);

    auto r = s.controller->getExplanation(adminCtx(), "req-dup");
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.body["trigger_layer"], "L1")
        << "D3-A first-write-wins violated: second call overwrote the first "
        << "stage's explanation. explanations_ must use emplace, not operator[].";
    EXPECT_EQ(r.body["trigger_rule_id"], "injection_pattern_A");
    s.bus->shutdown();
}
