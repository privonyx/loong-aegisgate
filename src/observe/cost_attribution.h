#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <chrono>

namespace aegisgate {

struct CostAttributionEntry {
    std::string request_id;
    std::string tenant_id;
    std::string app_id;
    std::string model;
    double cost = 0.0;
    int tokens = 0;
    std::chrono::steady_clock::time_point timestamp;

    CostAttributionEntry() : timestamp(std::chrono::steady_clock::now()) {}
    CostAttributionEntry(std::string rid, std::string tid, std::string aid,
                          std::string m, double c, int t)
        : request_id(std::move(rid)), tenant_id(std::move(tid)),
          app_id(std::move(aid)), model(std::move(m)), cost(c), tokens(t),
          timestamp(std::chrono::steady_clock::now()) {}
};

struct AppCostSummary {
    std::string app_id;
    double total_cost = 0.0;
    int total_tokens = 0;
    int request_count = 0;

    AppCostSummary() = default;
    AppCostSummary(std::string aid, double c, int t, int r)
        : app_id(std::move(aid)), total_cost(c), total_tokens(t), request_count(r) {}
};

class CostAttribution {
public:
    CostAttribution();

    void record(const CostAttributionEntry& entry);

    double getCostByApp(const std::string& app_id,
                         std::chrono::seconds window = std::chrono::seconds(86400)) const;
    double getCostByTenant(const std::string& tenant_id,
                            std::chrono::seconds window = std::chrono::seconds(86400)) const;
    double getCostByModel(const std::string& model,
                           std::chrono::seconds window = std::chrono::seconds(86400)) const;
    std::vector<AppCostSummary> getTopCostApps(
        size_t limit = 10,
        std::chrono::seconds window = std::chrono::seconds(86400)) const;

    size_t size() const;
    void clear();

private:
    void pruneOldEntries();

    std::vector<CostAttributionEntry> entries_;
    mutable std::mutex mutex_;
    size_t max_entries_ = 100000;
    static constexpr std::chrono::hours kMaxRetention{168};
};

} // namespace aegisgate
