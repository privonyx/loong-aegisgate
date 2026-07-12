#include "auth/totp_service.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace aegisgate {

namespace {
constexpr char BASE32_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
constexpr int TOTP_PERIOD = 30;
constexpr int TOTP_DIGITS = 6;
constexpr int SECRET_BYTES = 20;
// TASK-20260702-02 P2-2（SR-2）：10 字节 = 80bit 熵（此前 4 字节=32bit 可爆破）。
// 编码为 20 位 hex；存量恢复码以哈希存储，改长度不影响旧码校验。
constexpr int RECOVERY_CODE_BYTES = 10;
} // namespace

std::string TotpService::base32Encode(const std::vector<uint8_t>& data) {
    std::string result;
    int buffer = 0;
    int bits_left = 0;

    for (uint8_t byte : data) {
        buffer = (buffer << 8) | byte;
        bits_left += 8;
        while (bits_left >= 5) {
            bits_left -= 5;
            result += BASE32_ALPHABET[(buffer >> bits_left) & 0x1F];
        }
    }

    if (bits_left > 0) {
        result += BASE32_ALPHABET[(buffer << (5 - bits_left)) & 0x1F];
    }

    return result;
}

std::vector<uint8_t> TotpService::base32Decode(const std::string& encoded) {
    std::vector<uint8_t> result;
    int buffer = 0;
    int bits_left = 0;

    for (char c : encoded) {
        char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        int val;
        if (upper >= 'A' && upper <= 'Z') {
            val = upper - 'A';
        } else if (upper >= '2' && upper <= '7') {
            val = upper - '2' + 26;
        } else {
            continue; // skip padding or invalid
        }

        buffer = (buffer << 5) | val;
        bits_left += 5;

        if (bits_left >= 8) {
            bits_left -= 8;
            result.push_back(static_cast<uint8_t>((buffer >> bits_left) & 0xFF));
        }
    }

    return result;
}

std::string TotpService::generateCode(const std::vector<uint8_t>& secret, int64_t counter) {
    uint8_t counter_bytes[8];
    for (int i = 7; i >= 0; --i) {
        counter_bytes[i] = static_cast<uint8_t>(counter & 0xFF);
        counter >>= 8;
    }

    unsigned char hmac_result[EVP_MAX_MD_SIZE];
    unsigned int hmac_len = 0;
    unsigned char* rc = HMAC(EVP_sha1(), secret.data(),
                             static_cast<int>(secret.size()),
                             counter_bytes, sizeof(counter_bytes),
                             hmac_result, &hmac_len);
    if (!rc || hmac_len == 0) {
        throw std::runtime_error("HMAC-SHA1 failed");
    }

    int offset = hmac_result[hmac_len - 1] & 0x0F;
    uint32_t truncated =
        (static_cast<uint32_t>(hmac_result[offset] & 0x7F) << 24) |
        (static_cast<uint32_t>(hmac_result[offset + 1]) << 16) |
        (static_cast<uint32_t>(hmac_result[offset + 2]) << 8) |
        static_cast<uint32_t>(hmac_result[offset + 3]);

    uint32_t otp = truncated % 1000000;

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(TOTP_DIGITS) << otp;
    return oss.str();
}

std::string TotpService::generateSecret() {
    std::vector<uint8_t> secret(SECRET_BYTES);
    if (RAND_bytes(secret.data(), SECRET_BYTES) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return base32Encode(secret);
}

std::string TotpService::generateOtpAuthUri(const std::string& secret_base32,
                                             const std::string& user,
                                             const std::string& issuer) {
    return "otpauth://totp/" + issuer + ":" + user +
           "?secret=" + secret_base32 +
           "&issuer=" + issuer +
           "&algorithm=SHA1"
           "&digits=6"
           "&period=30";
}

bool TotpService::verifyCode(const std::string& secret_base32, const std::string& code,
                              int64_t timestamp) {
    if (timestamp == 0) {
        timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    auto secret = base32Decode(secret_base32);
    int64_t counter = timestamp / TOTP_PERIOD;

    for (int64_t i = -1; i <= 1; ++i) {
        if (generateCode(secret, counter + i) == code) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> TotpService::generateRecoveryCodes(int count) {
    std::vector<std::string> codes;
    codes.reserve(count);

    for (int i = 0; i < count; ++i) {
        unsigned char buf[RECOVERY_CODE_BYTES];
        if (RAND_bytes(buf, sizeof(buf)) != 1) {
            throw std::runtime_error("RAND_bytes failed");
        }
        std::ostringstream oss;
        for (int j = 0; j < RECOVERY_CODE_BYTES; ++j) {
            oss << std::uppercase << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(buf[j]);
        }
        codes.push_back(oss.str());
    }
    return codes;
}

std::string TotpService::hashRecoveryCode(const std::string& code) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");

    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
              EVP_DigestUpdate(ctx, code.data(), code.size()) == 1 &&
              EVP_DigestFinal_ex(ctx, hash, &hash_len) == 1;
    EVP_MD_CTX_free(ctx);

    if (!ok || hash_len == 0) {
        throw std::runtime_error("SHA-256 digest failed");
    }

    std::ostringstream oss;
    for (unsigned int i = 0; i < hash_len; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(hash[i]);
    }
    return oss.str();
}

bool TotpService::verifyRecoveryCode(std::vector<std::string>& codes_hash,
                                      const std::string& code) {
    // TASK-20260702-02 P2-2（SR-2）：常量时间比较，避免 `==` 短路暴露计时侧信道。
    // 遍历全部候选、逐个 CRYPTO_memcmp（等长时），记录命中下标后再擦除，不提前 return。
    std::string hashed = hashRecoveryCode(code);
    int match = -1;
    for (size_t i = 0; i < codes_hash.size(); ++i) {
        const std::string& stored = codes_hash[i];
        bool equal = (stored.size() == hashed.size()) &&
                     (CRYPTO_memcmp(stored.data(), hashed.data(),
                                    hashed.size()) == 0);
        if (equal) match = static_cast<int>(i);
    }
    if (match >= 0) {
        codes_hash.erase(codes_hash.begin() + match);
        return true;
    }
    return false;
}

} // namespace aegisgate
