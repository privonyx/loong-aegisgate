// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 1 task 1.5.
//
// RecoveryApplier tests (13).
//
// Coverage:
//   override_quality_tier:
//     1. ApplyValidPayloadSucceeds
//     2. ApplyDryRunDoesNotMutate            — M4 mutation target (SR2)
//     3. RollbackReversesEffect
//   switch_router_fallback:
//     4. SwitchRouterFallbackEmitsAuditAndWarn
//   switch_connector:
//     5. SwitchConnectorEmitsAuditAndWarn
//   apply_budget_cap:
//     6. ApplyValidatesNewCapNonNegative
//     7. ApplyMutatesBudgetCap
//   propose_hpa_scale:
//     8. ProposeHpaScaleEmitsAuditAndWarn
//   send_webhook:
//     9. SendWebhookEmitsAuditAndWarn        — v1 stub (no real HTTP)
//   audit_only:
//    10. AuditOnlyWritesAudit
//   Cross-cutting:
//    11. IsLowRiskForOverrideTierWithinLimit
//    12. IsHighRiskForLargeBudgetCapChange
//    13. ApplyUnknownActionTypeFails

#include "gateway/ml_router.h"
#include "guardrail/audit.h"
#include "observe/autonomy/approval_proposal.h"
#include "observe/recovery/recovery_applier.h"
#include "server/budget_guard_stage.h"

#include <gtest/gtest.h>

#include <memory>

using namespace aegisgate;
using namespace aegisgate::autonomy;

namespace {

ApprovalProposal makeProposal(const std::string& action,
                                nlohmann::json extra = nlohmann::json::object()) {
    ApprovalProposal p;
    p.id = "prop-" + action;
    p.source = AutonomySource::AutoRecovery;
    p.subject = "tenant-test";
    p.payload = {{"action", action}};
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        p.payload[it.key()] = it.value();
    }
    p.state = ApprovalState::APPROVED;
    return p;
}

struct TestFixture {
    std::shared_ptr<MLRouter>         router       = std::make_shared<MLRouter>();
    std::shared_ptr<AuditLogger>      audit        = std::make_shared<AuditLogger>();
    std::shared_ptr<BudgetGuardStage> budget_guard;
    RecoveryApplier::Deps             deps;

    TestFixture() {
        BudgetGuardConfig cfg;
        cfg.enabled = true;
        cfg.per_tenant_24h_usd  = 100.0;
        cfg.per_request_max_usd = 1.0;
        budget_guard =
            std::make_shared<BudgetGuardStage>(nullptr, router, cfg);
        deps.router       = router;
        deps.budget_guard = budget_guard;
        deps.audit        = audit;
    }
};

} // namespace

// --- 1: override_quality_tier apply path -----------------------------------

TEST(RecoveryApplierTest, ApplyValidPayloadSucceeds) {
    TestFixture f;
    RecoveryApplier applier(f.deps);

    auto p = makeProposal("override_quality_tier", {
        {"tenant_id", "tenant-A"},
        {"to_quality_tier", "economy"}});

    auto r = applier.apply(p, /*dry_run=*/false);

    ASSERT_TRUE(r.success) << r.error_code << ": " << r.error_message;
    EXPECT_EQ(f.router->getQualityTierOverride("tenant-A").value_or(""),
              "economy");
}

// --- 2: dry_run guard ------------------------------------------------------

TEST(RecoveryApplierTest, ApplyDryRunDoesNotMutate) {
    TestFixture f;
    RecoveryApplier applier(f.deps);

    auto p = makeProposal("override_quality_tier", {
        {"tenant_id", "tenant-A"},
        {"to_quality_tier", "economy"}});

    auto r = applier.apply(p, /*dry_run=*/true);

    ASSERT_TRUE(r.success);
    EXPECT_FALSE(f.router->getQualityTierOverride("tenant-A").has_value())
        << "dry_run must NOT mutate MLRouter (M4 mutation target, SR2)";
    EXPECT_TRUE(r.details.value("dry_run", false));
}

// --- 3: rollback reverses --------------------------------------------------

