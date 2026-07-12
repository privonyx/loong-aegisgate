#pragma once

// Phase 11.2 TASK-20260521-03 — RoutingStrategyCatalog.
//
// Catalogue of 5 hardcoded routing strategy templates (cost-first,
// quality-first, hybrid, canary, shadow) with optional YAML override
// (D5=C decision — mirrors PIIFilter::addDefaultPatterns + loadPatterns
// pattern).
//
// Each strategy bundles:
//   - MultiObjective weights (cost/quality/latency)
//   - Bandit configuration as string fields (algorithm, mode, canary_pct)
//     to keep this header decoupled from BanditRouter (Epic 3 builds the
//     decoder from these strings to BanditRouter::Config).
//
// Threading: shared_mutex (Lock Layer 1) — read-heavy access path.

#include "gateway/multi_objective_router.h"

#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aegisgate {

struct RoutingStrategy {
    std::string name;
    MultiObjectiveRouter::Weights weights;
    std::string bandit_algorithm = "thompson";  // "epsilon_greedy" | "thompson"
    double bandit_epsilon = 0.1;                  // only for ε-greedy
    std::string bandit_mode = "shadow";           // "shadow" | "live"
    double canary_pct = 0.05;
    std::string description;
};

class RoutingStrategyCatalog {
public:
    RoutingStrategyCatalog();  // loads 5 hardcoded defaults

    // YAML schema:
    //   strategies:
    //     - name: ...
    //       weights: { cost: 0.7, quality: 0.2, latency: 0.1 }
    //       bandit_algorithm: epsilon_greedy | thompson
    //       bandit_epsilon: 0.1
    //       bandit_mode: shadow | live
    //       canary_pct: 0.05
    //       description: ...
    //
    // Missing/unparseable file is a soft no-op (defaults remain intact).
    // Existing strategies are overwritten by name; new names are added.
    void loadOverrides(const std::string& yaml_path);

    std::vector<RoutingStrategy> list() const;
    std::optional<RoutingStrategy> get(const std::string& name) const;

private:
    void addDefaultStrategies();
    void insertOrReplace(const RoutingStrategy& s);

    mutable std::shared_mutex mutex_;  // Lock Layer 1
    std::unordered_map<std::string, RoutingStrategy> strategies_;
};

}  // namespace aegisgate
