#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace aegisgate {

class CostTracker;
class SemanticCache;
class QualityMonitor;
class SavingsAggregator;

namespace admin {

// TASK-20260602-01 Epic 2 — Shared case-study snapshot builder (spec §2 D2).
//
// Consumed by:
//   AdminController::caseStudyHeadline()              (HTTP /admin/api/case-study/headline)
//   AdminWsController::caseStudyTimer / per-connection (WS 30s push)
//
// Inputs are non-owning pointers; any null source is treated as zero data so
// the response JSON always has a well-formed 5-key schema (frontend never
// sees missing fields).  Schema (hard-coded keys — see SR4 audit):
//
//   {
//     [include_envelope ? scope / tenant_id / timestamp / aggregator_since :]
//     saved_vs_baseline:  { actual_cost, baseline_cost, cost_saved, savings_percent },
//     cache_hit_by_type:  { total_hit_rate, hit_exact, hit_semantic, hit_conversation, miss },
//     quality_reason:     { current_ema, slope, reason_factuality, reason_refusal, reason_latency_degraded }
//   }
struct CaseStudyInputs {
    CostTracker* cost_tracker = nullptr;
    SemanticCache* semantic_cache = nullptr;
    QualityMonitor* quality_monitor = nullptr;
    SavingsAggregator* savings_aggregator = nullptr;
    // SuperAdmin → totalSummary / global cache.  Otherwise restricted to tenant_id.
    bool is_super = true;
    std::string tenant_id;
    // HTTP endpoint sets true (returns scope/tenant_id/timestamp/aggregator_since
    // alongside data blocks).  WS payload sets false (envelope is added by caller
    // under a `data` field with discriminator `type`).
    bool include_envelope = false;
};

nlohmann::json buildCaseStudySnapshot(const CaseStudyInputs& in);

} // namespace admin
} // namespace aegisgate
