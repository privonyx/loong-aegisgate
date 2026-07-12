#pragma once
#include "cache/vector_store.h"
#include "cache/partitioned_vector_index.h"
#include <memory>

namespace aegisgate {

class HnswVectorStore : public VectorStore {
public:
    HnswVectorStore(size_t dim, size_t max_elements, size_t max_partitions = 64);

    bool initialize() override;

    bool insert(const std::string& partition_key,
                const std::string& id,
                const std::vector<float>& vec) override;

    bool remove(const std::string& partition_key,
                const std::string& id) override;

    std::vector<VectorSearchResult> search(
        const std::string& partition_key,
        const std::vector<float>& query,
        size_t top_k,
        float threshold = 0.0f) const override;

    std::vector<VectorSearchResult> searchAllPartitions(
        const std::vector<float>& query,
        size_t top_k,
        float threshold = 0.0f) const override;

    size_t size() const override;

    std::string backendName() const override { return "hnswlib"; }

    bool enumerate(EnumerateVisitor visitor) const override;

    PartitionedVectorIndex& index() { return *index_; }

private:
    std::unique_ptr<PartitionedVectorIndex> index_;
    size_t dim_;
    size_t max_elements_;
    size_t max_partitions_;
};

} // namespace aegisgate
