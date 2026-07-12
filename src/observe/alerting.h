#pragma once
#include "core/pipeline.h"
#include "core/feature_gate.h"
#include "observe/metrics.h"
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <chrono>
#include <unordered_map>

namespace aegisgate {

enum class AlertSeverity { Info, Warning, Critical };

// Maps a config severity string to the enum. info→Info, warning/warn→Warning,
// critical/crit→Critical; unknown/empty→Warning. Case-insensitive.
AlertSeverity parseAlertSeverity(const std::string& s);

// C1 (REV20260702-C1): a rule's metric string may carry a label selector, e.g.
// `requests_total{status="rejected"}`. Parses it into the base metric name and a
// LabelSet filter. No braces → name only, empty filter. Tolerates optional
// quotes and surrounding whitespace; malformed input degrades to name-only.
struct MetricSelector {
    std::string name;
    LabelSet labels;
};
MetricSelector parseMetricSelector(const std::string& metric);

struct AlertRule {
    std::string id;
    std::string description;
    AlertSeverity severity = AlertSeverity::Warning;
    std::string metric_name;
    double threshold = 0.0;
    bool enabled = true;
    // Minimum seconds between two fires of this rule. 0 = fire on every
    // evaluation (backward compatible). Guards against webhook storms when a
    // counter stays above threshold across many requests.
    int cooldown_seconds = 0;
};

struct Alert {
    std::string rule_id;
    std::string description;
    AlertSeverity severity;
    double current_value;
    double threshold;
    std::string timestamp;
};

using AlertChannel = std::function<void(const Alert&)>;

class AlertManager : public PipelineStage {
public:
    explicit AlertManager(const FeatureGate& gate);

    void addRule(const AlertRule& rule);
    void setChannel(AlertChannel channel);

    void checkValue(const std::string& metric_name, double value);
    void checkAndAlert(const std::string& metric_name, double value);

    const std::vector<Alert>& firedAlerts() const { return fired_alerts_; }
    size_t ruleCount() const { return rules_.size(); }
    void clearAlerts();

    // Injects a monotonic clock for deterministic cooldown testing. Defaults
    // to std::chrono::steady_clock::now.
    using MonoClock = std::function<std::chrono::steady_clock::time_point()>;
    void setClockForTest(MonoClock clock);

    StageResult process(RequestContext& ctx) override;
    StageResult processChunk(RequestContext& ctx,
                             std::string_view chunk) override;
    std::string name() const override { return "alerting"; }

private:
    std::string currentTimestamp() const;
    // Evaluates threshold + cooldown for a single rule and fires if exceeded.
    // Caller must hold mutex_.
    void fireRuleLocked(const AlertRule& rule, double value);

    const FeatureGate& gate_;
    std::vector<AlertRule> rules_;
    AlertChannel channel_;
    std::vector<Alert> fired_alerts_;
    mutable std::mutex mutex_; // Lock Layer 3 — see docs/LOCK_ORDERING.md
    size_t max_alerts_ = 10000;
    // Per-rule last-fire timestamps for cooldown enforcement (keyed by rule.id).
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_fired_;
    MonoClock clock_;
};

} // namespace aegisgate
