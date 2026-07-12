#include "cache/embedder.h"
#include <functional>
#include <cmath>

namespace aegisgate {

HashEmbedder::HashEmbedder(size_t dim) : dim_(dim) {}

std::vector<float> HashEmbedder::embed(const std::string& text) {
    std::vector<float> vec(dim_, 0.0f);
    if (text.empty()) return vec;

    // Generate deterministic pseudo-random vector from text hash
    // Uses multiple hash seeds for different dimensions
    std::hash<std::string> hasher;
    for (size_t i = 0; i < dim_; i++) {
        std::string salted = text + "_" + std::to_string(i);
        size_t h = hasher(salted);
        // Map hash to [-1, 1] range
        vec[i] = static_cast<float>(static_cast<int64_t>(h % 20000) - 10000) / 10000.0f;
    }

    // L2-normalize
    float norm = 0.0f;
    for (float v : vec) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 1e-8f) {
        for (float& v : vec) v /= norm;
    }

    return vec;
}

} // namespace aegisgate