TEST(RecoveryApplierTest, RollbackReversesEffect) {
    TestFixture f;
    RecoveryApplier applier(f.deps);

    auto p = makeProposal("override_quality_tier", {
        {"tenant_id", "tenant-A"},
        {"to_quality_tier", "economy"}});

    ASSERT_TRUE(applier.apply(p, /*dry_run=*/false).success);
    ASSERT_TRUE(f.router->getQualityTierOverride("tenant-A").has_value());

    auto r = applier.rollback(p);
    ASSERT_TRUE(r.success);
    EXPECT_FALSE(f.router->getQualityTierOverride("tenant-A").has_value());
}

// --- 4: switch_router_fallback ---------------------------------------------

TEST(RecoveryApplierTest, SwitchRouterFallbackEmitsAuditAndWarn) {
    TestFixture f;
    RecoveryApplier applier(f.deps);

    auto p = makeProposal("switch_router_fallback", {
        {"model_id", "gpt-4o"},
        {"off_until_ms", 60000}});

    auto r = applier.apply(p, /*dry_run=*/false);

    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.details.value("mode", ""), "advisory");
    // audit_only proposal should still write an audit entry.
    auto entries = f.audit->entries();
    ASSERT_FALSE(entries.empty());
    EXPECT_EQ(entries.back().action, "auto_recovery.apply.switch_router_fallback");
}

// --- 5: switch_connector ---------------------------------------------------

TEST(RecoveryApplierTest, SwitchConnectorEmitsAuditAndWarn) {
    TestFixture f;
    RecoveryApplier applier(f.deps);

    auto p = makeProposal("switch_connector", {
        {"from_provider", "openai"},
        {"to_provider", "anthropic"},
        {"until_ms", 60000}});

    auto r = applier.apply(p, /*dry_run=*/false);

    ASSERT_TRUE(r.success);
    auto entries = f.audit->entries();
    ASSERT_FALSE(entries.empty());
    EXPECT_EQ(entries.back().action, "auto_recovery.apply.switch_connector");
}

// --- 6: apply_budget_cap schema validation ---------------------------------

TEST(RecoveryApplierTest, ApplyValidatesNewCapNonNegative) {
    TestFixture f;
    RecoveryApplier applier(f.deps);

    auto p = makeProposal("apply_budget_cap", {
        {"new_per_tenant_24h_usd", -10.0}});

    auto r = applier.apply(p, /*dry_run=*/false);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_code, "schema_invalid");
}

// --- 7: apply_budget_cap mutates ------------------------------------------

TEST(RecoveryApplierTest, ApplyMutatesBudgetCap) {
    TestFixture f;
    RecoveryApplier applier(f.deps);

    auto p = makeProposal("apply_budget_cap", {
        {"new_per_tenant_24h_usd", 50.0}});

    auto r = applier.apply(p, /*dry_run=*/false);
    ASSERT_TRUE(r.success) << r.error_code;
    EXPECT_DOUBLE_EQ(f.budget_guard->config().per_tenant_24h_usd, 50.0);
}

// --- 8: propose_hpa_scale --------------------------------------------------

TEST(RecoveryApplierTest, ProposeHpaScaleEmitsAuditAndWarn) {
    TestFixture f;
    RecoveryApplier applier(f.deps);

    auto p = makeProposal("propose_hpa_scale", {
        {"target_replicas", 6},
        {"suggested_kubectl_command",
         "kubectl scale deployment/aegisgate --replicas=6"}});

    auto r = applier.apply(p, /*dry_run=*/false);
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.details.value("mode", ""), "advisory");
    auto entries = f.audit->entries();
    ASSERT_FALSE(entries.empty());
    EXPECT_EQ(entries.back().action, "auto_recovery.apply.propose_hpa_scale");
}

// --- 9: send_webhook -------------------------------------------------------

TEST(RecoveryApplierTest, SendWebhookEmitsAuditAndWarn) {
    TestFixture f;
    RecoveryApplier applier(f.deps);

    auto p = makeProposal("send_webhook", {
        {"url", "https://example.test/webhook"},
        {"body", "incident triggered"}});

    auto r = applier.apply(p, /*dry_run=*/false);
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.details.value("mode", ""), "advisory");
}

// --- 10: audit_only --------------------------------------------------------

