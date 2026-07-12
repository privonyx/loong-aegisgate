#pragma once
#include "cache/vector_store.h"
#include <string>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace aegisgate {

struct QdrantConfig {
    std::string host = "127.0.0.1";
    int port = 6333;
    std::string collection_prefix = "aegisgate_cache";
    size_t dimension = 128;
    std::string distance = "Cosine";
    std::string api_key;
    int connect_timeout_ms = 5000;
    int request_timeout_ms = 10000;
    bool auto_create_collection = true;
};

class QdrantVectorStore : public VectorStore {
public:
    explicit QdrantVectorStore(const QdrantConfig& config);
    ~QdrantVectorStore() override = default;

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

    std::string backendName() const override { return "qdrant"; }

    // DA-1 / D8=A: remote Qdrant scroll API is not wired into our HTTP
    // connector; enumeration is explicitly unsupported (CacheMigrator dumps
    // the local hnsw index instead).
    bool enumerate(EnumerateVisitor /*visitor*/) const override { return false; }

private:
    std::string baseUrl() const;
    std::string collectionName(const std::string& partition_key) const;
    bool ensureCollection(const std::string& name);
    std::string httpRequest(const std::string& method,
                            const std::string& path,
                            const std::string& body = "") const;

    QdrantConfig config_;
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, bool> known_collections_;
    mutable std::atomic<size_t> approx_size_{0};
};

} // namespace aegisgate
