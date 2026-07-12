#include "auth/encryption.h"
#include <gtest/gtest.h>
#include <cstdlib>

using namespace aegisgate;

class EncryptionTest : public ::testing::Test {
protected:
    void SetUp() override {
        setenv("AEGISGATE_ENCRYPTION_KEY",
               "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 1);
    }
    void TearDown() override {
        unsetenv("AEGISGATE_ENCRYPTION_KEY");
    }
};

TEST_F(EncryptionTest, RoundTrip) {
    Encryption enc;
    ASSERT_TRUE(enc.isAvailable());

    std::string plaintext = "my-secret-client-secret";
    auto encrypted = enc.encrypt(plaintext, "sso");
    EXPECT_NE(encrypted, plaintext);
    EXPECT_FALSE(encrypted.empty());

    auto decrypted = enc.decrypt(encrypted, "sso");
    ASSERT_TRUE(decrypted.has_value());
    EXPECT_EQ(*decrypted, plaintext);
}

TEST_F(EncryptionTest, DifferentPurposeCannotDecrypt) {
    Encryption enc;
    auto encrypted = enc.encrypt("secret", "sso");
    auto decrypted = enc.decrypt(encrypted, "totp");
    EXPECT_FALSE(decrypted.has_value());
}

TEST_F(EncryptionTest, TamperedDataFails) {
    Encryption enc;
    auto encrypted = enc.encrypt("secret", "sso");
    std::string tampered = encrypted;
    if (!tampered.empty()) {
        tampered[tampered.size() / 2] ^= 0x01;
    }
    auto decrypted = enc.decrypt(tampered, "sso");
    EXPECT_FALSE(decrypted.has_value());
}

TEST_F(EncryptionTest, EmptyPlaintext) {
    Encryption enc;
    auto encrypted = enc.encrypt("", "sso");
    auto decrypted = enc.decrypt(encrypted, "sso");
    ASSERT_TRUE(decrypted.has_value());
    EXPECT_EQ(*decrypted, "");
}

TEST_F(EncryptionTest, NotAvailableWithoutKey) {
    unsetenv("AEGISGATE_ENCRYPTION_KEY");
    Encryption enc;
    EXPECT_FALSE(enc.isAvailable());
    auto encrypted = enc.encrypt("secret", "sso");
    EXPECT_TRUE(encrypted.empty());
}

TEST_F(EncryptionTest, InvalidKeyLength) {
    setenv("AEGISGATE_ENCRYPTION_KEY", "0123456789abcdef", 1);
    Encryption enc;
    EXPECT_FALSE(enc.isAvailable());
}
