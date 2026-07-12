#include "observe/metrics.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace aegisgate {

namespace {

std::string escapePrometheusLabelValue(const std::string& v) {
    std::string out;
    out.reserve(v.size() + 8);
    for (unsigned char c : v) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out += static_cast<char>(c);
                break;
        }
    }
    return out;
}

void appendEscapedLabelsFromKey(std::ostringstream& oss, const std::string& key) {
    size_t pos = 0;
    bool first = true;
    while (pos < key.size()) {
        const auto eq = key.find('=', pos);
        if (eq == std::string::npos) break;
        auto comma = key.find(',', eq);
        if (comma == std::string::npos) comma = key.size();
        if (!first) oss << ",";
        first = false;
        oss << key.substr(pos, eq - pos) << "=\""
            << escapePrometheusLabelValue(key.substr(eq + 1, comma - eq - 1)) << "\"";
        pos = comma + 1;
    }
}

} // namespace

// --- LabelSet ---

const std::string& LabelSet::key() const {
    if (key_cached_) return cached_key_;
    cached_key_.clear();
    size_t estimated = 0;
    for (const auto& [name, value] : labels) {
        estimated += name.size() + 1 + value.size() + 1;
    }
    cached_key_.reserve(estimated);
    for (const auto& [name, value] : labels) {
        if (!cached_key_.empty()) cached_key_ += ',';
        cached_key_ += name;
        cached_key_ += '=';
        cached_key_ += value;
    }
    key_cached_ = true;
    return cached_key_;
}

std::string LabelSet::prometheus() const {
    if (labels.empty()) return "";
    std::string s = "{";
    bool first = true;
    for (const auto& [name, value] : labels) {
        if (!first) s += ",";
        s += name + "=\"" + escapePrometheusLabelValue(value) + "\"";
        first = false;
    }
    s += "}";
    return s;
}

// --- Counter ---

Counter::Counter(const std::string& name, const std::string& help)
    : name_(name), help_(help) {}

void Counter::inc(const LabelSet& labels, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    values_[labels.key()] += value;
}

double Counter::get(const LabelSet& labels) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = values_.find(labels.key());
    return (it != values_.end()) ? it->second : 0.0;
}

namespace {
// Parse a bucket key ("k1=v1,k2=v2") back into label pairs. Mirrors
// LabelSet::key() formatting (comma-separated, '=' delimited, unescaped) — same
// limitation applies: label values containing ',' or '=' are not disambiguated.
std::vector<std::pair<std::string, std::string>> parseKeyLabels(
    const std::string& key) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t pos = 0;
    while (pos < key.size()) {
        auto comma = key.find(',', pos);
        if (comma == std::string::npos) comma = key.size();
        auto seg = key.substr(pos, comma - pos);
        auto eq = seg.find('=');
        if (eq != std::string::npos) {
            out.emplace_back(seg.substr(0, eq), seg.substr(eq + 1));
        }
        pos = comma + 1;
    }
    return out;
}

// A bucket matches when its labels are a superset of the filter's labels.
bool keyMatchesFilter(const std::string& key, const LabelSet& filter) {
    if (filter.labels.empty()) return true;
    auto bucket = parseKeyLabels(key);
    for (const auto& want : filter.labels) {
        bool found = false;
        for (const auto& have : bucket) {
            if (have.first == want.first && have.second == want.second) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}
}  // namespace

double Counter::getSum(const LabelSet& filter) const {
    std::lock_guard<std::mutex> lock(mutex_);
    double sum = 0.0;
    for (const auto& [key, val] : values_) {
        if (keyMatchesFilter(key, filter)) sum += val;
    }
    return sum;
}

std::string Counter::expose() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    oss << "# HELP " << name_ << " " << help_ << "\n";
    oss << "# TYPE " << name_ << " counter\n";
    if (values_.empty()) {
        oss << name_ << " 0\n";
    } else {
        for (const auto& [key, val] : values_) {
            oss << name_;
            if (!key.empty()) {
                oss << "{";
                appendEscapedLabelsFromKey(oss, key);
                oss << "}";
            }
            oss << " " << val << "\n";
        }
    }
    return oss.str();
}

void Counter::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.clear();
}

// --- Histogram ---

Histogram::Histogram(const std::string& name, const std::string& help,
                     const std::vector<double>& buckets)
    : name_(name), help_(help), buckets_(buckets) {
    std::sort(buckets_.begin(), buckets_.end());
}

void Histogram::observe(double value, const LabelSet& labels) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& bd = data_[labels.key()];
    if (bd.counts.empty()) {
        bd.counts.resize(buckets_.size() + 1, 0);
    }
    for (size_t i = 0; i < buckets_.size(); i++) {
        if (value <= buckets_[i]) {
            bd.counts[i]++;
        }
    }
    bd.counts.back()++;  // +Inf
    bd.total_count++;
    bd.total_sum += value;
}

