#include "observe/autonomy/approval_queue.h"

#include "storage/persistent_store.h"

#include <spdlog/spdlog.h>
#include <algorithm>

namespace aegisgate::autonomy {

ApprovalQueue::ApprovalQueue(PersistentStore* store) : store_(store) {
    if (!store_) {
        spdlog::warn(
            "ApprovalQueue: no PersistentStore provided, running in-memory only");
    }
}

bool ApprovalQueue::initialize() {
    if (!store_) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
        return true;
    }
    // Pull every record (no filter, big-enough limit). The store guarantees
    // its own concurrency; we only sync into the local cache.
    ApprovalProposalQuery q;
    q.limit  = 1000000;  // §C-3 design budget: 1M records / startup
    q.offset = 0;
    auto records = store_->listApprovalProposals(q);

    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    cache_.reserve(records.size());
    for (const auto& rec : records) {
        auto p = fromRecord(rec);
        if (p) cache_[rec.id] = std::move(*p);
    }
    spdlog::info(
        "ApprovalQueue: restored {} proposals from PersistentStore",
        cache_.size());
    return true;
}

std::string ApprovalQueue::insert(const ApprovalProposal& p) {
    if (p.id.empty()) return std::string();

    if (store_) {
        // I/O OUTSIDE Layer-3 lock.
        if (!store_->insertApprovalProposal(toRecord(p))) {
            return std::string();
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Belt-and-braces guard against double insert when memory-only.
        if (cache_.find(p.id) != cache_.end() && !store_) {
            return std::string();
        }
        cache_[p.id] = p;
    }
    return p.id;
}

bool ApprovalQueue::update(const ApprovalProposal& p) {
    if (p.id.empty()) return false;

    if (store_) {
        if (!store_->updateApprovalProposal(toRecord(p))) {
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(p.id);
        if (it == cache_.end()) {
            // store accepted but cache lost the entry — repopulate so
            // subsequent get() / list() stay consistent.
            cache_[p.id] = p;
        } else {
            it->second = p;
        }
    }
    return true;
}

std::optional<ApprovalProposal> ApprovalQueue::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(id);
    if (it == cache_.end()) return std::nullopt;
    return it->second;
}

std::vector<ApprovalProposal>
ApprovalQueue::list(const ApprovalQueueQuery& q) const {
    std::vector<ApprovalProposal> matched;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        matched.reserve(cache_.size());
        for (const auto& kv : cache_) {
            const auto& p = kv.second;
            if (q.state_filter && p.state != *q.state_filter)   continue;
            if (q.source_filter && p.source != *q.source_filter) continue;
            matched.push_back(p);
        }
    }
    std::sort(matched.begin(), matched.end(),
        [](const ApprovalProposal& a, const ApprovalProposal& b) {
            if (a.proposed_at_ms != b.proposed_at_ms)
                return a.proposed_at_ms > b.proposed_at_ms;
            return a.id > b.id;
        });
    int offset = std::max(0, q.offset);
    int limit  = q.limit > 0 ? q.limit : static_cast<int>(matched.size());
    if (offset >= static_cast<int>(matched.size())) return {};
    auto first = matched.begin() + offset;
    auto last  = (offset + limit >= static_cast<int>(matched.size()))
                   ? matched.end()
                   : first + limit;
    return std::vector<ApprovalProposal>(first, last);
}

std::int64_t ApprovalQueue::count(const ApprovalQueueQuery& q) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::int64_t n = 0;
    for (const auto& kv : cache_) {
        const auto& p = kv.second;
        if (q.state_filter && p.state != *q.state_filter)   continue;
        if (q.source_filter && p.source != *q.source_filter) continue;
        ++n;
    }
    return n;
}

std::int64_t ApprovalQueue::prune(int retention_days) {
    if (retention_days <= 0) return 0;

    std::int64_t pruned = 0;
    if (store_) {
        pruned = store_->pruneApprovalProposals(retention_days);
    }
    // Re-sync cache from store so eviction tracks store reality. Cheaper
    // than maintaining a parallel cutoff timestamp in code.
    if (store_) {
        ApprovalProposalQuery q;
        q.limit  = 1000000;
        auto records = store_->listApprovalProposals(q);
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
        for (const auto& rec : records) {
            auto p = fromRecord(rec);
            if (p) cache_[rec.id] = std::move(*p);
        }
    } else {
        // memory-only: best-effort eviction using cache's own timestamps.
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const std::int64_t cutoff_ms = now_ms -
            static_cast<std::int64_t>(retention_days) * 86400LL * 1000LL;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = cache_.begin(); it != cache_.end(); ) {
            if (it->second.proposed_at_ms < cutoff_ms) {
                it = cache_.erase(it);
                ++pruned;
            } else {
                ++it;
            }
        }
    }
    return pruned;
}

std::size_t ApprovalQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

} // namespace aegisgate::autonomy
