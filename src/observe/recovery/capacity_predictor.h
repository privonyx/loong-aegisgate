#pragma once

// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 4.
//
// CapacityPredictor — emits HPA-shaped JSON proposals based on linear
// extrapolation of recent QPS samples. v1 deliberately does NOT call any
// Kubernetes API (YAGNI). The output is a structured "kubectl command +
// rationale" recommendation that an operator (or a Runbook with
// approval_required: true) can execute.
//
// SR-NEW3 (HPA proposal limits) is enforced in proposeHpa():
//   * proposed_replicas clamped to [min_replicas, max_replicas]
//   * estimated_cost_increase_usd_24h reported so reviewers can judge
//   * rationale includes the safety margin and target qps_per_replica
//     used so dry-run previews are auditable
//
// All inputs are POCOs; this class has no external dependencies and is
// trivially unit-testable.

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace aegisgate {

struct QpsSample {
    std::int64_t ts_ms = 0;
    double       qps   = 0.0;
};

class CapacityPredictor {
public:
    struct Config {
        int    target_qps_per_replica       = 100;
        double safety_margin                = 0.2;        // +20% headroom
        int    forecast_horizon_seconds     = 30 * 60;
        int    min_replicas                 = 1;
        int    max_replicas                 = 50;
        double max_cost_per_replica_usd_24h = 1.0;
    };

    CapacityPredictor();
    explicit CapacityPredictor(Config cfg);

    // Linear extrapolation: fit a line through (ts_ms, qps) samples and
    // project horizon_s seconds beyond the latest sample. Empty / single
    // sample → 0 (forces "no scaling" recommendation).
    double predictQps(const std::vector<QpsSample>& history,
                       int horizon_s) const;

    // Returns a JSON object matching docs/specs §6.4 HPA proposal schema:
    //   { generated_at_ms, current_qps, predicted_qps_30min,
    //     current_replicas, proposed_replicas, safety_margin,
    //     estimated_cost_increase_usd_24h, suggested_kubectl_command,
    //     rationale }
    nlohmann::json proposeHpa(const std::vector<QpsSample>& history,
                                int current_replicas) const;

    Config config() const { return cfg_; }

private:
    Config cfg_;
};

} // namespace aegisgate
