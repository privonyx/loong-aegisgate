#include "cache/partitioned_vector_index.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace aegisgate {

PartitionedVectorIndex::PartitionedVectorIndex(
    size_t dim, size_t max_elements, size_t max_partitions)
    : dim_(dim), max_elements_(max_elements),
      max_partitions_(max_partitions) {}

std::string PartitionedVectorIndex::resolveKey(const std::string& key) const {
    if (partitions_.count(key)) return key;
    if (partitions_.size() >= max_partitions_ && !key.empty() && key != kOverflowKey) {
        return kOverflowKey;
    }
    return key;
}

VectorIndex& PartitionedVectorIndex::getOrCreate(const std::string& key) {
    std::string resolved = resolveKey(key);
    auto it = partitions_.find(resolved);
    if (it != partitions_.end()) return *it->second;

    if (partitions_.size() >= max_partitions_ && resolved != kOverflowKey) {
        resolved = kOverflowKey;
        it = partitions_.find(resolved);
        if (it != partitions_.end()) return *it->second;
        spdlog::warn("PartitionedVectorIndex: max partitions ({}) reached, "
                     "routing to overflow", max_partitions_);
    }

    auto idx = std::make_unique<VectorIndex>(dim_, max_elements_);
    auto& ref = *idx;
    partitions_[resolved] = std::move(idx);
    return ref;
}

const VectorIndex* PartitionedVectorIndex::getPartition(
    const std::string& key) const {
    auto resolved = resolveKey(key);
    auto it = partitions_.find(resolved);
    return (it != partitions_.end()) ? it->second.get() : nullptr;
}

VectorIndex* PartitionedVectorIndex::getPartitionMut(const std::string& key) {
    auto resolved = resolveKey(key);
    auto it = partitions_.find(resolved);
    return (it != partitions_.end()) ? it->second.get() : nullptr;
}

bool PartitionedVectorIndex::insert(const std::string& partition_key,
                                     const std::string& id,
                                     const std::vector<float>& vec) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& idx = getOrCreate(partition_key);
    // Release PVI lock, call into VectorIndex (has its own lock)
    // But since VectorIndex::insert acquires its own lock, and we hold
    // PVI mutex only for partition map lookup, we can safely call here.
    // The VectorIndex mutex is on the same layer but different instances.
    return idx.insert(id, vec);
}

bool PartitionedVectorIndex::remove(const std::string& partition_key,
                                     const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* idx = getPartitionMut(partition_key);
    if (!idx) return false;
    return idx->remove(id);
}

std::vector<SearchResult> PartitionedVectorIndex::search(
    const std::string& partition_key,
    const std::vector<float>& query,
    size_t top_k, float threshold) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* idx = getPartition(partition_key);
    if (!idx) return {};
    return idx->search(query, top_k, threshold);
}

std::vector<SearchResult> PartitionedVectorIndex::searchAll(
    const std::vector<float>& query, size_t top_k, float threshold) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SearchResult> merged;
    for (const auto& [key, idx] : partitions_) {
        if (!idx) continue;
        auto part = idx->search(query, top_k, threshold);
        merged.insert(merged.end(), part.begin(), part.end());
    }
    std::sort(merged.begin(), merged.end(),
              [](const SearchResult& a, const SearchResult& b) {
                  return a.similarity > b.similarity;
              });
    if (merged.size() > top_k) merged.resize(top_k);
    return merged;
}

size_t PartitionedVectorIndex::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto& [_, idx] : partitions_) {
        total += idx->size();
    }
    return total;
}

size_t PartitionedVectorIndex::partitionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return partitions_.size();
}

bool PartitionedVectorIndex::contains(const std::string& partition_key,
                                       const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* idx = getPartition(partition_key);
    if (!idx) return false;
    return idx->contains(id);
}

bool PartitionedVectorIndex::enumerate(const EnumerateVisitor& visitor) const {
    if (!visitor) return false;
    // Snapshot partition pointers under lock; iteration runs without holding
    // mutex_ (VectorIndex::forEach takes its own internal lock).
    std::vector<std::pair<std::string, const VectorIndex*>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot.reserve(partitions_.size());
        for (const auto& [key, idx] : partitions_) {
            snapshot.emplace_back(key, idx.get());
        }
    }
    bool stopped = false;
    for (const auto& [key, idx] : snapshot) {
        if (!idx) continue;
        const std::string& pkey = key;
        const bool finished = idx->forEach(
            [&visitor, &pkey, &stopped](const std::string& id,
                                          const std::vector<float>& vec) -> bool {
                if (!visitor(pkey, id, vec)) { stopped = true; return false; }
                return true;
            });
        if (!finished || stopped) return false;
    }
    return true;
}

} // namespace aegisgate
