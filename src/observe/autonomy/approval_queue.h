#pragma once

// Phase 11.5 AutonomyApprovalWorkflow (TASK-20260518-02) — Epic 1.4.
//
// ApprovalQueue — in-memory cache of every live ApprovalProposal backed
// by a PersistentStore. The cache acts as the hot read path (Admin UI
// list / workflow lookup); the store is the source of truth that survives
// process restart.
//
// Concurrency model (Lock Layer 3, see docs/LOCK_ORDERING.md):
//   - mutex_ protects cache_ only.
//   - PersistentStore I/O is performed OUTSIDE mutex_ (CostTracker pattern)
//     to honour the "no Layer 3 → Layer 2 nesting" rule. Each public
//     mutator follows the order: store call (lock-free) → success branch
//     re-acquires mutex_ to update cache. Failures don't touch cache.
//   - initialize() takes the lock once, fully populates cache_ from the
//     store, releases — there is no concurrent write path at startup.
//
// store_ may be nullptr; the queue then runs in pure memory mode (used
// by tests + the C5 "no PersistentStore configured" fallback) and emits
// a single warn() on construction.

#include "observe/autonomy/approval_proposal.h"
#include "observe/autonomy/approval_state.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aegisgate {
class PersistentStore;  // forward — header avoids pulling storage details.
}

namespace aegisgate::autonomy {

// Query parameters mirror ApprovalProposalQuery but use the runtime
// enum types so callers don't have to stringify.
struct ApprovalQueueQuery {
    std::optional<ApprovalState>   state_filter;
    std::optional<AutonomySource>  source_filter;
    int                            limit  = 100;
    int                            offset = 0;
};

class ApprovalQueue {
public:
    // store may be nullptr → memory-only mode.
    explicit ApprovalQueue(PersistentStore* store);

    // Load every existing proposal from the store into cache_. Idempotent.
    // Returns true on success (including the no-store case).
    bool initialize();

    // Insert a fully-populated proposal. Returns p.id on success or empty
    // string on failure (e.g. store rejected duplicate id / empty id).
    // The caller is responsible for minting p.id (ULID); ApprovalQueue
    // never generates ids itself.
    std::string insert(const ApprovalProposal& p);

    // Update an existing proposal (state transitions etc.). Returns true
    // when both store + cache succeeded.
    bool update(const ApprovalProposal& p);

    // Fetch by id. Returns std::nullopt when missing.
    std::optional<ApprovalProposal> get(const std::string& id) const;

    // Filter + sort + page. Sorted by proposed_at_ms DESC, id DESC.
    std::vector<ApprovalProposal> list(const ApprovalQueueQuery& q) const;

    // Total entries matching q.state_filter / q.source_filter, ignoring
    // q.limit / q.offset — the denominator for paginated list views.
    std::int64_t count(const ApprovalQueueQuery& q) const;

    // Prune store-side records older than `retention_days`. Also drops
    // matching cache entries. Returns number of records pruned from
    // storage (cache eviction is best-effort).
    std::int64_t prune(int retention_days);

    // Current cache size — for tests / metrics.
    std::size_t size() const;

private:
    PersistentStore* store_;  // not owned; may be nullptr
    mutable std::mutex mutex_;  // Lock Layer 3 — see docs/LOCK_ORDERING.md
    std::unordered_map<std::string, ApprovalProposal> cache_;
};

} // namespace aegisgate::autonomy
