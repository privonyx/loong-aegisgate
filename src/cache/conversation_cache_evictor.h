#pragma once
#include <chrono>
#include <string>
#include <vector>

namespace aegisgate {

// Per-conversation rollup stats used by the evictor to score entries
// without coupling to the SemanticCache internals.
struct ConversationEntryStats {
    std::string partition_key;
    uint64_t hit_count = 0;
    std::chrono::steady_clock::time_point last_accessed{};
    double estimated_savings_usd = 0.0;   // cumulative cost saved by this entry
    std::chrono::steady_clock::time_point created_at{};
};

// 4-factor scoring evictor (D3 / Epic 1.6).
// score = 0.4 * normalized(hit_count)
//       + 0.3 * recency      (1.0 if just-accessed, 0.0 at recency_horizon)
//       + 0.2 * normalized(savings)
//       + 0.1 * freshness    (1.0 if just-created, 0.0 at freshness_horizon)
//
// Lower score = better eviction candidate.
class ConversationCacheEvictor {
public:
    struct Weights {
        double hit_count = 0.4;
        double recency = 0.3;
        double savings = 0.2;
        double freshness = 0.1;
    };

    struct Horizons {
        std::chrono::seconds recency = std::chrono::seconds(3600);
        std::chrono::seconds freshness = std::chrono::seconds(86400);
    };

    ConversationCacheEvictor() = default;
    ConversationCacheEvictor(Weights w, Horizons h)
        : weights_(w), horizons_(h) {}

    double score(const ConversationEntryStats& s,
                 std::chrono::steady_clock::time_point now,
                 uint64_t global_max_hits,
                 double global_max_savings) const;

    // Returns at most `n` partition_keys with the lowest score, sorted
    // ascending by score (worst first).
    std::vector<std::string> selectEvictees(
        const std::vector<ConversationEntryStats>& entries,
        size_t n,
        std::chrono::steady_clock::time_point now) const;

    const Weights& weights() const { return weights_; }
    const Horizons& horizons() const { return horizons_; }

private:
    Weights weights_;
    Horizons horizons_;
};

} // namespace aegisgate
