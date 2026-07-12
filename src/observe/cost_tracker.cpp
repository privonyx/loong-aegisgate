#include "observe/cost_tracker.h"
#include "observe/anomaly_detector.h"
#include "observe/cost_attribution.h"
#include "observe/cost_optimizer.h"
#include "storage/persistent_store.h"
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace aegisgate {

CostTracker::CostTracker() = default;

void CostTracker::setPricing(const std::string& model,
                              double per_1k_input, double per_1k_output) {
    std::lock_guard<std::mutex> lock(mutex_);
    pricing_[model] = {model, per_1k_input, per_1k_output};
    // TASK-20260527-02: first registered model becomes the default baseline,
    // unless the caller has already set one explicitly.
    if (baseline_model_.empty()) {
        baseline_model_ = model;
    }
}

void CostTracker::setBaselineModel(const std::string& model) {
    std::lock_guard<std::mutex> lock(mutex_);
    baseline_model_ = model;
}

std::string CostTracker::baselineModel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return baseline_model_;
}

void CostTracker::loadPricing(const std::string& yaml_path) {
    try {
        auto root = YAML::LoadFile(yaml_path);
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto providers = root["providers"]) {
            for (const auto& provider : providers) {
                if (auto models = provider["models"]) {
                    for (const auto& model : models) {
                        auto name = model["id"].as<std::string>();
                        auto input = model["cost_per_1k_input"].as<double>(0.0);
                        auto output = model["cost_per_1k_output"].as<double>(0.0);
                        pricing_[name] = {name, input, output};
                    }
                }
            }
        }
        // P1-5: inject a baseline model on the production loadPricing() path.
        // Previously only setPricing() seeded baseline_model_, so the runtime
        // (which calls loadPricing) left it empty and baseline_cost was always
        // 0. When no explicit baseline was set, default to the most expensive
        // model by (input+output) per-1k cost — the "what if we never routed
        // down" reference — with a name tie-break for determinism.
        if (baseline_model_.empty() && !pricing_.empty()) {
            const ModelPricing* best = nullptr;
            for (const auto& [name, mp] : pricing_) {
                double total = mp.cost_per_1k_input + mp.cost_per_1k_output;
                if (!best) { best = &mp; continue; }
                double best_total = best->cost_per_1k_input +
                                    best->cost_per_1k_output;
                if (total > best_total ||
                    (total == best_total && mp.model < best->model)) {
                    best = &mp;
                }
            }
            if (best && (best->cost_per_1k_input + best->cost_per_1k_output) > 0.0) {
                baseline_model_ = best->model;
                spdlog::info("CostTracker baseline model set to '{}' (P1-5)",
                             baseline_model_);
            }
        }
        spdlog::info("Loaded pricing for {} models", pricing_.size());
    } catch (const YAML::Exception& e) {
        spdlog::error("Failed to load pricing: {}", e.what());
    }
}

CostRecord CostTracker::calculate(const std::string& model,
                                    int input_tokens, int output_tokens) const {
    CostRecord rec;
    rec.model = model;
    rec.input_tokens = input_tokens;
    rec.output_tokens = output_tokens;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pricing_.find(model);
    if (it != pricing_.end()) {
        rec.input_cost = (input_tokens / 1000.0) * it->second.cost_per_1k_input;
        rec.output_cost = (output_tokens / 1000.0) * it->second.cost_per_1k_output;
    }
    rec.total_cost = rec.input_cost + rec.output_cost;
    // TASK-20260527-02: prefill baseline_cost (will be recomputed by record(),
    // SR2 — external callers cannot bypass this even if they construct
    // CostRecord directly; record() always re-derives baseline_cost).
    if (!baseline_model_.empty()) {
        auto bit = pricing_.find(baseline_model_);
        if (bit != pricing_.end()) {
            rec.baseline_cost =
                (input_tokens / 1000.0) * bit->second.cost_per_1k_input +
                (output_tokens / 1000.0) * bit->second.cost_per_1k_output;
        }
    }
    return rec;
}

