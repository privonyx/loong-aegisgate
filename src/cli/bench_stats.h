#pragma once
#include <vector>
#include <algorithm>

namespace aegisgate::cli {

struct BenchStats {
    double p50 = 0;
    double p90 = 0;
    double p99 = 0;
    double rps = 0;
    double total_sec = 0;
    int completed = 0;
    int errors = 0;
};

// Sorts latencies in-place and computes percentiles + RPS.
inline BenchStats computeBenchStats(std::vector<double>& latencies,
                                    int errors, double total_sec) {
    BenchStats stats;
    stats.errors = errors;
    stats.total_sec = total_sec;
    stats.completed = static_cast<int>(latencies.size());

    if (latencies.empty()) return stats;

    std::sort(latencies.begin(), latencies.end());
    int n = static_cast<int>(latencies.size());
    stats.p50 = latencies[n * 50 / 100];
    stats.p90 = latencies[n * 90 / 100];
    stats.p99 = latencies[std::min(n - 1, n * 99 / 100)];
    stats.rps = (total_sec > 0) ? n / total_sec : 0;
    return stats;
}

// Distributes total_requests across concurrency threads.
// Returns a vector of per-thread request counts.
inline std::vector<int> distributeWork(int total_requests, int concurrency) {
    if (concurrency <= 0) return {};
    std::vector<int> counts(concurrency);
    int per_thread = total_requests / concurrency;
    int remainder = total_requests % concurrency;
    for (int t = 0; t < concurrency; ++t) {
        counts[t] = per_thread + (t < remainder ? 1 : 0);
    }
    return counts;
}

} // namespace aegisgate::cli
