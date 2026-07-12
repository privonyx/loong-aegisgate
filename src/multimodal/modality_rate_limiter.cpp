#include "multimodal/modality_rate_limiter.h"

namespace aegisgate {

std::string ModalityRateLimiter::makeKey(Modality m, const std::string& identity) {
    return "modality:" + modalityToString(m) + ":" + identity;
}

void ModalityRateLimiter::setQuota(Modality m, const RateLimiter::Config& cfg) {
    std::lock_guard<std::mutex> lk(mu_);
    quotas_[m] = cfg;
    // Quota update -> reset all previously-seeded buckets so the new
    // size/refill applies. seeded_keys_ are emptied; on next allow() the
    // backing RateLimiter receives a fresh setKeyConfig.
    seeded_keys_.clear();
}

void ModalityRateLimiter::clearQuota(Modality m) {
    std::lock_guard<std::mutex> lk(mu_);
    quotas_.erase(m);
}

bool ModalityRateLimiter::hasQuota(Modality m) const {
    return quotas_.find(m) != quotas_.end();
}

bool ModalityRateLimiter::allow(Modality m, const std::string& identity,
                                 double cost) {
    const auto key = makeKey(m, identity);
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = quotas_.find(m);
        if (it != quotas_.end() && seeded_keys_.insert(key).second) {
            // First time we see this (modality, identity) since the quota was
            // (re)configured. Seed the backing limiter; subsequent allows go
            // straight through without resetting the bucket.
            backing_.setKeyConfig(key, it->second);
        }
    }
    return backing_.allow(key, cost);
}

} // namespace aegisgate
