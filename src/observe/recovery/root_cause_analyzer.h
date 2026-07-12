#pragma once

// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 3.
//
// RootCauseAnalyzer (RCA v1) — multi-signal rule engine that fuses
// metrics + FeedbackBus events + spdlog ringbuffer log lines into a list
// of structured RootCauseHypothesis objects. Closed creative decisions:
//
//   C2.1 (rule schema)            B  — multi-condition conditions[]
//   C2.2 (scoring)                A  — weighted-sum + 0.3 threshold
//   C2.3 (evidence struct)        B  — 4-field with PII mask defence-in-depth
//   C2.4 (signal fusion)          B  — OR + min_matched_conditions
//   C2.5 (Runbook integration)    B  — exposes findHypothesis() for
//                                       RunbookEngine::evaluateRcaRequired
//
// Lock layering: shared_mutex mu_ (Layer 3) for rules_ + last_loaded_ms_.
// PIIFilter::mask() is invoked WHILE mu_ is read-locked; PIIFilter has its
// own Layer 1 patterns_mutex_ so no inversion risk.
//
// SR-NEW1 (YAML integrity) is enforced in reloadRules():
//   * canonical-path check rejects "../" traversal
//   * basename allowlist rejects unexpected file names
//   * sha256 of the loaded body is logged at INFO so an operator can
//     reconcile against ./scripts/audit-rca-rules.sh

#include "observe/recovery/log_ringbuffer_sink.h"
#include "observe/recovery/signal_snapshot.h"
#include "aegisgate/feedback_event.h"

#include <re2/re2.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace aegisgate {

class PIIFilter;

// --- C2.3 evidence struct --------------------------------------------------
struct RcaEvidence {
    std::string source;          // "metric:p99_latency_ms" / "feedback:OpsIncident" / "log_pattern"
    std::string label;           // human-readable, post-PII-mask
    std::string current_value;   // actual numeric / count, post-PII-mask
    std::string expected_value;  // threshold / baseline, post-PII-mask
};

// --- C2.1 condition variant ------------------------------------------------
enum class ConditionKind {
    MetricThreshold,
    FeedbackEventCount,
    LogPatternCount,
};

struct MetricThresholdCondition {
    std::string signal;          // p99_latency_ms / error_rate / custom
    std::string op;              // ">" / ">=" / "<" / "<=" / "=="
    double      value = 0.0;
    std::string evidence_label_tpl;
};

struct FeedbackEventCountCondition {
    FeedbackEventType event_type = FeedbackEventType::OpsIncident;
    std::string       topic_match;     // empty = any topic
    int               window_seconds  = 300;
    int               count_threshold = 1;
    std::string       evidence_label_tpl;
};

struct LogPatternCountCondition {
    std::shared_ptr<RE2> pattern;     // compiled re2 (shared so YAML ↔ struct cheap)
    std::string          pattern_text; // original source for evidence + audit
    int                  window_seconds  = 300;
    int                  count_threshold = 1;
    std::string          evidence_label_tpl;
};

struct Condition {
    ConditionKind kind;
    std::variant<MetricThresholdCondition,
                 FeedbackEventCountCondition,
                 LogPatternCountCondition> impl;
};

// --- Rule struct -----------------------------------------------------------
struct Rule {
    std::string                id;
    std::string                category;
    std::string                summary;
    std::vector<Condition>     conditions;
    std::size_t                min_matched_conditions = 1;
    double                     score_per_condition    = 0.3;
    double                     score_when_all_matched = 0.9;
    std::vector<std::string>   suggested_actions;     // runbook names
};

// --- Hypothesis output -----------------------------------------------------
struct RootCauseHypothesis {
    std::string              rule_id;
    double                   score = 0.0;
    std::string              category;
    std::string              summary;
    std::vector<RcaEvidence> evidence;
    std::vector<std::string> suggested_actions;
};

// --- RcaSignals (input to analyze) -----------------------------------------
struct RcaSignals {
    SignalSnapshot                       metrics_now;
    SignalSnapshot                       metrics_baseline;
    std::vector<FeedbackEvent>           recent_feedback_events;
    std::vector<LogRingbufferSink::Entry> recent_log_entries;
};

// Global threshold below which a hypothesis is not surfaced (creative C2.2).
inline constexpr double kRcaOutputScoreThreshold = 0.3;

// Pure scoring function — exposed for unit tests and mutation testing.
//   matched_count < rule.min_matched_conditions → 0.0
//   else min(rule.score_per_condition * matched_count,
//            rule.score_when_all_matched)
double scoreRule(const Rule& rule, std::size_t matched_count);

// --- Analyzer --------------------------------------------------------------
class RootCauseAnalyzer {
public:
    explicit RootCauseAnalyzer(std::shared_ptr<PIIFilter> pii_filter);

    // SR-NEW1: yaml_path is canonical-checked; failure to load retains the
    // previous rule set (PIIFilter::reloadPatterns parity).
    bool reloadRules(const std::string& yaml_path);

    // Hypothesis list, sorted by descending score, filtered to score >=
    // kRcaOutputScoreThreshold.
    std::vector<RootCauseHypothesis> analyze(const RcaSignals& signals) const;

    // C2.5 helper used by RunbookEngine::evaluateRcaRequired.
    std::optional<RootCauseHypothesis> findHypothesis(
        const std::vector<RootCauseHypothesis>& results,
        const std::string& rule_id) const;

    // (rule_count, last_loaded_ms_wallclock).
    std::pair<std::size_t, std::int64_t> health() const;

private:
    std::shared_ptr<PIIFilter>  pii_filter_;
    mutable std::shared_mutex   mu_;            // Layer 3
    std::vector<Rule>           rules_;
    std::int64_t                last_loaded_ms_ = 0;
};

} // namespace aegisgate
