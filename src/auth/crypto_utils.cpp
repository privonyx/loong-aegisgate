#include "auth/crypto_utils.h"
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <stdexcept>

namespace aegisgate::auth {

namespace {
constexpr char BASE62_CHARS[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
constexpr size_t BASE62_LEN = 62;
constexpr size_t KEY_RANDOM_BYTES = 33;
constexpr const char* KEY_PREFIX = "sk-";
} // namespace

std::string generateApiKey() {
    unsigned char buf[KEY_RANDOM_BYTES];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }

    std::string result(KEY_PREFIX);
    result.reserve(3 + KEY_RANDOM_BYTES);
    for (size_t i = 0; i < KEY_RANDOM_BYTES; ++i) {
        result += BASE62_CHARS[buf[i] % BASE62_LEN];
    }

    OPENSSL_cleanse(buf, sizeof(buf));
    return result;
}

} // namespace aegisgate::auth
