#pragma once
#include "core/crypto.h"
#include <string>

namespace aegisgate::auth {

std::string generateApiKey();

inline std::string extractKeyPrefix(const std::string& key) {
    constexpr size_t PREFIX_LEN = 8;
    if (key.size() < PREFIX_LEN) return key;
    return key.substr(0, PREFIX_LEN);
}

inline std::string hashApiKey(const std::string& key) {
    return crypto::sha256(key);
}

inline bool verifyApiKey(const std::string& key_hash, const std::string& candidate_hash) {
    return crypto::constantTimeEquals(key_hash, candidate_hash);
}

} // namespace aegisgate::auth
