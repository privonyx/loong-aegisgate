#pragma once

#include <cstdint>
#include <string_view>

namespace aegisgate {

inline uint64_t fnv1a64(std::string_view s) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// Classic 64-bit SimHash over byte trigrams (creative D1=A).
inline uint64_t simhash64(std::string_view text) {
    int32_t acc[64] = {};
    auto add_feature = [&](std::string_view feat) {
        uint64_t fh = fnv1a64(feat);
        for (int i = 0; i < 64; ++i) {
            acc[i] += (fh & (1ull << i)) ? 1 : -1;
        }
    };
    if (text.size() < 3) {
        add_feature(text);
    } else {
        for (size_t i = 0; i + 3 <= text.size(); ++i) {
            add_feature(text.substr(i, 3));
        }
    }
    uint64_t out = 0;
    for (int i = 0; i < 64; ++i) {
        if (acc[i] > 0) {
            out |= (1ull << i);
        }
    }
    return out;
}

inline int hamming64(uint64_t a, uint64_t b) {
    return static_cast<int>(__builtin_popcountll(a ^ b));
}

}  // namespace aegisgate
