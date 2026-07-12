#pragma once
#include <string>
#include <vector>
#include <cmath>
#include <numeric>
#include <functional>
#include <unordered_map>
#include <mutex>

namespace aegisgate {

class Embedder {
public:
    virtual ~Embedder() = default;
    virtual std::vector<float> embed(const std::string& text) = 0;
    virtual size_t dimension() const = 0;
};

// Hash-based embedder for testing/fallback (deterministic, no model needed)
class HashEmbedder : public Embedder {
public:
    explicit HashEmbedder(size_t dim = 128);

    std::vector<float> embed(const std::string& text) override;
    size_t dimension() const override { return dim_; }

private:
    size_t dim_;
};

inline float cosineSimilarity(const std::vector<float>& a,
                               const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (size_t i = 0; i < a.size(); i++) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    float denom = std::sqrt(na) * std::sqrt(nb);
    return (denom > 1e-8f) ? (dot / denom) : 0.0f;
}

} // namespace aegisgate