void CostTracker::setPersistentStore(PersistentStore* store) {
    std::lock_guard<std::mutex> lock(mutex_);
    store_ = store;
}

void CostTracker::loadFromStore(int retention_days, size_t cap) {
    PersistentStore* store_snapshot = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        store_snapshot = store_;
    }
    if (!store_snapshot) return;

    std::string from_iso;
    if (retention_days > 0) {
        auto from = std::chrono::system_clock::now() -
                    std::chrono::hours(24 * retention_days);
        auto tt = std::chrono::system_clock::to_time_t(from);
        std::tm tm{};
        gmtime_r(&tt, &tm);
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        from_iso = oss.str();
    }
    // Upper bound must be non-empty: SQLite query uses `timestamp <= ?` and an
    // empty bound would exclude everything (memory backend treats it as open).
    auto rows = store_snapshot->queryCostsByDateRange(
        /*tenant_id=*/"", from_iso, /*to=*/"9999-12-31T23:59:59Z");

    std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();
    size_t effective_cap = (cap == 0) ? max_records_ : std::min(cap, max_records_);
    // rows are ordered ascending by timestamp → keep the most recent cap rows.
    size_t start = rows.size() > effective_cap ? rows.size() - effective_cap : 0;
    for (size_t i = start; i < rows.size(); ++i) {
        records_.push_back(std::move(rows[i]));
    }
    spdlog::info("CostTracker loadFromStore: replayed {} cost records (window={}d, cap={})",
                 records_.size(), retention_days, effective_cap);
}

void CostTracker::record(const CostRecord& rec_in) {
    // TASK-20260527-02 (SR2): make a local copy so we can override the
    // baseline_cost field with the internally-computed value, ignoring
    // whatever the caller put in. This guarantees that no external code
    // path — including malicious construction — can inject a fabricated
    // saved_vs_baseline number that would mislead Case Study Numbers.
    CostRecord rec = rec_in;
    PersistentStore* store_snapshot = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Recompute baseline_cost from pricing table (SR2 reverse anchor).
        rec.baseline_cost = 0.0;
        if (!baseline_model_.empty()) {
            auto bit = pricing_.find(baseline_model_);
            if (bit != pricing_.end()) {
                rec.baseline_cost =
                    (rec.input_tokens / 1000.0) * bit->second.cost_per_1k_input +
                    (rec.output_tokens / 1000.0) * bit->second.cost_per_1k_output;
            }
        }
        if (records_.size() >= max_records_) {
            records_.erase(records_.begin());
        }
        if (cost_log_.size() >= max_records_) {
            cost_log_.erase(cost_log_.begin());
        }
        records_.push_back(rec);
        cost_log_.push_back(CostEntry{rec.model, rec.tenant_id, rec.total_cost,
                                       std::chrono::steady_clock::now()});
        store_snapshot = store_;
    }
    if (store_snapshot) {
        if (!store_snapshot->insertCostRecord(rec)) {
            spdlog::warn("Cost persist failed: req={}", rec.request_id);
        }
    }
}

CostSummary CostTracker::summaryByTenant(const std::string& tenant_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    CostSummary s;
    for (const auto& r : records_) {
        if (r.tenant_id == tenant_id) {
            s.total_cost += r.total_cost;
            s.total_input_tokens += r.input_tokens;
            s.total_output_tokens += r.output_tokens;
            s.total_baseline_cost += r.baseline_cost;
            s.request_count++;
        }
    }
    return s;
}

CostSummary CostTracker::summaryByModel(const std::string& model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    CostSummary s;
    for (const auto& r : records_) {
        if (r.model == model) {
            s.total_cost += r.total_cost;
            s.total_input_tokens += r.input_tokens;
            s.total_output_tokens += r.output_tokens;
            s.total_baseline_cost += r.baseline_cost;
            s.request_count++;
        }
    }
    return s;
}

