#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace aegisgate {

struct VectorSearchResult {
    std::string id;
    float score;
};

class VectorStore {
public:
    virtual ~VectorStore() = default;

    virtual bool initialize() = 0;

    // P2-#1: returns true only when the vector is durably present in the
    // backend after the call (success, or already-present). A false return
    // means the caller MUST NOT record a corresponding logical entry, else the
    // cache and the index drift into split-brain (entry served-but-unsearchable
    // / capacity wasted / persisted phantom on reload).
    virtual bool insert(const std::string& partition_key,
                        const std::string& id,
                        const std::vector<float>& vec) = 0;

    virtual bool remove(const std::string& partition_key,
                        const std::string& id) = 0;

    virtual std::vector<VectorSearchResult> search(
        const std::string& partition_key,
        const std::vector<float>& query,
        size_t top_k,
        float threshold = 0.0f) const = 0;

    // TASK-20260703-04 (D3=A / SR-6): search across ALL partitions (crossing
    // tenant boundaries). ONLY used by SemanticCache::getCrossTenant when
    // cross_tenant sharing is explicitly enabled. Default returns {} so backends
    // that cannot support a global scan simply never share across tenants
    // (fail-safe — isolation preserved).
    virtual std::vector<VectorSearchResult> searchAllPartitions(
        const std::vector<float>& /*query*/,
        size_t /*top_k*/,
        float /*threshold*/ = 0.0f) const { return {}; }

    virtual size_t size() const = 0;

    virtual std::string backendName() const = 0;

    // Phase 6.2 (D8=A, DA-1): full-scan enumeration for CacheMigrator dump.
    // Visitor receives (partition_key, id, vector); return false to stop.
    // Default implementation returns false (backend not supported), so
    // remote/networked stores can opt-in only when they expose a scroll API.
    using EnumerateVisitor = std::function<bool(
        const std::string& partition_key,
        const std::string& id,
        const std::vector<float>& vec)>;

    virtual bool enumerate(EnumerateVisitor /*visitor*/) const { return false; }
};

} // namespace aegisgate
