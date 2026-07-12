#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <vector>
#include <sstream>

namespace aegisgate {

struct LabelSet {
    std::vector<std::pair<std::string, std::string>> labels;

    const std::string& key() const;
    std::string prometheus() const;

    mutable std::string cached_key_;
    mutable bool key_cached_ = false;
};

class Counter {
public:
    explicit Counter(const std::string& name, const std::string& help);

    void inc(const LabelSet& labels = {}, double value = 1.0);
    double get(const LabelSet& labels = {}) const;
    // C1 (REV20260702-C1): sum all label buckets whose labels are a superset of
    // `filter`. Empty filter → sum of every bucket (total). Used by alerting so
    // rules on labeled metrics (e.g. requests_total) actually evaluate a value
    // instead of reading the empty-label bucket (which stays 0).
    double getSum(const LabelSet& filter = {}) const;
    std::string expose() const;
    void reset();

    const std::string& name() const { return name_; }

private:
    std::string name_;
    std::string help_;
    mutable std::mutex mutex_; // Lock Layer 3 — see docs/LOCK_ORDERING.md
    std::unordered_map<std::string, double> values_;
};

class Histogram {
public:
    Histogram(const std::string& name, const std::string& help,
              const std::vector<double>& buckets);

    void observe(double value, const LabelSet& labels = {});
    std::string expose() const;
    void reset();

    const std::string& name() const { return name_; }

private:
    struct BucketData {
        std::vector<uint64_t> counts;
        uint64_t total_count = 0;
        double total_sum = 0.0;
    };

    std::string name_;
    std::string help_;
    std::vector<double> buckets_;
    mutable std::mutex mutex_; // Lock Layer 3 — see docs/LOCK_ORDERING.md
    std::unordered_map<std::string, BucketData> data_;
};

class Gauge {
public:
    explicit Gauge(const std::string& name, const std::string& help);

    void set(double value, const LabelSet& labels = {});
    void inc(const LabelSet& labels = {}, double value = 1.0);
    void dec(const LabelSet& labels = {}, double value = 1.0);
    double get(const LabelSet& labels = {}) const;
    // C1: sum all buckets whose labels are a superset of `filter` (empty → all).
    double getSum(const LabelSet& filter = {}) const;
    std::string expose() const;
    void reset();

    const std::string& name() const { return name_; }

private:
    std::string name_;
    std::string help_;
    mutable std::mutex mutex_; // Lock Layer 3 — see docs/LOCK_ORDERING.md
    std::unordered_map<std::string, double> values_;
};

class MetricsRegistry {
public:
    static MetricsRegistry& instance();

    Counter& requestsTotal();
    Counter& tokensTotal();
    Counter& guardrailBlocksTotal();
    Counter& fallbackTotal();
    Counter& rateLimitedTotal();
    // TASK-20260708-02 / REV20260707-C1 — increments each time the
    // assembler falls back from Redis-backed rate limiting to in-memory
    // because Redis is unhealthy at wiring time. Ops can alert on this to
    // detect silent degradation of the "cluster quotas" claim.
    Counter& rateLimiterDegradedTotal();
    // Phase 6.1 Epic 5.1c (B1, TASK-20260515-01) — labels: {"modality", <name>}
    Counter& modalityRateLimitedTotal();
    Counter& cacheHitsTotal();
    Counter& cacheQueriesTotal();
    Histogram& requestDuration();
    Histogram& upstreamDuration();
    Gauge& activeConnections();
    Counter& preprocessorNormalizedTotal();
    Counter& preprocessorEncodingDetectedTotal();
    Gauge& circuitBreakerState();
    Histogram& qualityScore();
    Counter& tokensSavedTotal();

    Counter& ragRetrievalsTotal();
    Counter& agentStepsTotal();
    Counter& anomalyEventsTotal();

    // TASK-20260703-04 (D3=A / SR-6) — cross-tenant cache hit audit counter.
    // Every time a query is answered from another tenant's cached entry (only
    // possible when cross_tenant sharing is explicitly enabled), this MUST be
    // incremented. Metadata only — never labeled with prompt/response plaintext.
    Counter& crossTenantCacheHitsTotal();
    Histogram& groundednessScore();
    Counter& cacheFeedbackTotal();

    // Phase 11.0 — FeedbackBus bridge metric (eagerly initialized).
    // Labels: {"type": <topic>} — e.g. "guard.feedback", "router.outcome"
    Counter& feedbackEventsTotal();

    // Phase 11.5 E3.1 — BudgetGuardStage trigger counter.
    // Labels: {"tenant_id": ...} and {"reason": "tenant_24h" | "request_estimate"}
    Counter& budgetGuardTriggered();

    // TASK-20260609-02 P1-9 — A/B experiment assignment counter.
    // Labels: {"experiment": <name>} and {"variant": <model>}
    Counter& abExperimentAssignedTotal();

    std::string exposeAll() const;
    void resetAll();

private:
    MetricsRegistry();

    Counter requests_total_;
    Counter tokens_total_;
    Counter guardrail_blocks_total_;
    Counter fallback_total_;
    Counter rate_limited_total_;
    Counter rate_limiter_degraded_total_;
    Counter modality_rate_limited_total_;
    Counter cache_hits_total_;
    Counter cache_queries_total_;
    Histogram request_duration_;
    Histogram upstream_duration_;
    Gauge active_connections_;
    Counter preprocessor_normalized_total_;
    Counter preprocessor_encoding_detected_total_;
    Gauge circuit_breaker_state_;
    Histogram quality_score_;
    Counter tokens_saved_total_;

    Counter rag_retrievals_total_;
    Counter agent_steps_total_;
    Counter anomaly_events_total_;
    Counter cross_tenant_cache_hits_total_;
    Histogram groundedness_score_;
    Counter cache_feedback_total_;

    Counter feedback_events_total_;
    Counter budget_guard_triggered_;
    Counter ab_experiment_assigned_total_;
};

} // namespace aegisgate
