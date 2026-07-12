#include "server/case_study_builder.h"

#include "cache/semantic_cache.h"
#include "observe/cost_tracker.h"
#include "observe/quality_monitor.h"
#include "observe/savings_aggregator.h"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace aegisgate::admin {

namespace {

std::string formatIso(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

} // namespace

nlohmann::json buildCaseStudySnapshot(const CaseStudyInputs& in) {
    nlohmann::json body;

    if (in.include_envelope) {
        body["scope"] = in.is_super ? "global" : "tenant";
        if (!in.is_super) {
            body["tenant_id"] = in.tenant_id;
        }
        body["timestamp"] = formatIso(std::chrono::system_clock::now());

        // aggregator_since: uptime anchor for hero "since" text (D8=B from 28-01).
        // Reuses dashboardSummary / getSavingsSummary's existing
        // SavingsAggregator.snapshot.since semantics; null when not wired.
        body["aggregator_since"] = nullptr;
        if (in.savings_aggregator) {
            auto now = std::chrono::system_clock::now();
            auto from = now - std::chrono::hours(24 * 30);
            const std::string scope_tenant =
                in.is_super ? std::string() : in.tenant_id;
            auto snap = in.savings_aggregator->snapshot(scope_tenant, from, now);
            body["aggregator_since"] = formatIso(snap.since);
        }
    }

    // --- 1. saved_vs_baseline ---
    nlohmann::json svb = {
        {"actual_cost", 0.0},
        {"baseline_cost", 0.0},
        {"cost_saved", 0.0},
        {"savings_percent", 0.0},
    };
    if (in.cost_tracker) {
        CostSummary s = in.is_super
            ? in.cost_tracker->totalSummary()
            : in.cost_tracker->summaryByTenant(in.tenant_id);
        const double actual = s.total_cost;
        const double baseline = s.total_baseline_cost;
        const double saved = std::max(0.0, baseline - actual);
        const double pct = (baseline > 0.0) ? (saved / baseline * 100.0) : 0.0;
        svb["actual_cost"] = actual;
        svb["baseline_cost"] = baseline;
        svb["cost_saved"] = saved;
        svb["savings_percent"] = pct;
    }
    body["saved_vs_baseline"] = svb;

    // --- 2. cache_hit_by_type ---
    // SemanticCache is a process-level singleton today (same as dashboardSummary).
    // Per-tenant cache stats is v2 backlog (TASK-W-PerTenantCache).
    nlohmann::json ch = {
        {"total_hit_rate", 0.0},
        {"hit_exact", 0},
        {"hit_semantic", 0},
        {"hit_conversation", 0},
        {"miss", 0},
    };
    if (in.semantic_cache) {
        auto stats = in.semantic_cache->getStats();
        ch["total_hit_rate"] = static_cast<double>(stats.hit_rate);
        ch["hit_exact"] = stats.hit_exact;
        ch["hit_semantic"] = stats.hit_semantic;
        ch["hit_conversation"] = stats.hit_conversation;
        ch["miss"] = stats.miss_count;
    }
    body["cache_hit_by_type"] = ch;

    // --- 3. quality_reason ---
    // QualityMonitor aggregates by model (process-level, not per-tenant —
    // see SemanticCache note above).
    nlohmann::json q = {
        {"current_ema", 0.0},
        {"slope", 0.0},
        {"reason_factuality", 0},
        {"reason_refusal", 0},
        {"reason_latency_degraded", 0},
    };
    if (in.quality_monitor) {
        size_t r_fact = 0;
        size_t r_ref = 0;
        size_t r_lat = 0;
        double ema = 0.0;
        double slope = 0.0;
        size_t n = 0;
        for (const auto& t : in.quality_monitor->getTrends()) {
            r_fact += t.reason_factuality;
            r_ref += t.reason_refusal;
            r_lat += t.reason_latency_degraded;
            ema += t.current_ema;
            slope += t.slope;
            ++n;
        }
        if (n > 0) {
            ema /= static_cast<double>(n);
            slope /= static_cast<double>(n);
        }
        q["current_ema"] = ema;
        q["slope"] = slope;
        q["reason_factuality"] = r_fact;
        q["reason_refusal"] = r_ref;
        q["reason_latency_degraded"] = r_lat;
    }
    body["quality_reason"] = q;

    return body;
}

} // namespace aegisgate::admin
