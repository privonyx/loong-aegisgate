#include "auth/encryption.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/kdf.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <cstdlib>
#include <stdexcept>

namespace aegisgate {

static constexpr int kIvSize = 12;
static constexpr int kTagSize = 16;
static constexpr int kKeySize = 32;
static constexpr int kHexKeyLen = 64;

static std::vector<uint8_t> hexDecode(const std::string& hex) {
    if (hex.size() % 2 != 0) return {};
    std::vector<uint8_t> out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        unsigned int byte = 0;
        auto hi = hex[2 * i], lo = hex[2 * i + 1];
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + c - 'a';
            if (c >= 'A' && c <= 'F') return 10 + c - 'A';
            return -1;
        };
        int h = nibble(hi), l = nibble(lo);
        if (h < 0 || l < 0) return {};
        byte = static_cast<unsigned int>(h) << 4 | static_cast<unsigned int>(l);
        out[i] = static_cast<uint8_t>(byte);
    }
    return out;
}

static std::string base64Encode(const std::vector<uint8_t>& data) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data.data(), static_cast<int>(data.size()));
    BIO_flush(b64);

    BUF_MEM* bptr = nullptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string result(bptr->data, bptr->length);
    BIO_free_all(b64);
    return result;
}

static std::vector<uint8_t> base64Decode(const std::string& encoded) {
    std::vector<uint8_t> out(encoded.size());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new_mem_buf(encoded.data(), static_cast<int>(encoded.size()));
    mem = BIO_push(b64, mem);
    BIO_set_flags(mem, BIO_FLAGS_BASE64_NO_NL);
    int len = BIO_read(mem, out.data(), static_cast<int>(out.size()));
    BIO_free_all(mem);
    if (len < 0) return {};
    out.resize(static_cast<size_t>(len));
    return out;
}

Encryption::Encryption() {
    const char* env = std::getenv("AEGISGATE_ENCRYPTION_KEY");
    if (!env) return;
    std::string hex(env);
    if (hex.size() != kHexKeyLen) return;
    auto key = hexDecode(hex);
    if (key.size() != kKeySize) return;
    master_key_ = std::move(key);
    available_ = true;
}

Encryption& Encryption::instance() {
    static Encryption inst;
    return inst;
}

bool Encryption::isAvailable() const { return available_; }

std::vector<uint8_t> Encryption::deriveKey(const std::string& purpose) const {
    std::vector<uint8_t> derived(kKeySize);

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!ctx) return {};

    bool ok = true;
    ok = ok && EVP_PKEY_derive_init(ctx) > 0;
    ok = ok && EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) > 0;
    ok = ok && EVP_PKEY_CTX_set1_hkdf_salt(
        ctx, reinterpret_cast<const unsigned char*>("aegisgate"), 9) > 0;
    ok = ok && EVP_PKEY_CTX_set1_hkdf_key(
        ctx, master_key_.data(), static_cast<int>(master_key_.size())) > 0;
    ok = ok && EVP_PKEY_CTX_add1_hkdf_info(
        ctx, reinterpret_cast<const unsigned char*>(purpose.data()),
        static_cast<int>(purpose.size())) > 0;

    size_t outlen = kKeySize;
    ok = ok && EVP_PKEY_derive(ctx, derived.data(), &outlen) > 0;
    EVP_PKEY_CTX_free(ctx);

    if (!ok || outlen != kKeySize) return {};
    return derived;
}

std::string Encryption::encrypt(const std::string& plaintext, const std::string& purpose) const {
    if (!available_) return "";

    auto key = deriveKey(purpose);
    if (key.empty()) return "";

    std::vector<uint8_t> iv(kIvSize);
    if (RAND_bytes(iv.data(), kIvSize) != 1) return "";

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return "";

    std::vector<uint8_t> ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
    int len = 0, ciphertext_len = 0;

    bool ok = true;
    ok = ok && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvSize, nullptr) == 1;
    ok = ok && EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) == 1;
    ok = ok && EVP_EncryptUpdate(ctx,
        ciphertext.data(), &len,
        reinterpret_cast<const unsigned char*>(plaintext.data()),
        static_cast<int>(plaintext.size())) == 1;
    ciphertext_len = len;
    ok = ok && EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) == 1;
    ciphertext_len += len;

    std::vector<uint8_t> tag(kTagSize);
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagSize, tag.data()) == 1;
    EVP_CIPHER_CTX_free(ctx);

    if (!ok) return "";

    // IV || ciphertext || tag
    std::vector<uint8_t> combined;
    combined.reserve(static_cast<size_t>(kIvSize + ciphertext_len + kTagSize));
    combined.insert(combined.end(), iv.begin(), iv.end());
    combined.insert(combined.end(), ciphertext.begin(), ciphertext.begin() + ciphertext_len);
    combined.insert(combined.end(), tag.begin(), tag.end());

    return base64Encode(combined);
}

std::optional<std::string> Encryption::decrypt(const std::string& ciphertext_b64,
                                                const std::string& purpose) const {
    if (!available_) return std::nullopt;

    auto combined = base64Decode(ciphertext_b64);
    if (combined.size() < static_cast<size_t>(kIvSize + kTagSize)) return std::nullopt;

    auto key = deriveKey(purpose);
    if (key.empty()) return std::nullopt;

    const uint8_t* iv_ptr = combined.data();
    size_t ct_len = combined.size() - kIvSize - kTagSize;
    const uint8_t* ct_ptr = combined.data() + kIvSize;
    const uint8_t* tag_ptr = combined.data() + kIvSize + ct_len;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return std::nullopt;

    std::vector<uint8_t> plaintext(ct_len + EVP_MAX_BLOCK_LENGTH);
    int len = 0, plaintext_len = 0;

    bool ok = true;
    ok = ok && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvSize, nullptr) == 1;
    ok = ok && EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv_ptr) == 1;
    ok = ok && EVP_DecryptUpdate(ctx, plaintext.data(), &len,
                                  ct_ptr, static_cast<int>(ct_len)) == 1;
    plaintext_len = len;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagSize,
                                    const_cast<uint8_t*>(tag_ptr)) == 1;

    int ret = EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len);
    EVP_CIPHER_CTX_free(ctx);

    if (!ok || ret <= 0) return std::nullopt;
    plaintext_len += len;

    return std::string(reinterpret_cast<char*>(plaintext.data()),
                       static_cast<size_t>(plaintext_len));
}

} // namespace aegisgate
