#include "observe/autonomy/cost_autonomy_applier.h"

#include "gateway/ml_router.h"

#include <spdlog/spdlog.h>

namespace aegisgate::autonomy {

CostAutonomyApplier::CostAutonomyApplier(std::shared_ptr<MLRouter> router)
    : router_(std::move(router)) {}

int CostAutonomyApplier::rankTier(const std::string& tier) {
    if (tier == "economy")  return 0;
    if (tier == "standard") return 1;
    if (tier == "premium")  return 2;
    return -1;  // unknown — treated as "below economy" so R1 rejects
}

CostAutonomyApplier::PayloadView
CostAutonomyApplier::extract(const ApprovalProposal& p) {
    PayloadView v;
    if (!p.payload.is_object()) {
        v.error = "payload is not a JSON object";
        return v;
    }
    v.tenant_id = p.payload.value("tenant_id", std::string{});
    v.from_tier = p.payload.value("from_quality_tier", std::string{});
    v.to_tier   = p.payload.value("to_quality_tier",   std::string{});
    v.savings_usd_24h =
        p.payload.value("estimated_savings_usd_24h", 0.0);
    v.affected_rps    =
        p.payload.value("affected_requests_per_hour", 0);

    if (v.tenant_id.empty())  { v.error = "missing tenant_id"; return v; }
    if (v.from_tier.empty())  { v.error = "missing from_quality_tier"; return v; }
    if (v.to_tier.empty())    { v.error = "missing to_quality_tier"; return v; }
    v.schema_ok = true;
    return v;
}

ApplyResult CostAutonomyApplier::applyImpl(const ApprovalProposal& p,
                                            bool dry_run) {
    auto v = extract(p);
    if (!v.schema_ok) {
        return makeFailSchemaInvalid(v.error);
    }
    if (!router_) {
        return makeFailMissingDep("router_missing", "MLRouter not wired");
    }

    nlohmann::json details = {
        {"tenant_id",      v.tenant_id},
        {"from_tier",      v.from_tier},
        {"to_tier",        v.to_tier},
        {"router_before",  nullptr},
        {"router_after",   nullptr}};
    auto before = router_->getQualityTierOverride(v.tenant_id);
    details["router_before"] =
        before.has_value() ? nlohmann::json(*before) : nlohmann::json(nullptr);

    if (dry_run) {
        details["router_after"] = v.to_tier;
        return makeDryRunOk(std::move(details));
    }

    router_->overrideQualityTier(v.tenant_id, v.to_tier);
    auto after = router_->getQualityTierOverride(v.tenant_id);
    details["router_after"] =
        after.has_value() ? nlohmann::json(*after) : nlohmann::json(nullptr);

    auto r = ApplyResult::ok();
    r.details = std::move(details);
    return r;  // base wrapper fills duration_ms
}

ApplyResult CostAutonomyApplier::rollbackImpl(const ApprovalProposal& p) {
    auto v = extract(p);
    if (!v.schema_ok) {
        return makeFailSchemaInvalid(v.error);
    }
    if (!router_) {
        return makeFailMissingDep("router_missing", "MLRouter not wired");
    }

    nlohmann::json details = {
        {"tenant_id",     v.tenant_id},
        {"router_before", nullptr},
        {"router_after",  nullptr}};
    auto before = router_->getQualityTierOverride(v.tenant_id);
    details["router_before"] =
        before.has_value() ? nlohmann::json(*before) : nlohmann::json(nullptr);

    router_->clearQualityTierOverride(v.tenant_id);
    auto after = router_->getQualityTierOverride(v.tenant_id);
    details["router_after"] =
        after.has_value() ? nlohmann::json(*after) : nlohmann::json(nullptr);

    auto r = ApplyResult::ok();
    r.details = std::move(details);
    return r;  // base wrapper fills duration_ms
}

bool CostAutonomyApplier::isLowRisk(const ApprovalProposal& p) const {
    auto v = extract(p);
    if (!v.schema_ok) {
        spdlog::debug("CostAutonomyApplier::isLowRisk: fail-closed schema: {}",
                      v.error);
        return false;  // fail closed → manual approval required
    }

    const int from = rankTier(v.from_tier);
    const int to   = rankTier(v.to_tier);
    if (from < 0 || to < 0) return false;  // unknown tier name

    // R1: only allow downgrade (to must be strictly lower-rank than from).
    if (to >= from)                  return false;
    // R2: no two-tier jumps (premium → economy = 2-step delta blocked).
    if (from - to > 1)               return false;
    // R3: savings cap.
    if (v.savings_usd_24h > 50.0)    return false;
    // R4: blast radius cap.
    if (v.affected_rps > 1000)       return false;
    return true;
}

} // namespace aegisgate::autonomy
