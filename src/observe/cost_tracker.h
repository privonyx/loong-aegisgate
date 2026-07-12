#pragma once
#include "core/pipeline.h"
#include <chrono>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace aegisgate {

class PersistentStore;
class CostAttribution;
class AnomalyDetector;
class CostOptimizer;

struct ModelPricing {
    std::string model;
    double cost_per_1k_input = 0.0;
    double cost_per_1k_output = 0.0;
};

struct CostRecord {
    std::string request_id;
    std::string tenant_id;
    std::string app_id;
    std::string model;
    int input_tokens = 0;
    int output_tokens = 0;
    double input_cost = 0.0;
    double output_cost = 0.0;
    double total_cost = 0.0;
    std::string timestamp;
    // Phase 6.1: modality label for per-modality attribution.
    // Defaults to "chat" so existing (non-multimodal) sites stay
    // back-compatible without code change. (modality strings match
    // multimodal/modality.h: "chat" | "embedding" | "image_gen" | ...)
    std::string modality = "chat";

    // TASK-20260527-02 (MVP-5 case-study) — additive only. SR2 enforces that
    // baseline_cost is computed internally by CostTracker::record() from the
    // pricing table, ignoring any caller-supplied value.
    std::string routing_decision_reason;  // "user_pinned" | "router_economy" | "router_quality" | ""
    double baseline_cost = 0.0;            // hypothetical cost if baseline_model_ was used
};

struct CostSummary {
    double total_cost = 0.0;
    int total_input_tokens = 0;
    int total_output_tokens = 0;
    int request_count = 0;
    // TASK-20260527-02 — saved-vs-baseline aggregation source.
    double total_baseline_cost = 0.0;
};

struct WindowedCost {
    double cost = 0.0;
    int request_count = 0;
    std::chrono::steady_clock::time_point window_start;
};

class CostTracker : public PipelineStage {
public:
    CostTracker();

    void setPricing(const std::string& model, double per_1k_input, double per_1k_output);
    void loadPricing(const std::string& yaml_path);
    void setPersistentStore(PersistentStore* store);

    // P0-5: when set, each tracked request also emits a per-app/tenant/model
    // attribution entry, so CostAttribution's query API is fed from the same
    // single收尾 point as the cost ledger. Borrowed, may be null.
    void setCostAttribution(CostAttribution* attribution) {
        cost_attribution_ = attribution;
    }
    void setAnomalyDetector(AnomalyDetector* detector) {
        anomaly_detector_ = detector;
    }
    void setCostOptimizer(CostOptimizer* optimizer) {
        cost_optimizer_ = optimizer;
    }

    // TASK-20260617-02: 启动回读。从 store 回填 records_，使重启后仪表盘
    // 成本聚合（totalSummary / summaryBy*）恢复近 retention_days 天视图。
    // 仅回填基于 system_clock 的 records_（cost_log_ 窗口滚动基于 steady_clock，
    // 进程重启不可恢复，故不回填）。cap=0 表示用 max_records_；结果超 cap 时
    // 保留最近的 cap 条（按 timestamp 升序取末尾）。
    void loadFromStore(int retention_days, size_t cap = 0);

    // TASK-20260527-02 (MVP-5 case-study) — baseline model selection.
    // The baseline model is the hypothetical "what if we never routed" model
    // used to compute saved_vs_baseline. By default, the first model passed to
    // setPricing() is taken as the baseline; callers may override explicitly.
    // SR2: baseline_cost is *always* recomputed inside record() from pricing,
    //      so external CostRecord{baseline_cost=...} input is silently ignored.
    void setBaselineModel(const std::string& model);
    std::string baselineModel() const;

    CostRecord calculate(const std::string& model, int input_tokens,
                          int output_tokens) const;
    void record(const CostRecord& rec);

    CostSummary summaryByTenant(const std::string& tenant_id) const;
    CostSummary summaryByModel(const std::string& model) const;
    // Phase 6.1: per-modality summary. SR5 RBAC enforcement is the
    // caller's responsibility — e.g. AdminController filters records by
    // `effectiveTenantId` before invoking this method.
    CostSummary summaryByModality(const std::string& modality) const;
    // Returns {modality -> CostSummary} for the supplied tenant filter
    // (empty tenant = aggregate across all tenants; used by SuperAdmin).
    std::unordered_map<std::string, CostSummary> summariesByModality(
        const std::string& tenant_filter = "") const;
    CostSummary totalSummary() const;

    double getCostInWindow(const std::string& model, std::chrono::seconds window) const;
    double getTotalCostInWindow(std::chrono::seconds window) const;

    // Phase 11.5 E3.0 — per-tenant rolling spend over the last `window`.
    // Read by BudgetGuardStage to decide whether the next request must be
    // downgraded to economy tier. Empty tenant_id returns 0.0.
    double getTenantCostInWindow(const std::string& tenant_id,
                                  std::chrono::seconds window) const;

    const std::vector<CostRecord>& records() const { return records_; }
    void clear();

    StageResult process(RequestContext& ctx) override;
    std::string name() const override { return "CostTracker"; }

private:
    struct CostEntry {
        std::string model;
        std::string tenant_id;
        double cost = 0.0;
        std::chrono::steady_clock::time_point timestamp;
    };

    std::string currentTimestamp() const;

    std::unordered_map<std::string, ModelPricing> pricing_;
    std::vector<CostRecord> records_;
    std::vector<CostEntry> cost_log_;
    PersistentStore* store_ = nullptr;
    CostAttribution* cost_attribution_ = nullptr;  // P0-5: borrowed, may be null
    AnomalyDetector* anomaly_detector_ = nullptr;
    CostOptimizer* cost_optimizer_ = nullptr;
    mutable std::mutex mutex_; // Lock Layer 3 — see docs/LOCK_ORDERING.md
    size_t max_records_ = 100000;
    // TASK-20260527-02 — baseline model name; set by first setPricing() call
    // unless overridden via setBaselineModel().
    std::string baseline_model_;
};

} // namespace aegisgate
