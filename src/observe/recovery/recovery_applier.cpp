// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 1 task 1.5.

#include "observe/recovery/recovery_applier.h"

#include "gateway/ml_router.h"
#include "guardrail/audit.h"
#include "server/budget_guard_stage.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

namespace aegisgate::autonomy {

namespace {

constexpr char kActionOverrideQualityTier[]  = "override_quality_tier";
constexpr char kActionSwitchRouterFallback[] = "switch_router_fallback";
constexpr char kActionSwitchConnector[]      = "switch_connector";
constexpr char kActionApplyBudgetCap[]       = "apply_budget_cap";
constexpr char kActionProposeHpaScale[]      = "propose_hpa_scale";
constexpr char kActionSendWebhook[]          = "send_webhook";
constexpr char kActionAuditOnly[]            = "audit_only";

int rankTier(const std::string& tier) {
    if (tier == "economy")  return 0;
    if (tier == "standard") return 1;
    if (tier == "premium")  return 2;
    return -1;
}

} // namespace

const std::vector<std::string>& RecoveryApplier::knownActions() {
    static const std::vector<std::string> kKnown = {
        kActionOverrideQualityTier,  kActionSwitchRouterFallback,
        kActionSwitchConnector,      kActionApplyBudgetCap,
        kActionProposeHpaScale,      kActionSendWebhook,
        kActionAuditOnly,
    };
    return kKnown;
}

RecoveryApplier::RecoveryApplier(Deps deps) : deps_(std::move(deps)) {}

std::string RecoveryApplier::extractAction(const ApprovalProposal& p) {
    if (!p.payload.is_object()) return {};
    return p.payload.value("action", std::string{});
}

void RecoveryApplier::writeAudit(const ApprovalProposal& p,
                                   const std::string& action,
                                   const std::string& detail) {
    if (!deps_.audit) return;
    deps_.audit->logAction(/*request_id=*/p.id,
                            /*tenant_id=*/p.subject,
                            /*stage=*/"AutoRecovery",
                            action,
                            detail);
}

// --- override_quality_tier --------------------------------------------------

ApplyResult RecoveryApplier::handleOverrideQualityTier(
    const ApprovalProposal& p, bool dry_run) {
    if (!p.payload.is_object()) {
        return ApplyResult::fail("schema_invalid", "payload not object");
    }
    const std::string tenant = p.payload.value("tenant_id", std::string{});
    const std::string to_tier = p.payload.value("to_quality_tier", std::string{});
    if (tenant.empty() || to_tier.empty()) {
        return ApplyResult::fail("schema_invalid",
                                  "missing tenant_id or to_quality_tier");
    }
    if (!deps_.router) {
        return ApplyResult::fail("router_missing", "MLRouter not wired");
    }

    nlohmann::json details = {
        {"action",        kActionOverrideQualityTier},
        {"tenant_id",     tenant},
        {"to_tier",       to_tier},
        {"router_before", nullptr},
        {"router_after",  nullptr}};
    auto before = deps_.router->getQualityTierOverride(tenant);
    details["router_before"] =
        before.has_value() ? nlohmann::json(*before) : nlohmann::json(nullptr);

    if (dry_run) {
        details["dry_run"]       = true;
        details["router_after"]  = to_tier;
        auto r = ApplyResult::ok();
        r.details = std::move(details);
        return r;
    }

    deps_.router->overrideQualityTier(tenant, to_tier);
    auto after = deps_.router->getQualityTierOverride(tenant);
    details["router_after"] =
        after.has_value() ? nlohmann::json(*after) : nlohmann::json(nullptr);

    writeAudit(p, "auto_recovery.apply." + std::string(kActionOverrideQualityTier),
                "tenant=" + tenant + " to_tier=" + to_tier);
    auto r = ApplyResult::ok();
    r.details = std::move(details);
    return r;
}

ApplyResult RecoveryApplier::rollbackOverrideQualityTier(
    const ApprovalProposal& p) {
    if (!p.payload.is_object()) {
        return ApplyResult::fail("schema_invalid", "payload not object");
    }
    const std::string tenant = p.payload.value("tenant_id", std::string{});
    if (tenant.empty()) {
        return ApplyResult::fail("schema_invalid", "missing tenant_id");
    }
    if (!deps_.router) {
        return ApplyResult::fail("router_missing", "MLRouter not wired");
    }
    deps_.router->clearQualityTierOverride(tenant);
    writeAudit(p, "auto_recovery.rollback." + std::string(kActionOverrideQualityTier),
                "tenant=" + tenant);
    auto r = ApplyResult::ok();
    r.details = {{"tenant_id", tenant}, {"cleared", true}};
    return r;
}

// --- apply_budget_cap -------------------------------------------------------

ApplyResult RecoveryApplier::handleApplyBudgetCap(
    const ApprovalProposal& p, bool dry_run) {
    if (!p.payload.is_object()) {
        return ApplyResult::fail("schema_invalid", "payload not object");
    }
    if (!p.payload.contains("new_per_tenant_24h_usd")) {
        return ApplyResult::fail("schema_invalid",
                                  "missing new_per_tenant_24h_usd");
    }
    const double new_cap =
        p.payload.value("new_per_tenant_24h_usd", -1.0);
    if (new_cap < 0.0) {
        return ApplyResult::fail("schema_invalid",
                                  "new_per_tenant_24h_usd must be >= 0");
    }
    if (!deps_.budget_guard) {
        return ApplyResult::fail("budget_guard_missing",
                                  "BudgetGuardStage not wired");
    }

    auto cfg = deps_.budget_guard->config();
    nlohmann::json details = {
        {"action",                kActionApplyBudgetCap},
        {"previous_cap",          cfg.per_tenant_24h_usd},
        {"new_cap",               new_cap}};

    if (dry_run) {
        details["dry_run"] = true;
        auto r = ApplyResult::ok();
        r.details = std::move(details);
        return r;
    }

    cfg.per_tenant_24h_usd = new_cap;
    deps_.budget_guard->setConfig(cfg);

    writeAudit(p, "auto_recovery.apply." + std::string(kActionApplyBudgetCap),
                "new_cap_usd=" + std::to_string(new_cap));
    auto r = ApplyResult::ok();
    r.details = std::move(details);
    return r;
}

ApplyResult RecoveryApplier::rollbackApplyBudgetCap(
    const ApprovalProposal& p) {
    if (!p.payload.is_object()) {
        return ApplyResult::fail("schema_invalid", "payload not object");
    }
    if (!p.payload.contains("previous_per_tenant_24h_usd")) {
        return ApplyResult::fail(
            "schema_invalid",
            "rollback needs previous_per_tenant_24h_usd in payload");
    }
    if (!deps_.budget_guard) {
        return ApplyResult::fail("budget_guard_missing",
                                  "BudgetGuardStage not wired");
    }
    const double prev_cap =
        p.payload.value("previous_per_tenant_24h_usd", -1.0);
    if (prev_cap < 0.0) {
        return ApplyResult::fail("schema_invalid", "negative previous cap");
    }
    auto cfg = deps_.budget_guard->config();
    cfg.per_tenant_24h_usd = prev_cap;
    deps_.budget_guard->setConfig(cfg);
    writeAudit(p, "auto_recovery.rollback." + std::string(kActionApplyBudgetCap),
                "restored_cap_usd=" + std::to_string(prev_cap));
    auto r = ApplyResult::ok();
    r.details = {{"restored_cap", prev_cap}};
    return r;
}

// --- advisory actions ------------------------------------------------------

ApplyResult RecoveryApplier::handleAdvisory(const ApprovalProposal& p,
                                              const std::string& action,
                                              bool /*dry_run*/) {
    // Advisory actions never mutate production state; dry_run is therefore
    // a no-op (audit entry still useful for "what would I have done?" runs,
    // but we mark mode=advisory in details for clarity).
    spdlog::warn("[RecoveryApplier] advisory action={} subject={} payload={}",
                  action, p.subject, p.payload.dump());

    writeAudit(p, "auto_recovery.apply." + action,
                "advisory: " + p.payload.dump());

    nlohmann::json details = {
        {"action", action},
        {"mode",   "advisory"}};
    auto r = ApplyResult::ok();
    r.details = std::move(details);
    return r;
}

// --- dispatcher ------------------------------------------------------------

ApplyResult RecoveryApplier::applyImpl(const ApprovalProposal& p, bool dry_run) {
    const std::string action = extractAction(p);

    if (action == kActionOverrideQualityTier) {
        return handleOverrideQualityTier(p, dry_run);
    }
    if (action == kActionApplyBudgetCap) {
        return handleApplyBudgetCap(p, dry_run);
    }
    if (action == kActionSwitchRouterFallback ||
        action == kActionSwitchConnector ||
        action == kActionProposeHpaScale ||
        action == kActionSendWebhook ||
        action == kActionAuditOnly) {
        return handleAdvisory(p, action, dry_run);
    }
    return ApplyResult::fail("unknown_action",
                              "unrecognised action: " + action);
    // base wrapper fills duration_ms
}

ApplyResult RecoveryApplier::rollbackImpl(const ApprovalProposal& p) {
    const std::string action = extractAction(p);
    if (action == kActionOverrideQualityTier) {
        return rollbackOverrideQualityTier(p);
    }
    if (action == kActionApplyBudgetCap) {
        return rollbackApplyBudgetCap(p);
    }
    if (action == kActionSwitchRouterFallback ||
        action == kActionSwitchConnector ||
        action == kActionProposeHpaScale ||
        action == kActionSendWebhook ||
        action == kActionAuditOnly) {
        // Advisory actions are no-ops; rollback also writes an audit
        // line for completeness.
        writeAudit(p, "auto_recovery.rollback." + action,
                    "advisory rollback (no-op)");
        auto r = ApplyResult::ok();
        r.details = {{"action", action}, {"mode", "advisory_noop"}};
        return r;
    }
    return ApplyResult::fail("unknown_action",
                              "unrecognised action: " + action);
    // base wrapper fills duration_ms
}

// --- isLowRisk -------------------------------------------------------------

bool RecoveryApplier::isLowRisk(const ApprovalProposal& p) const {
    const std::string action = extractAction(p);
    if (!p.payload.is_object()) return false;

    if (action == kActionOverrideQualityTier) {
        const std::string from_tier =
            p.payload.value("from_quality_tier", std::string{});
        const std::string to_tier =
            p.payload.value("to_quality_tier",   std::string{});
        const double savings =
            p.payload.value("estimated_savings_usd_24h", 0.0);
        const int    rps =
            p.payload.value("affected_requests_per_hour", 0);

        const int from = rankTier(from_tier);
        const int to   = rankTier(to_tier);
        if (from < 0 || to < 0) return false;
        if (to >= from)         return false;     // only downgrade
        if (from - to > 1)      return false;     // no double-step
        if (savings > 50.0)     return false;
        if (rps     > 1000)     return false;
        return true;
    }

    if (action == kActionApplyBudgetCap) {
        const double new_cap =
            p.payload.value("new_per_tenant_24h_usd", -1.0);
        const double cur_cap =
            p.payload.value("current_per_tenant_24h_usd", -1.0);
        if (new_cap < 0.0 || cur_cap <= 0.0) return false;
        // Only allow shrinking by ≤ 50%; cuts below half cap need a human.
        if (new_cap < cur_cap * 0.5) return false;
        return true;
    }

    // Advisory actions never auto-apply.
    return false;
}

} // namespace aegisgate::autonomy
