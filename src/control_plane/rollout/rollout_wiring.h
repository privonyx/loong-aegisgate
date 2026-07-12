#pragma once

// Phase 9.3.4 Epic C.2 — production wiring helpers for SR16 + SR17.
//
// These are standalone functions suitable for injection into
// RolloutController::Deps. Kept as free functions for testability.

#include "control_plane/rollout/rollout_record.h"
#include "storage/persistent_store.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

namespace aegisgate {

// SR16: returns true if `creator` has fewer than `max_per_24h` rollouts
// started in the last 24 hours.
inline bool rolloutQuotaCheck(PersistentStore* store,
                               const std::string& creator,
                               int max_per_24h,
                               std::int64_t now_ms) {
    if (store == nullptr) return true;
    RolloutQuery q;
    q.limit = 500;
    auto all = store->listRollouts(q);
    const std::int64_t window = 24LL * 3600 * 1000;
    int count = 0;
    for (const auto& r : all) {
        if (r.creator == creator && r.started_at > 0 &&
            (now_ms - r.started_at) < window) {
            ++count;
        }
    }
    return count < max_per_24h;
}

// SR17: returns true if auto-rollback is permitted (env var not set to "1").
inline bool autoRollbackEnabledFromEnv() {
    const char* v = std::getenv("AEGISGATE_DISABLE_AUTO_ROLLBACK");
    return !(v != nullptr && std::strcmp(v, "1") == 0);
}

} // namespace aegisgate
