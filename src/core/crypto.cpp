#include "crypto.h"
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <iomanip>
#include <sstream>

namespace aegisgate::crypto {

namespace {
std::string evpSha256(const unsigned char* data, size_t len) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";

    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) &&
              EVP_DigestUpdate(ctx, data, len) &&
              EVP_DigestFinal_ex(ctx, hash, &hash_len);

    EVP_MD_CTX_free(ctx);

    if (!ok) return "";

    std::ostringstream oss;
    for (unsigned int i = 0; i < hash_len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(hash[i]);
    }

    OPENSSL_cleanse(hash, sizeof(hash));
    return oss.str();
}
} // namespace

std::string sha256(const std::string& input) {
    return evpSha256(reinterpret_cast<const unsigned char*>(input.data()),
                     input.size());
}

std::string sha256WithSalt(const std::string& input, const std::string& salt) {
    std::string salted = salt + input;
    auto result = evpSha256(
        reinterpret_cast<const unsigned char*>(salted.data()), salted.size());
    OPENSSL_cleanse(salted.data(), salted.size());
    return result;
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        volatile unsigned char sink = 0;
        size_t len = std::min(a.size(), b.size());
        for (size_t i = 0; i < len; ++i) {
            sink |= static_cast<unsigned char>(a[i]) ^
                    static_cast<unsigned char>(b[i]);
        }
        (void)sink;
        return false;
    }
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace aegisgate::crypto
