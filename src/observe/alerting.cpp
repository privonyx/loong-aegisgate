#include "observe/alerting.h"
#include "observe/metrics.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>

namespace aegisgate {

AlertSeverity parseAlertSeverity(const std::string& s) {
    std::string v;
    v.reserve(s.size());
    for (char c : s) v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (v == "info") return AlertSeverity::Info;
    if (v == "critical" || v == "crit") return AlertSeverity::Critical;
    // warning / warn / unknown / empty all fall back to Warning.
    return AlertSeverity::Warning;
}

MetricSelector parseMetricSelector(const std::string& metric) {
    MetricSelector sel;
    auto trim = [](std::string s) {
        auto notspace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
        return s;
    };
    auto strip_quotes = [](std::string s) {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
            return s.substr(1, s.size() - 2);
        }
        return s;
    };

    auto brace = metric.find('{');
    if (brace == std::string::npos) {
        sel.name = trim(metric);
        return sel;
    }
    sel.name = trim(metric.substr(0, brace));
    auto close = metric.rfind('}');
    if (close == std::string::npos || close < brace) {
        return sel;  // malformed selector → name only
    }
    std::string inner = metric.substr(brace + 1, close - brace - 1);
    size_t pos = 0;
    while (pos < inner.size()) {
        auto comma = inner.find(',', pos);
        if (comma == std::string::npos) comma = inner.size();
        auto seg = inner.substr(pos, comma - pos);
        auto eq = seg.find('=');
        if (eq != std::string::npos) {
            auto k = trim(seg.substr(0, eq));
            auto v = strip_quotes(trim(seg.substr(eq + 1)));
            if (!k.empty()) sel.labels.labels.emplace_back(k, v);
        }
        pos = comma + 1;
    }
    return sel;
}

namespace {

// Reads the current value of a registry metric filtered by `labels`. Uses
// getSum so a rule on a labeled metric aggregates all matching buckets (C1)
// instead of reading only the empty-label bucket.
std::optional<double> registryMetricValue(const std::string& metric_name,
                                          const LabelSet& labels) {
    auto& reg = MetricsRegistry::instance();

    struct Mapping {
        const char* full;
        const char* alias;
        std::function<double()> getter;
    };

    const Mapping mappings[] = {
        {"aegisgate_requests_total", "requests_total",
         [&] { return reg.requestsTotal().getSum(labels); }},
        {"aegisgate_tokens_total", "tokens_total",
         [&] { return reg.tokensTotal().getSum(labels); }},
        {"aegisgate_guardrail_blocks_total", "guardrail_blocks_total",
         [&] { return reg.guardrailBlocksTotal().getSum(labels); }},
        {"aegisgate_fallback_total", "fallback_total",
         [&] { return reg.fallbackTotal().getSum(labels); }},
        {"aegisgate_rate_limited_total", "rate_limited_total",
         [&] { return reg.rateLimitedTotal().getSum(labels); }},
        {"aegisgate_cache_hits_total", "cache_hits_total",
         [&] { return reg.cacheHitsTotal().getSum(labels); }},
        {"aegisgate_cache_queries_total", "cache_queries_total",
         [&] { return reg.cacheQueriesTotal().getSum(labels); }},
        {"aegisgate_active_connections", "active_connections",
         [&] { return reg.activeConnections().getSum(labels); }},
        {"aegisgate_preprocessor_normalized_total", "preprocessor_normalized_total",
         [&] { return reg.preprocessorNormalizedTotal().getSum(labels); }},
        {"aegisgate_preprocessor_encoding_detected_total", "preprocessor_encoding_detected_total",
         [&] { return reg.preprocessorEncodingDetectedTotal().getSum(labels); }},
    };

    for (const auto& m : mappings) {
        if (metric_name == m.full || metric_name == m.alias) {
            return m.getter();
        }
    }
    return std::nullopt;
}

} // namespace

AlertManager::AlertManager(const FeatureGate& gate)
    : gate_(gate),
      clock_([] { return std::chrono::steady_clock::now(); }) {}

void AlertManager::setClockForTest(MonoClock clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clock) clock_ = std::move(clock);
}

void AlertManager::addRule(const AlertRule& rule) {
    std::lock_guard<std::mutex> lock(mutex_);
    rules_.push_back(rule);
}

void AlertManager::setChannel(AlertChannel channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    channel_ = std::move(channel);
}

void AlertManager::fireRuleLocked(const AlertRule& rule, double value) {
    if (value < rule.threshold) return;

    // Cooldown: suppress a re-fire if this rule fired within its cooldown
    // window. cooldown_seconds == 0 keeps the fire-every-eval behavior. Keyed by
    // rule.id so distinct rules don't interfere.
    if (rule.cooldown_seconds > 0 && !rule.id.empty()) {
        auto now = clock_();
        auto it = last_fired_.find(rule.id);
        if (it != last_fired_.end() &&
            now - it->second < std::chrono::seconds(rule.cooldown_seconds)) {
            return;
        }
        last_fired_[rule.id] = now;
    }

    Alert alert;
    alert.rule_id = rule.id;
    alert.description = rule.description;
    alert.severity = rule.severity;
    alert.current_value = value;
    alert.threshold = rule.threshold;
    alert.timestamp = currentTimestamp();

    if (fired_alerts_.size() >= max_alerts_) {
        fired_alerts_.erase(fired_alerts_.begin());
    }
    fired_alerts_.push_back(alert);

    if (channel_) {
        channel_(alert);
    }

    spdlog::warn("Alert fired: rule={} metric={} value={} threshold={}",
                 rule.id, rule.metric_name, value, rule.threshold);
}

void AlertManager::checkValue(const std::string& metric_name, double value) {
    if (!gate_.isEnabled(Feature::Alerting)) return;

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& rule : rules_) {
        if (!rule.enabled || rule.metric_name != metric_name) continue;
        fireRuleLocked(rule, value);
    }
}

void AlertManager::checkAndAlert(const std::string& metric_name, double value) {
    checkValue(metric_name, value);
}

void AlertManager::clearAlerts() {
    std::lock_guard<std::mutex> lock(mutex_);
    fired_alerts_.clear();
}

StageResult AlertManager::process(RequestContext& ctx) {
    (void)ctx;
    if (!gate_.isEnabled(Feature::Alerting)) {
        return StageResult::Continue;
    }

    std::vector<AlertRule> rules_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rules_copy = rules_;
    }

    // Evaluate per rule: different rules may share a metric name but carry
    // distinct label selectors, so each needs its own filtered read.
    for (const auto& rule : rules_copy) {
        if (!rule.enabled) continue;
        auto sel = parseMetricSelector(rule.metric_name);
        auto value = registryMetricValue(sel.name, sel.labels);
        if (!value.has_value()) continue;
        std::lock_guard<std::mutex> lock(mutex_);
        fireRuleLocked(rule, *value);
    }

    return StageResult::Continue;
}

StageResult AlertManager::processChunk(RequestContext& ctx, std::string_view chunk) {
    (void)ctx;
    (void)chunk;
    return StageResult::Continue;
}

std::string AlertManager::currentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time_t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

} // namespace aegisgate