TEST(RecoveryApplierTest, AuditOnlyWritesAudit) {
    TestFixture f;
    RecoveryApplier applier(f.deps);

    auto p = makeProposal("audit_only", {{"note", "manual review needed"}});

    auto r = applier.apply(p, /*dry_run=*/false);
    ASSERT_TRUE(r.success);
    auto entries = f.audit->entries();
    ASSERT_FALSE(entries.empty());
    EXPECT_EQ(entries.back().action, "auto_recovery.apply.audit_only");
}

// --- 11: isLowRisk override_tier within limit ------------------------------

TEST(RecoveryApplierTest, IsLowRiskForOverrideTierWithinLimit) {
    TestFixture f;
    RecoveryApplier applier(f.deps);

    auto p = makeProposal("override_quality_tier", {
        {"tenant_id", "tenant-A"},
        {"from_quality_tier", "standard"},
        {"to_quality_tier",   "economy"},
        {"estimated_savings_usd_24h", 10.0},
        {"affected_requests_per_hour", 100}});

    EXPECT_TRUE(applier.isLowRisk(p));
}

// --- 12: isLowRisk large budget cap change is HIGH risk --------------------

TEST(RecoveryApplierTest, IsHighRiskForLargeBudgetCapChange) {
    TestFixture f;
    RecoveryApplier applier(f.deps);

    auto p = makeProposal("apply_budget_cap", {
        {"new_per_tenant_24h_usd", 1.0},   // cuts cap from 100 to 1 — too aggressive
        {"current_per_tenant_24h_usd", 100.0}});

    EXPECT_FALSE(applier.isLowRisk(p));
}

// --- 13: unknown action ----------------------------------------------------

TEST(RecoveryApplierTest, ApplyUnknownActionTypeFails) {
    TestFixture f;
    RecoveryApplier applier(f.deps);

    auto p = makeProposal("delete_tenant", {{"tenant_id", "tenant-A"}});

    auto r = applier.apply(p, /*dry_run=*/false);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_code, "unknown_action");
}

// --- 14: SR17 layer 2 defense (TASK-20260523-03 net new) -------------------
//
// Pre-migration RecoveryApplier had NO SR17 layer 2 check (it relied on
// AutonomyApprovalWorkflow at layer 1 only). The TASK-20260523-03
// BaseAutonomyApplier migration adds SR17 layer 2 uniformly to all 5
// appliers — this test locks Recovery's net-new defense in place so a
// future "remove SR17 from base" mutation cannot regress Recovery
// silently while the other 4 appliers continue to fail.
//
// Layer-specific assertion (P1 已正式): the error_code "autonomy_disabled"
// is now produced by the BASE layer (base_autonomy_applier.cpp); this
// test passing means Recovery successfully inherits SR17 layer 2 from
// the new base, NOT that Recovery has its own copy of the check.

TEST(RecoveryApplierTest, RefusesWhenAutonomyDisabled) {
    TestFixture f;
    RecoveryApplier applier(f.deps);

    auto p = makeProposal("override_quality_tier", {
        {"tenant_id", "tenant-A"},
        {"to_quality_tier", "economy"}});

    ::setenv("AEGISGATE_DISABLE_AUTONOMY", "1", /*overwrite=*/1);
    auto r = applier.apply(p, /*dry_run=*/false);
    ::unsetenv("AEGISGATE_DISABLE_AUTONOMY");

    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_code, "autonomy_disabled")
        << "RecoveryApplier must inherit SR17 layer 2 from BaseAutonomyApplier";
    EXPECT_FALSE(f.router->getQualityTierOverride("tenant-A").has_value())
        << "SR17 trip must short-circuit BEFORE applyImpl mutates MLRouter";
}

// --- 15: SR17 layer 2 also gates rollback ----------------------------------

TEST(RecoveryApplierTest, RollbackRefusesWhenAutonomyDisabled) {
    TestFixture f;
    RecoveryApplier applier(f.deps);

    auto p = makeProposal("override_quality_tier", {
        {"tenant_id", "tenant-A"},
        {"to_quality_tier", "economy"}});

    ::setenv("AEGISGATE_DISABLE_AUTONOMY", "1", /*overwrite=*/1);
    auto r = applier.rollback(p);
    ::unsetenv("AEGISGATE_DISABLE_AUTONOMY");

    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_code, "autonomy_disabled");
}
