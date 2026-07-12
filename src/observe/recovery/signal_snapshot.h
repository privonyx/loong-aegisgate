#pragma once

// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 1 task 1.1.
//
// Shared POCOs across recovery/ submodules.
//
// Reused by:
//   * AutoRecoveryController (base class): evaluate input/output
//   * RolloutController (derived): collectMetrics / evaluateBreach
//   * RootCauseAnalyzer: RcaSignals.metrics_now / metrics_baseline
//   * RunbookEngine: trigger evaluation input

#include <cstdint>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace aegisgate {

// SignalSnapshot — point-in-time metrics aggregate. Sample window length is
// implicit (carried by the producer). Custom metrics let derived controllers
// inject domain-specific signals (e.g. cost_per_token) without forcing the
// base class to know about them.
struct SignalSnapshot {
    std::int64_t timestamp_ms = 0;
    double       sample_count = 0;
    double       error_rate   = 0.0;     // 0.0–1.0
    double       p99_latency_ms = 0.0;
    std::unordered_map<std::string, double> custom_metrics;
    nlohmann::json metadata = nlohmann::json::object();  // free-form
};

// BreachVerdict — derived class outputs this from evaluateBreach(). The base
// class uses `breached` to decide whether to call triggerRecovery(); the rest
// of the fields are surfaced via audit + ApprovalProposal.payload.
struct BreachVerdict {
    bool        breached = false;
    std::string reason;   // human-readable e.g. "error_rate > 5%"
    std::string detail;   // machine-readable e.g. "0.07>0.05@stage=canary"
    nlohmann::json evidence = nlohmann::json::object();
};

} // namespace aegisgate
