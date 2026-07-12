#include "auth/totp_service.h"
#include <gtest/gtest.h>
#include <openssl/hmac.h>
#include <set>
#include <regex>
#include <cstdio>

using aegisgate::TotpService;

// Helper: access private base32 methods via the public interface roundtrip
namespace {

std::vector<uint8_t> decodeBase32(const std::string& encoded) {
    std::vector<uint8_t> result;
    int buffer = 0;
    int bits_left = 0;
    for (char c : encoded) {
        char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        int val;
        if (upper >= 'A' && upper <= 'Z') val = upper - 'A';
        else if (upper >= '2' && upper <= '7') val = upper - '2' + 26;
        else continue;
        buffer = (buffer << 5) | val;
        bits_left += 5;
        if (bits_left >= 8) {
            bits_left -= 8;
            result.push_back(static_cast<uint8_t>((buffer >> bits_left) & 0xFF));
        }
    }
    return result;
}

std::string encodeBase32(const std::vector<uint8_t>& data) {
    constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string result;
    int buffer = 0;
    int bits_left = 0;
    for (uint8_t byte : data) {
        buffer = (buffer << 8) | byte;
        bits_left += 8;
        while (bits_left >= 5) {
            bits_left -= 5;
            result += ALPHABET[(buffer >> bits_left) & 0x1F];
        }
    }
    if (bits_left > 0) {
        result += ALPHABET[(buffer << (5 - bits_left)) & 0x1F];
    }
    return result;
}

// TOTP code generation matching TotpService::generateCode for test vectors
std::string computeTotp(const std::vector<uint8_t>& secret, int64_t timestamp) {
    int64_t counter = timestamp / 30;
    uint8_t counter_bytes[8];
    for (int i = 7; i >= 0; --i) {
        counter_bytes[i] = static_cast<uint8_t>(counter & 0xFF);
        counter >>= 8;
    }
    unsigned char hmac_result[EVP_MAX_MD_SIZE];
    unsigned int hmac_len = 0;
    HMAC(EVP_sha1(), secret.data(), static_cast<int>(secret.size()),
         counter_bytes, 8, hmac_result, &hmac_len);
    int offset = hmac_result[hmac_len - 1] & 0x0F;
    uint32_t truncated =
        (static_cast<uint32_t>(hmac_result[offset] & 0x7F) << 24) |
        (static_cast<uint32_t>(hmac_result[offset + 1]) << 16) |
        (static_cast<uint32_t>(hmac_result[offset + 2]) << 8) |
        static_cast<uint32_t>(hmac_result[offset + 3]);
    uint32_t otp = truncated % 1000000;
    char buf[7];
    std::snprintf(buf, sizeof(buf), "%06u", otp);
    return std::string(buf);
}

} // namespace

TEST(TotpServiceTest, GenerateSecretLength) {
    auto secret = TotpService::generateSecret();
    EXPECT_FALSE(secret.empty());
    auto decoded = decodeBase32(secret);
    EXPECT_EQ(decoded.size(), 20u);
}

TEST(TotpServiceTest, GenerateSecretUniqueness) {
    auto s1 = TotpService::generateSecret();
    auto s2 = TotpService::generateSecret();
    EXPECT_NE(s1, s2);
}

TEST(TotpServiceTest, GenerateOtpAuthUri) {
    auto uri = TotpService::generateOtpAuthUri("JBSWY3DPEHPK3PXP", "user@example.com", "AegisGate");
    EXPECT_TRUE(uri.find("otpauth://totp/") == 0);
    EXPECT_NE(uri.find("secret=JBSWY3DPEHPK3PXP"), std::string::npos);
    EXPECT_NE(uri.find("issuer=AegisGate"), std::string::npos);
    EXPECT_NE(uri.find("algorithm=SHA1"), std::string::npos);
    EXPECT_NE(uri.find("digits=6"), std::string::npos);
    EXPECT_NE(uri.find("period=30"), std::string::npos);
}

TEST(TotpServiceTest, VerifyCodeValid) {
    // RFC 6238 test vector: secret = "12345678901234567890" (ASCII), time=59 → "287082"
    std::string ascii_secret = "12345678901234567890";
    std::vector<uint8_t> secret_bytes(ascii_secret.begin(), ascii_secret.end());
    std::string secret_b32 = encodeBase32(secret_bytes);

    EXPECT_TRUE(TotpService::verifyCode(secret_b32, "287082", 59));
}

