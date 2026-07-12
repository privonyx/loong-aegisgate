#pragma once
#include <hnswlib/hnswlib.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <functional>

namespace aegisgate {

struct SearchResult {
    std::string id;
    float distance;
    float similarity;
};

class VectorIndex {
public:
    VectorIndex(size_t dim, size_t max_elements, size_t M = 16, size_t ef_construction = 200);

    // P2-#1: true when the id is present in the index after the call (inserted
    // now, or already present). false on dimension mismatch / capacity
    // exhaustion so the caller can refuse to register a phantom logical entry.
    bool insert(const std::string& id, const std::vector<float>& vec);
    bool remove(const std::string& id);
    std::vector<SearchResult> search(const std::vector<float>& query,
                                      size_t top_k,
                                      float threshold = 0.0f) const;

    size_t size() const;
    size_t dimension() const { return dim_; }
    bool contains(const std::string& id) const;

    void save(const std::string& path) const;
    void load(const std::string& path);

    // Phase 6.2: enumerate all (id, vector) pairs currently in the index.
    // Visitor returns false to stop. Throws nothing; failed label lookups
    // (e.g., concurrently deleted) are silently skipped.
    using ForEachVisitor = std::function<bool(const std::string& id,
                                              const std::vector<float>& vec)>;
    bool forEach(const ForEachVisitor& visitor) const;

private:
    size_t dim_;
    size_t max_elements_;
    std::unique_ptr<hnswlib::InnerProductSpace> space_;
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> index_;
    std::unordered_map<std::string, hnswlib::labeltype> id_to_label_;
    std::unordered_map<hnswlib::labeltype, std::string> label_to_id_;
    hnswlib::labeltype next_label_ = 0;
    mutable std::mutex mutex_; // Lock Layer 3 — see docs/LOCK_ORDERING.md
};

} // namespace aegisgate