std::string Histogram::expose() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    oss << "# HELP " << name_ << " " << help_ << "\n";
    oss << "# TYPE " << name_ << " histogram\n";

    for (const auto& [key, bd] : data_) {
        for (size_t i = 0; i < buckets_.size(); i++) {
            oss << name_ << "_bucket{";
            appendEscapedLabelsFromKey(oss, key);
            if (!key.empty()) oss << ",";
            oss << "le=\"" << buckets_[i] << "\"} " << bd.counts[i] << "\n";
        }
        oss << name_ << "_bucket{";
        appendEscapedLabelsFromKey(oss, key);
        if (!key.empty()) oss << ",";
        oss << "le=\"+Inf\"} " << bd.counts.back() << "\n";
        oss << name_ << "_sum{";
        appendEscapedLabelsFromKey(oss, key);
        oss << "} " << bd.total_sum << "\n";
        oss << name_ << "_count{";
        appendEscapedLabelsFromKey(oss, key);
        oss << "} " << bd.total_count << "\n";
    }
    return oss.str();
}

void Histogram::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.clear();
}

// --- Gauge ---

Gauge::Gauge(const std::string& name, const std::string& help)
    : name_(name), help_(help) {}

void Gauge::set(double value, const LabelSet& labels) {
    std::lock_guard<std::mutex> lock(mutex_);
    values_[labels.key()] = value;
}

void Gauge::inc(const LabelSet& labels, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    values_[labels.key()] += value;
}

void Gauge::dec(const LabelSet& labels, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    values_[labels.key()] -= value;
}

double Gauge::get(const LabelSet& labels) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = values_.find(labels.key());
    return (it != values_.end()) ? it->second : 0.0;
}

double Gauge::getSum(const LabelSet& filter) const {
    std::lock_guard<std::mutex> lock(mutex_);
    double sum = 0.0;
    for (const auto& [key, val] : values_) {
        if (keyMatchesFilter(key, filter)) sum += val;
    }
    return sum;
}

std::string Gauge::expose() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    oss << "# HELP " << name_ << " " << help_ << "\n";
    oss << "# TYPE " << name_ << " gauge\n";
    if (values_.empty()) {
        oss << name_ << " 0\n";
    } else {
        for (const auto& [key, val] : values_) {
            oss << name_;
            if (!key.empty()) {
                oss << "{";
                appendEscapedLabelsFromKey(oss, key);
                oss << "}";
            }
            oss << " " << val << "\n";
        }
    }
    return oss.str();
}

void Gauge::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.clear();
}

// --- MetricsRegistry ---

MetricsRegistry& MetricsRegistry::instance() {
    static MetricsRegistry reg;
    return reg;
}

MetricsRegistry::MetricsRegistry()
    : requests_total_("aegisgate_requests_total", "Total number of requests"),
      tokens_total_("aegisgate_tokens_total", "Total tokens consumed"),
      guardrail_blocks_total_("aegisgate_guardrail_blocks_total", "Guardrail block count"),
      fallback_total_("aegisgate_fallback_total", "Fallback events"),
      rate_limited_total_("aegisgate_rate_limited_total", "Rate limited requests"),
      rate_limiter_degraded_total_(
          "aegisgate_rate_limiter_degraded_total",
          "Rate limiter fallbacks from Redis to in-memory (Redis unhealthy at wiring)"),
      modality_rate_limited_total_("aegisgate_modality_rate_limited_total",
                                    "Per-modality rate-limited requests (label: modality)"),
      cache_hits_total_("aegisgate_cache_hits_total", "Cache hits"),
      cache_queries_total_("aegisgate_cache_queries_total", "Cache queries"),
      request_duration_("aegisgate_request_duration_seconds",
                        "Request duration in seconds",
                        {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0}),
      upstream_duration_("aegisgate_upstream_duration_seconds",
                         "Upstream model call duration in seconds",
                         {0.01, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0, 30.0}),
      active_connections_("aegisgate_active_connections", "Active connections"),
      preprocessor_normalized_total_("aegisgate_preprocessor_normalized_total",
                                      "Total inputs that underwent Unicode normalization"),
      preprocessor_encoding_detected_total_("aegisgate_preprocessor_encoding_detected_total",
                                             "Total encoding segments detected"),
      circuit_breaker_state_("aegisgate_circuit_breaker_state",
                              "Circuit breaker state per model (0=Closed, 1=Open, 2=HalfOpen)"),
      quality_score_("aegisgate_quality_score",
                      "Output quality score distribution",
                      {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0}),
      tokens_saved_total_("aegisgate_tokens_saved_total",
                           "Total tokens saved by optimization methods"),
      rag_retrievals_total_("aegisgate_rag_retrievals_total",
                             "Total RAG retrieval operations"),
      agent_steps_total_("aegisgate_agent_steps_total",
                          "Total agent orchestration steps executed"),
      anomaly_events_total_("aegisgate_anomaly_events_total",
                             "Total anomaly events detected"),
      cross_tenant_cache_hits_total_(
          "aegisgate_cross_tenant_cache_hits_total",
          "Total cross-tenant cache hits (D3=A / SR-6 audit)"),
      groundedness_score_("aegisgate_groundedness_score",
                           "RAG groundedness score distribution",
                           {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0}),
      cache_feedback_total_("aegisgate_cache_feedback_total",
                             "Total cache quality feedback received"),
      feedback_events_total_("feedback_events_total",
                              "FeedbackBus events by topic (Phase 11.0)"),
      budget_guard_triggered_("aegisgate_budget_guard_triggered_total",
                               "BudgetGuardStage downgrades by tenant + reason"
                               " (Phase 11.5)"),
      ab_experiment_assigned_total_("aegisgate_ab_experiment_assigned_total",
                                     "A/B experiment assignments by experiment"
                                     " + variant (P1-9)") {}

