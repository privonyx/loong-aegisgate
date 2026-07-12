#include "cache/hnsw_vector_store.h"

namespace aegisgate {

HnswVectorStore::HnswVectorStore(size_t dim, size_t max_elements,
                                   size_t max_partitions)
    : dim_(dim), max_elements_(max_elements),
      max_partitions_(max_partitions) {}

bool HnswVectorStore::initialize() {
    index_ = std::make_unique<PartitionedVectorIndex>(
        dim_, max_elements_, max_partitions_);
    return true;
}

bool HnswVectorStore::insert(const std::string& partition_key,
                               const std::string& id,
                               const std::vector<float>& vec) {
    return index_->insert(partition_key, id, vec);
}

bool HnswVectorStore::remove(const std::string& partition_key,
                               const std::string& id) {
    return index_->remove(partition_key, id);
}

std::vector<VectorSearchResult> HnswVectorStore::search(
    const std::string& partition_key,
    const std::vector<float>& query,
    size_t top_k,
    float threshold) const {
    auto raw = index_->search(partition_key, query, top_k, threshold);
    std::vector<VectorSearchResult> results;
    results.reserve(raw.size());
    for (const auto& r : raw) {
        results.push_back({r.id, r.similarity});
    }
    return results;
}

std::vector<VectorSearchResult> HnswVectorStore::searchAllPartitions(
    const std::vector<float>& query, size_t top_k, float threshold) const {
    auto raw = index_->searchAll(query, top_k, threshold);
    std::vector<VectorSearchResult> results;
    results.reserve(raw.size());
    for (const auto& r : raw) {
        results.push_back({r.id, r.similarity});
    }
    return results;
}

size_t HnswVectorStore::size() const {
    return index_->size();
}

bool HnswVectorStore::enumerate(EnumerateVisitor visitor) const {
    if (!index_) return false;
    return index_->enumerate(std::move(visitor));
}

} // namespace aegisgate
