#pragma once

// Phase 11.5 BudgetGuardStage (TASK-20260518-02) — Epic 3.1.
//
// Inbound pipeline stage that enforces per-tenant 24h spend caps and
// per-request cost estimates. Sits between ResolveStage (which resolves
// the requested model) and the Router (which dispatches it), so the
// downgrade decision can mutate ctx.chat_request.extra["quality_tier"]
// before routing.
//
// Behaviour (design spec §4.4 + spec §6.1):
//   - if tenant_24h_cost + estimated_request_cost > per_tenant_24h_usd
//     OR estimated_request_cost > per_request_max_usd:
//       fail-CLOSED: stamp quality_tier="economy" + response header
//       X-AegisGate-Budget-Guard=triggered + metrics counter inc.
//       The request continues (no Reject) — user experience degrades
//       gracefully rather than 429ing.
//   - if BudgetGuardStage itself throws (CostTracker unreachable etc.):
//       fail-OPEN per config (default) so an observability bug never
//       takes the data plane down. ERROR logged so SRE notices.
//
// Audit: T08 defence. Every threshold change in CONFIG (not per-request
// trigger; that would flood the log) is written to AuditLogger by the
// outer config-reload pipeline, not here. This stage records per-trigger
// activity in metrics only.

#include "core/pipeline.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>

namespace aegisgate {
class CostTracker;  // forward
class MLRouter;     // forward (optional — currently unused; reserved for E4.2)

struct BudgetGuardConfig {
    bool   enabled                  = false;
    double per_tenant_24h_usd       = 100.0;
    double per_request_max_usd      = 1.0;
    bool   fail_open_on_error       = true;
    // Header to stamp on downgrade. Empty disables the header.
    std::string downgrade_header_name  = "X-AegisGate-Budget-Guard";
    std::string downgrade_header_value = "triggered";
    // Target tier on trigger. Defaults to "economy" per design §5.
    std::string downgrade_tier      = "economy";
};

class BudgetGuardStage : public PipelineStage {
public:
    // tracker: required. router: reserved (currently unused but parameter
    // matches plan §D Task 3.1 signature so the GatewayRuntime wiring
    // in E4.2 doesn't need a second refactor).
    BudgetGuardStage(std::shared_ptr<CostTracker> tracker,
                     std::shared_ptr<MLRouter>   router,
                     BudgetGuardConfig           cfg);

    StageResult process(RequestContext& ctx) override;
    std::string name() const override { return "BudgetGuardStage"; }

    // Live config reload (no restart needed — design §7.2 case 6).
    void setConfig(const BudgetGuardConfig& cfg);
    BudgetGuardConfig config() const;

private:
    // Returns the cost we'd attribute to the in-flight request. Uses
    // tokens_estimated when populated, otherwise a small floor so a
    // first-request-of-window is never zero-cost.
    double estimateRequestCost(const RequestContext& ctx) const;

    std::shared_ptr<CostTracker> tracker_;
    std::shared_ptr<MLRouter>    router_;
    mutable std::mutex           cfg_mutex_;  // Lock Layer 1 (config)
    BudgetGuardConfig            cfg_;
};

} // namespace aegisgate
