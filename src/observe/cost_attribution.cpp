#include "observe/cost_attribution.h"
#include <algorithm>

namespace aegisgate {

CostAttribution::CostAttribution() = default;

void CostAttribution::record(const CostAttributionEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(entry);
    if (entries_.size() > max_entries_) {
        pruneOldEntries();
        if (entries_.size() > max_entries_) {
            entries_.erase(entries_.begin(),
                           entries_.begin() + static_cast<long>(entries_.size() - max_entries_));
        }
    }
}

double CostAttribution::getCostByApp(const std::string& app_id,
                                      std::chrono::seconds window) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto cutoff = std::chrono::steady_clock::now() - window;
    double total = 0.0;
    for (const auto& e : entries_) {
        if (e.app_id == app_id && e.timestamp >= cutoff) {
            total += e.cost;
        }
    }
    return total;
}

double CostAttribution::getCostByTenant(const std::string& tenant_id,
                                         std::chrono::seconds window) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto cutoff = std::chrono::steady_clock::now() - window;
    double total = 0.0;
    for (const auto& e : entries_) {
        if (e.tenant_id == tenant_id && e.timestamp >= cutoff) {
            total += e.cost;
        }
    }
    return total;
}

double CostAttribution::getCostByModel(const std::string& model,
                                        std::chrono::seconds window) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto cutoff = std::chrono::steady_clock::now() - window;
    double total = 0.0;
    for (const auto& e : entries_) {
        if (e.model == model && e.timestamp >= cutoff) {
            total += e.cost;
        }
    }
    return total;
}

std::vector<AppCostSummary> CostAttribution::getTopCostApps(
    size_t limit, std::chrono::seconds window) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto cutoff = std::chrono::steady_clock::now() - window;

    struct Accumulator {
        double total_cost = 0.0;
        int total_tokens = 0;
        int request_count = 0;
    };
    std::unordered_map<std::string, Accumulator> agg;

    for (const auto& e : entries_) {
        if (e.timestamp >= cutoff) {
            auto& a = agg[e.app_id];
            a.total_cost += e.cost;
            a.total_tokens += e.tokens;
            a.request_count++;
        }
    }

    std::vector<AppCostSummary> result;
    result.reserve(agg.size());
    for (const auto& [aid, acc] : agg) {
        result.emplace_back(aid, acc.total_cost, acc.total_tokens, acc.request_count);
    }

    std::sort(result.begin(), result.end(),
              [](const AppCostSummary& a, const AppCostSummary& b) {
                  return a.total_cost > b.total_cost;
              });

    if (result.size() > limit) {
        result.resize(limit);
    }
    return result;
}

size_t CostAttribution::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void CostAttribution::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

void CostAttribution::pruneOldEntries() {
    const auto cutoff = std::chrono::steady_clock::now() - kMaxRetention;
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                        [&cutoff](const CostAttributionEntry& e) {
                            return e.timestamp < cutoff;
                        }),
        entries_.end());
}

} // namespace aegisgate
