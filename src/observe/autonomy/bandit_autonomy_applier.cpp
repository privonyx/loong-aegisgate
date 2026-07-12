#include "observe/autonomy/bandit_autonomy_applier.h"

#include "gateway/bandit_router.h"

#include <spdlog/spdlog.h>

namespace aegisgate::autonomy {

BanditAutonomyApplier::BanditAutonomyApplier(
    std::shared_ptr<BanditRouter> router)
    : router_(std::move(router)) {}

BanditAutonomyApplier::PayloadView
BanditAutonomyApplier::extract(const ApprovalProposal& p) {
    PayloadView v;
    if (!p.payload.is_object()) {
        v.error = "payload is not a JSON object";
        return v;
    }
    v.action = p.payload.value("action", std::string{});
    v.strategy = p.payload.value("strategy", std::string{});
    v.canary_pct = p.payload.value("canary_pct", 0.0);

    if (p.payload.contains("shadow_metrics") &&
        p.payload["shadow_metrics"].is_object()) {
        const auto& sm = p.payload["shadow_metrics"];
        v.win_rate = sm.value("win_rate", 0.0);
        v.shadow_duration_min = sm.value("shadow_duration_min", 0);
        v.cost_delta_pct = sm.value("cost_delta_pct", 0.0);
    }

    if (v.action.empty()) { v.error = "missing action"; return v; }
    if (v.strategy.empty()) { v.error = "missing strategy"; return v; }
    v.schema_ok = true;
    return v;
}

ApplyResult BanditAutonomyApplier::applyImpl(const ApprovalProposal& p,
                                              bool dry_run) {
    auto v = extract(p);
    if (!v.schema_ok) {
        return makeFailSchemaInvalid(v.error);
    }
    if (!router_) {
        return makeFailMissingDep("router_missing", "BanditRouter not wired");
    }

    nlohmann::json details = {
        {"action",   v.action},
        {"strategy", v.strategy},
        {"canary_pct", v.canary_pct},
        {"mode_before", nullptr},
        {"mode_after",  nullptr}};
    details["mode_before"] = router_->getMode() == BanditMode::Live ? "Live"
                                                                       : "Shadow";

    if (dry_run) {
        details["mode_after"] =
            v.action == "revert_to_shadow" ? "Shadow" : "Live";
        return makeDryRunOk(std::move(details));
    }

    bool ok = false;
    if (v.action == "shadow_to_live" || v.action == "expand_canary") {
        ok = router_->transitionToLive(v.canary_pct);
    } else if (v.action == "revert_to_shadow") {
        router_->revertToShadow();
        ok = true;
    } else {
        return ApplyResult::fail("unknown_action", "action=" + v.action);
    }

    details["mode_after"] = router_->getMode() == BanditMode::Live ? "Live"
                                                                       : "Shadow";

    if (!ok) {
        return ApplyResult::fail("transition_refused",
                                  "BanditRouter rejected transition", details);
    }
    auto r = ApplyResult::ok();
    r.details = std::move(details);
    return r;  // base wrapper fills duration_ms
}

ApplyResult BanditAutonomyApplier::rollbackImpl(const ApprovalProposal& p) {
    auto v = extract(p);
    if (!v.schema_ok) {
        return makeFailSchemaInvalid(v.error);
    }
    if (!router_) {
        return makeFailMissingDep("router_missing", "BanditRouter not wired");
    }

    nlohmann::json details = {{"action", "revert_to_shadow"},
                              {"mode_before", nullptr},
                              {"mode_after", nullptr}};
    details["mode_before"] =
        router_->getMode() == BanditMode::Live ? "Live" : "Shadow";

    router_->revertToShadow();
    details["mode_after"] =
        router_->getMode() == BanditMode::Live ? "Live" : "Shadow";

    auto r = ApplyResult::ok();
    r.details = std::move(details);
    return r;  // base wrapper fills duration_ms
}

bool BanditAutonomyApplier::isLowRisk(const ApprovalProposal& p) const {
    auto v = extract(p);
    if (!v.schema_ok) {
        spdlog::debug(
            "BanditAutonomyApplier::isLowRisk: fail-closed schema: {}",
            v.error);
        return false;
    }

    // R1: shadow_metrics.win_rate >= 0.55  (beat baseline statistically)
    if (v.win_rate < 0.55) return false;
    // R2: shadow_duration_min >= 60  (≥1h shadow required)
    if (v.shadow_duration_min < 60) return false;
    // R3: canary_pct <= 0.05  (start small per SR5)
    if (v.canary_pct > 0.05) return false;
    // R4: cost_delta_pct >= -20  (no single big swing)
    if (v.cost_delta_pct < -20.0) return false;
    return true;
}

}  // namespace aegisgate::autonomy