CostSummary CostTracker::summaryByModality(const std::string& modality) const {
    std::lock_guard<std::mutex> lock(mutex_);
    CostSummary s;
    for (const auto& r : records_) {
        if (r.modality == modality) {
            s.total_cost += r.total_cost;
            s.total_input_tokens += r.input_tokens;
            s.total_output_tokens += r.output_tokens;
            s.total_baseline_cost += r.baseline_cost;
            ++s.request_count;
        }
    }
    return s;
}

std::unordered_map<std::string, CostSummary>
CostTracker::summariesByModality(const std::string& tenant_filter) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<std::string, CostSummary> out;
    for (const auto& r : records_) {
        // SR5 RBAC: when tenant_filter is non-empty (Admin role), only the
        // matching tenant's records contribute. SuperAdmin passes "" to
        // aggregate everything.
        if (!tenant_filter.empty() && r.tenant_id != tenant_filter) continue;
        auto& s = out[r.modality];
        s.total_cost += r.total_cost;
        s.total_input_tokens += r.input_tokens;
        s.total_output_tokens += r.output_tokens;
        s.total_baseline_cost += r.baseline_cost;
        ++s.request_count;
    }
    return out;
}

CostSummary CostTracker::totalSummary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    CostSummary s;
    for (const auto& r : records_) {
        s.total_cost += r.total_cost;
        s.total_input_tokens += r.input_tokens;
        s.total_output_tokens += r.output_tokens;
        s.total_baseline_cost += r.baseline_cost;
        s.request_count++;
    }
    return s;
}

void CostTracker::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();
    cost_log_.clear();
}

double CostTracker::getCostInWindow(const std::string& model,
                                     std::chrono::seconds window) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto cutoff = std::chrono::steady_clock::now() - window;
    double sum = 0.0;
    for (const auto& e : cost_log_) {
        if (e.model == model && e.timestamp >= cutoff) {
            sum += e.cost;
        }
    }
    return sum;
}

double CostTracker::getTotalCostInWindow(std::chrono::seconds window) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto cutoff = std::chrono::steady_clock::now() - window;
    double sum = 0.0;
    for (const auto& e : cost_log_) {
        if (e.timestamp >= cutoff) {
            sum += e.cost;
        }
    }
    return sum;
}

double CostTracker::getTenantCostInWindow(const std::string& tenant_id,
                                          std::chrono::seconds window) const {
    if (tenant_id.empty()) return 0.0;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto cutoff = std::chrono::steady_clock::now() - window;
    double sum = 0.0;
    for (const auto& e : cost_log_) {
        if (e.tenant_id == tenant_id && e.timestamp >= cutoff) {
            sum += e.cost;
        }
    }
    return sum;
}

StageResult CostTracker::process(RequestContext& ctx) {
    auto rec = calculate(
        ctx.target_model.empty() ? ctx.chat_request.model : ctx.target_model,
        ctx.token_usage.prompt_tokens,
        ctx.token_usage.completion_tokens);

    rec.request_id = ctx.request_id;
    rec.tenant_id = ctx.tenant_id;
    rec.app_id = ctx.app_id;
    rec.timestamp = currentTimestamp();
    // P1-7: carry the router's decision rationale into the cost ledger so the
    // ledger explains why each request was billed against a given model.
    rec.routing_decision_reason = ctx.routing_decision_reason;

    record(rec);

    // P0-5: feed per-app/tenant/model attribution from the same收尾 point.
    if (cost_attribution_) {
        cost_attribution_->record(CostAttributionEntry(
            rec.request_id, rec.tenant_id, rec.app_id, rec.model,
            rec.total_cost, rec.input_tokens + rec.output_tokens));
    }
    if (cost_optimizer_) {
        const double quality = ctx.quality_score >= 0.0 ? ctx.quality_score : 0.0;
        cost_optimizer_->recordUsage(rec.model, rec.total_cost, quality);
    }
    if (anomaly_detector_) {
        anomaly_detector_->recordMetric(AnomalyType::CostSpike, rec.total_cost);
    }

    spdlog::debug("Cost tracked: req={} model={} cost={:.6f}",
                  ctx.request_id, rec.model, rec.total_cost);
    return StageResult::Continue;
}

std::string CostTracker::currentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time_t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

} // namespace aegisgate
