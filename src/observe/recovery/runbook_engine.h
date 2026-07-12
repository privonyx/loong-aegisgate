#pragma once

// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 5.
//
// RunbookEngine — loads YAML-defined recovery runbooks, evaluates triggers
// against (metrics + feedback events + RCA hypotheses), and builds
// ApprovalProposal objects for AutonomyApprovalWorkflow consumption.
//
// Decoupled from AutonomyApprovalWorkflow to keep this engine unit-
// testable: the Runtime wiring layer (Epic 6) hooks evaluate() →
// buildProposal() → workflow.propose() → (auto-approve when
// approval_required=false) → workflow.apply().
//
// Trigger semantics (creative C2.5 + plan §4.2):
//   - kind: "metric"               — SignalSnapshot field op cmp value
//   - kind: "feedback_event_count" — count(event_type [+ topic_match]) ≥ N
//   - kind: "rca_required"         — exists hypothesis where rule_id == X
//                                      AND score >= rca_min_score
// Multiple triggers are combined with OR (any one fires → runbook matches).
//
// Cooldown gate (M3): after evaluate() returns a runbook id, the runtime
// calls markTriggered(id, now_ms). Subsequent evaluate() calls within
// cooldown_seconds will skip that runbook id.
//
// Lock layering: mu_ is Layer 3 (parallel to RootCauseAnalyzer mu_).

#include "aegisgate/feedback_event.h"
#include "common/clock.h"
#include "observe/autonomy/approval_proposal.h"
#include "observe/recovery/root_cause_analyzer.h"
#include "observe/recovery/signal_snapshot.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace aegisgate {

enum class RunbookTriggerKind {
    Metric,
    FeedbackEventCount,
    RcaRequired,
};

struct RunbookTrigger {
    RunbookTriggerKind kind = RunbookTriggerKind::Metric;
    // metric
    std::string       signal;
    std::string       op;
    double            value           = 0.0;
    int               hold_seconds    = 60;  // unused in v1; reserved for future debounce
    // feedback
    FeedbackEventType event_type      = FeedbackEventType::OpsIncident;
    std::string       topic_match;
    int               window_seconds  = 300;
    int               count_threshold = 1;
    // rca_required
    std::string       rca_rule_id;
    double            rca_min_score   = 0.6;
};

struct RunbookAction {
    std::string    action;          // matches RecoveryApplier action types
    nlohmann::json payload = nlohmann::json::object();
};

struct Runbook {
    std::string                 id;
    std::string                 description;
    std::vector<RunbookTrigger> triggers;
    std::vector<RunbookAction>  actions;
    std::vector<RunbookAction>  rollback_actions;
    bool                        approval_required = true;
    int                         cooldown_seconds  = 300;
};

struct RunbookMatch {
    std::string runbook_id;
    Runbook     runbook;          // deep copy to avoid lifetime hazards
    std::string trigger_summary;  // human-readable why-it-fired
};

class RunbookEngine {
public:
    explicit RunbookEngine(common::Clock* clock);

    // Reload from a directory of *.yaml runbook files. SR-NEW1 path
    // canonicalisation; load failure retains previous runbooks.
    bool reloadRunbooks(const std::string& yaml_dir);

    // Evaluate triggers; returns ids of runbooks whose ANY trigger matches
    // AND that are NOT in cooldown.
    std::vector<RunbookMatch> evaluate(
        const RcaSignals& signals,
        const std::vector<RootCauseHypothesis>& rca_results) const;

    // Build an ApprovalProposal targeting AutonomySource::AutoRecovery.
    // payload.actions is a JSON array of {action, payload} entries that
    // RecoveryApplier dispatches over.
    autonomy::ApprovalProposal buildProposal(const RunbookMatch& m) const;

    // Cooldown bookkeeping (M3).
    void markTriggered(const std::string& runbook_id,
                        std::int64_t steady_now_ms);

    std::size_t runbookCount() const;

private:
    common::Clock*                     clock_;
    mutable std::mutex                 mu_;     // Layer 3
    std::vector<Runbook>               runbooks_;
    std::unordered_map<std::string, std::int64_t> last_triggered_ms_;
};

} // namespace aegisgate
