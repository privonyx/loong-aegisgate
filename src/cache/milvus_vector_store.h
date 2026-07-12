#pragma once
#include "cache/vector_store.h"
#include <string>
#include <atomic>
#include <mutex>

namespace aegisgate {

struct MilvusConfig {
    std::string host = "127.0.0.1";
    int port = 19530;
    std::string collection_prefix = "aegisgate_cache";
    size_t dimension = 128;
    std::string metric_type = "IP";
    std::string token;
    int connect_timeout_ms = 5000;
    int request_timeout_ms = 10000;
    bool auto_create_collection = true;
};

class MilvusVectorStore : public VectorStore {
public:
    explicit MilvusVectorStore(const MilvusConfig& config);
    ~MilvusVectorStore() override = default;

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

    size_t size() const override;

    std::string backendName() const override { return "milvus"; }

    // DA-1 / D8=A: remote Milvus has no batch scroll API in our connector;
    // enumeration is explicitly unsupported (CacheMigrator dumps the local
    // hnsw index instead).
    bool enumerate(EnumerateVisitor /*visitor*/) const override { return false; }

private:
    std::string baseUrl() const;
    std::string collectionName(const std::string& partition_key) const;
    bool ensureCollection(const std::string& name);
    std::string httpPost(const std::string& path, const std::string& body) const;
    std::string httpGet(const std::string& path) const;

    MilvusConfig config_;
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, bool> known_collections_;
    mutable std::atomic<size_t> approx_size_{0};
};

} // namespace aegisgate