Counter& MetricsRegistry::requestsTotal() { return requests_total_; }
Counter& MetricsRegistry::tokensTotal() { return tokens_total_; }
Counter& MetricsRegistry::guardrailBlocksTotal() { return guardrail_blocks_total_; }
Counter& MetricsRegistry::fallbackTotal() { return fallback_total_; }
Counter& MetricsRegistry::rateLimitedTotal() { return rate_limited_total_; }
Counter& MetricsRegistry::rateLimiterDegradedTotal() { return rate_limiter_degraded_total_; }
Counter& MetricsRegistry::modalityRateLimitedTotal() { return modality_rate_limited_total_; }
Counter& MetricsRegistry::cacheHitsTotal() { return cache_hits_total_; }
Counter& MetricsRegistry::cacheQueriesTotal() { return cache_queries_total_; }
Histogram& MetricsRegistry::requestDuration() { return request_duration_; }
Histogram& MetricsRegistry::upstreamDuration() { return upstream_duration_; }
Gauge& MetricsRegistry::activeConnections() { return active_connections_; }
Counter& MetricsRegistry::preprocessorNormalizedTotal() { return preprocessor_normalized_total_; }
Counter& MetricsRegistry::preprocessorEncodingDetectedTotal() { return preprocessor_encoding_detected_total_; }
Gauge& MetricsRegistry::circuitBreakerState() { return circuit_breaker_state_; }
Histogram& MetricsRegistry::qualityScore() { return quality_score_; }
Counter& MetricsRegistry::tokensSavedTotal() { return tokens_saved_total_; }
Counter& MetricsRegistry::ragRetrievalsTotal() { return rag_retrievals_total_; }
Counter& MetricsRegistry::agentStepsTotal() { return agent_steps_total_; }
Counter& MetricsRegistry::anomalyEventsTotal() { return anomaly_events_total_; }
Counter& MetricsRegistry::crossTenantCacheHitsTotal() { return cross_tenant_cache_hits_total_; }
Histogram& MetricsRegistry::groundednessScore() { return groundedness_score_; }
Counter& MetricsRegistry::cacheFeedbackTotal() { return cache_feedback_total_; }
Counter& MetricsRegistry::feedbackEventsTotal() { return feedback_events_total_; }
Counter& MetricsRegistry::budgetGuardTriggered() { return budget_guard_triggered_; }
Counter& MetricsRegistry::abExperimentAssignedTotal() { return ab_experiment_assigned_total_; }

std::string MetricsRegistry::exposeAll() const {
    std::string out;
    out += requests_total_.expose();
    out += tokens_total_.expose();
    out += guardrail_blocks_total_.expose();
    out += fallback_total_.expose();
    out += rate_limited_total_.expose();
    out += rate_limiter_degraded_total_.expose();
    out += modality_rate_limited_total_.expose();
    out += cache_hits_total_.expose();
    out += cache_queries_total_.expose();
    out += request_duration_.expose();
    out += upstream_duration_.expose();
    out += active_connections_.expose();
    out += preprocessor_normalized_total_.expose();
    out += preprocessor_encoding_detected_total_.expose();
    out += circuit_breaker_state_.expose();
    out += quality_score_.expose();
    out += tokens_saved_total_.expose();
    out += rag_retrievals_total_.expose();
    out += agent_steps_total_.expose();
    out += anomaly_events_total_.expose();
    out += cross_tenant_cache_hits_total_.expose();
    out += groundedness_score_.expose();
    out += cache_feedback_total_.expose();
    out += feedback_events_total_.expose();
    out += budget_guard_triggered_.expose();
    out += ab_experiment_assigned_total_.expose();
    return out;
}

void MetricsRegistry::resetAll() {
    requests_total_.reset();
    tokens_total_.reset();
    guardrail_blocks_total_.reset();
    fallback_total_.reset();
    rate_limited_total_.reset();
    rate_limiter_degraded_total_.reset();
    modality_rate_limited_total_.reset();
    cache_hits_total_.reset();
    cache_queries_total_.reset();
    request_duration_.reset();
    upstream_duration_.reset();
    active_connections_.reset();
    preprocessor_normalized_total_.reset();
    preprocessor_encoding_detected_total_.reset();
    circuit_breaker_state_.reset();
    quality_score_.reset();
    tokens_saved_total_.reset();
    rag_retrievals_total_.reset();
    agent_steps_total_.reset();
    anomaly_events_total_.reset();
    cross_tenant_cache_hits_total_.reset();
    groundedness_score_.reset();
    cache_feedback_total_.reset();
    feedback_events_total_.reset();
    budget_guard_triggered_.reset();
    ab_experiment_assigned_total_.reset();
}

} // namespace aegisgate
