#pragma once
#include "gateway/rate_limiter.h"
#include "multimodal/modality.h"
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

namespace aegisgate {

// Phase 6.1 Epic 2.5: per-modality rate-limit quotas.
//
// Sits on top of the existing RateLimiter (token bucket) and exposes
// modality-scoped allowance. Internally each (modality, identity) pair
// maps to a unique RateLimiter key, so the existing sharded LRU bucket
// pool is reused.
//
// Design rationale (CR2 §4.5 + plan task 2.5):
//   - Avoid changing the proven RateLimiter implementation
//   - Make per-modality quotas opt-in (no quota config -> falls back to
//     RateLimiter's global config)
//   - Allow future runtime mutation of quotas via setQuota()
class ModalityRateLimiter {
public:
    explicit ModalityRateLimiter(RateLimiter& backing) : backing_(backing) {}

    // Apply per-modality quota; null config removes the override.
    void setQuota(Modality m, const RateLimiter::Config& cfg);
    void clearQuota(Modality m);

    // identity is typically the API key or tenant id; appended after the
    // modality prefix so different tenants get isolated buckets.
    bool allow(Modality m, const std::string& identity, double cost = 1.0);

    // Inspectors for tests / metric labels.
    bool hasQuota(Modality m) const;
    size_t configuredQuotaCount() const { return quotas_.size(); }

    static std::string makeKey(Modality m, const std::string& identity);

private:
    RateLimiter& backing_;
    std::map<Modality, RateLimiter::Config> quotas_;
    // Tracks which (modality, identity) keys have already been seeded
    // into the backing RateLimiter, so we only call setKeyConfig once
    // per key (it resets the bucket on every call).
    mutable std::mutex mu_;
    mutable std::unordered_set<std::string> seeded_keys_;
};

} // namespace aegisgate
