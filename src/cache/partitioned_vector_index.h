#pragma once
#include "cache/vector_index.h"
#include <functional>
#include <unordered_map>
#include <mutex>
#include <memory>

namespace aegisgate {

class PartitionedVectorIndex {
public:
    PartitionedVectorIndex(size_t dim, size_t max_elements,
                           size_t max_partitions = 64);

    bool insert(const std::string& partition_key,
                const std::string& id, const std::vector<float>& vec);
    bool remove(const std::string& partition_key, const std::string& id);
    std::vector<SearchResult> search(const std::string& partition_key,
                                      const std::vector<float>& query,
                                      size_t top_k,
                                      float threshold = 0.0f) const;

    // TASK-20260703-04 (D3=A): search across ALL partitions, merging the
    // top matches globally. This deliberately crosses tenant boundaries and is
    // ONLY reachable from SemanticCache::getCrossTenant when cross_tenant
    // sharing is explicitly enabled (fail-safe off by default). Partition-scoped
    // search() above remains the isolation-preserving default path.
    std::vector<SearchResult> searchAll(const std::vector<float>& query,
                                        size_t top_k,
                                        float threshold = 0.0f) const;
    size_t size() const;
    size_t partitionCount() const;
    bool contains(const std::string& partition_key,
                  const std::string& id) const;

    // Phase 6.2: iterate every (partition_key, id, vector) tuple.
    // Returns false if the visitor stopped early or no partitions are
    // populated; true otherwise.
    using EnumerateVisitor = std::function<bool(
        const std::string& partition_key,
        const std::string& id,
        const std::vector<float>& vec)>;
    bool enumerate(const EnumerateVisitor& visitor) const;

private:
    static constexpr const char* kOverflowKey = "_overflow";

    VectorIndex& getOrCreate(const std::string& key);
    const VectorIndex* getPartition(const std::string& key) const;
    VectorIndex* getPartitionMut(const std::string& key);
    std::string resolveKey(const std::string& key) const;

    size_t dim_;
    size_t max_elements_;
    size_t max_partitions_;
    mutable std::unordered_map<std::string,
        std::unique_ptr<VectorIndex>> partitions_;
    mutable std::mutex mutex_; // Lock Layer 3
};

} // namespace aegisgate
