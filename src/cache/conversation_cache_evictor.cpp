#include "cache/conversation_cache_evictor.h"
#include <algorithm>

namespace aegisgate {

namespace {

double normalize(uint64_t value, uint64_t global_max) {
    if (global_max == 0) return 0.0;
    return static_cast<double>(value) / static_cast<double>(global_max);
}

double normalize(double value, double global_max) {
    if (global_max <= 0.0) return 0.0;
    return std::clamp(value / global_max, 0.0, 1.0);
}

double timeDecay(std::chrono::steady_clock::time_point reference,
                 std::chrono::steady_clock::time_point now,
                 std::chrono::seconds horizon) {
    if (reference.time_since_epoch().count() == 0) return 0.0;
    if (horizon.count() <= 0) return 0.0;
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - reference);
    const double frac = static_cast<double>(elapsed.count()) /
                        static_cast<double>(horizon.count());
    return std::clamp(1.0 - frac, 0.0, 1.0);
}

} // namespace

double ConversationCacheEvictor::score(const ConversationEntryStats& s,
                                       std::chrono::steady_clock::time_point now,
                                       uint64_t global_max_hits,
                                       double global_max_savings) const {
    const double hit_n = normalize(s.hit_count, global_max_hits);
    const double recency = timeDecay(s.last_accessed, now, horizons_.recency);
    const double savings_n = normalize(s.estimated_savings_usd, global_max_savings);
    const double freshness = timeDecay(s.created_at, now, horizons_.freshness);

    return weights_.hit_count * hit_n
         + weights_.recency * recency
         + weights_.savings * savings_n
         + weights_.freshness * freshness;
}

std::vector<std::string> ConversationCacheEvictor::selectEvictees(
    const std::vector<ConversationEntryStats>& entries,
    size_t n,
    std::chrono::steady_clock::time_point now) const {
    if (entries.empty() || n == 0) return {};

    uint64_t max_hits = 0;
    double max_savings = 0.0;
    for (const auto& e : entries) {
        if (e.hit_count > max_hits) max_hits = e.hit_count;
        if (e.estimated_savings_usd > max_savings) max_savings = e.estimated_savings_usd;
    }

    std::vector<std::pair<double, std::string>> scored;
    scored.reserve(entries.size());
    for (const auto& e : entries) {
        scored.emplace_back(score(e, now, max_hits, max_savings), e.partition_key);
    }

    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first < b.first;
                  return a.second < b.second; // tiebreak: lexicographic for determinism
              });

    const size_t pick = std::min(n, scored.size());
    std::vector<std::string> out;
    out.reserve(pick);
    for (size_t i = 0; i < pick; ++i) out.push_back(scored[i].second);
    return out;
}

} // namespace aegisgate