TEST(TotpServiceTest, VerifyCodeWrong) {
    std::string ascii_secret = "12345678901234567890";
    std::vector<uint8_t> secret_bytes(ascii_secret.begin(), ascii_secret.end());
    std::string secret_b32 = encodeBase32(secret_bytes);

    // At time=59, valid code is "287082"; "000000" should fail
    EXPECT_FALSE(TotpService::verifyCode(secret_b32, "000000", 59));
}

TEST(TotpServiceTest, VerifyCodeAdjacentWindow) {
    std::string ascii_secret = "12345678901234567890";
    std::vector<uint8_t> secret_bytes(ascii_secret.begin(), ascii_secret.end());
    std::string secret_b32 = encodeBase32(secret_bytes);

    int64_t T = 90;  // counter = 3
    std::string code = computeTotp(secret_bytes, T);

    // Next window: T+30 → counter = 4, tolerance checks counter 3,4,5
    EXPECT_TRUE(TotpService::verifyCode(secret_b32, code, T + 30));
    // Previous window: T-30 → counter = 2, tolerance checks counter 1,2,3
    EXPECT_TRUE(TotpService::verifyCode(secret_b32, code, T - 30));
}

TEST(TotpServiceTest, VerifyCodeOutsideWindow) {
    std::string ascii_secret = "12345678901234567890";
    std::vector<uint8_t> secret_bytes(ascii_secret.begin(), ascii_secret.end());
    std::string secret_b32 = encodeBase32(secret_bytes);

    int64_t T = 90;
    std::string code = computeTotp(secret_bytes, T);

    // 3 windows later: T+90 → counter = 6, tolerance checks 5,6,7 — code for counter 3 should fail
    EXPECT_FALSE(TotpService::verifyCode(secret_b32, code, T + 90));
}

TEST(TotpServiceTest, VerifyCodeRfcVector2) {
    // RFC 6238: time=1111111109 (counter=37037036) → "081804"
    std::string ascii_secret = "12345678901234567890";
    std::vector<uint8_t> secret_bytes(ascii_secret.begin(), ascii_secret.end());
    std::string secret_b32 = encodeBase32(secret_bytes);

    EXPECT_TRUE(TotpService::verifyCode(secret_b32, "081804", 1111111109));
}

TEST(TotpServiceTest, GenerateRecoveryCodes) {
    auto codes = TotpService::generateRecoveryCodes(8);
    EXPECT_EQ(codes.size(), 8u);

    std::set<std::string> unique_codes(codes.begin(), codes.end());
    EXPECT_EQ(unique_codes.size(), 8u);

    // TASK-20260702-02 P2-2（SR-2）：熵升级到 10 字节 = 20 hex 字符（≥80bit）。
    std::regex hex_pattern("^[0-9A-F]{20}$");
    for (const auto& code : codes) {
        EXPECT_EQ(code.size(), 20u);
        EXPECT_TRUE(std::regex_match(code, hex_pattern));
    }
}

TEST(TotpServiceTest, VerifyRecoveryCodeValid) {
    auto codes = TotpService::generateRecoveryCodes(8);
    std::vector<std::string> hashes;
    for (const auto& c : codes) {
        hashes.push_back(TotpService::hashRecoveryCode(c));
    }

    EXPECT_TRUE(TotpService::verifyRecoveryCode(hashes, codes[0]));
    EXPECT_EQ(hashes.size(), 7u);
}

TEST(TotpServiceTest, VerifyRecoveryCodeUsed) {
    auto codes = TotpService::generateRecoveryCodes(8);
    std::vector<std::string> hashes;
    for (const auto& c : codes) {
        hashes.push_back(TotpService::hashRecoveryCode(c));
    }

    EXPECT_TRUE(TotpService::verifyRecoveryCode(hashes, codes[0]));
    EXPECT_FALSE(TotpService::verifyRecoveryCode(hashes, codes[0]));
}

TEST(TotpServiceTest, VerifyRecoveryCodeInvalid) {
    auto codes = TotpService::generateRecoveryCodes(8);
    std::vector<std::string> hashes;
    for (const auto& c : codes) {
        hashes.push_back(TotpService::hashRecoveryCode(c));
    }

    EXPECT_FALSE(TotpService::verifyRecoveryCode(hashes, "INVALID1"));
    EXPECT_EQ(hashes.size(), 8u);
}

TEST(TotpServiceTest, Base32RoundTrip) {
    std::vector<uint8_t> original(20);
    for (int i = 0; i < 20; ++i) original[i] = static_cast<uint8_t>(i * 13 + 7);

    std::string encoded = encodeBase32(original);
    auto decoded = decodeBase32(encoded);

    EXPECT_EQ(original, decoded);
}
