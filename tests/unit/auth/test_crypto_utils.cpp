#include "auth/crypto_utils.h"
#include <gtest/gtest.h>
#include <set>

using namespace aegisgate::auth;

TEST(GenerateApiKeyTest, HasSkPrefix) {
    auto key = generateApiKey();
    EXPECT_EQ(key.substr(0, 3), "sk-");
}

TEST(GenerateApiKeyTest, HasExpectedLength) {
    auto key = generateApiKey();
    EXPECT_EQ(key.size(), 36u);
}

TEST(GenerateApiKeyTest, ContainsOnlyValidChars) {
    auto key = generateApiKey();
    for (size_t i = 3; i < key.size(); ++i) {
        char c = key[i];
        EXPECT_TRUE((c >= '0' && c <= '9') ||
                     (c >= 'A' && c <= 'Z') ||
                     (c >= 'a' && c <= 'z'))
            << "Invalid char at position " << i << ": " << c;
    }
}

TEST(GenerateApiKeyTest, UniqueKeys) {
    std::set<std::string> keys;
    for (int i = 0; i < 100; ++i) {
        auto key = generateApiKey();
        EXPECT_TRUE(keys.insert(key).second) << "Duplicate key: " << key;
    }
}

TEST(ExtractKeyPrefixTest, Normal) {
    EXPECT_EQ(extractKeyPrefix("sk-abcdef12345"), "sk-abcde");
}

TEST(ExtractKeyPrefixTest, ExactLength) {
    EXPECT_EQ(extractKeyPrefix("12345678"), "12345678");
}

TEST(ExtractKeyPrefixTest, Short) {
    EXPECT_EQ(extractKeyPrefix("abc"), "abc");
}

TEST(HashApiKeyTest, DeterministicOutput) {
    auto h1 = hashApiKey("hello");
    auto h2 = hashApiKey("hello");
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(h1.size(), 64u);
}

TEST(HashApiKeyTest, DifferentInputDifferentHash) {
    EXPECT_NE(hashApiKey("hello"), hashApiKey("world"));
}

TEST(VerifyApiKeyTest, MatchingHashes) {
    auto h1 = hashApiKey("test-key-1");
    auto h2 = hashApiKey("test-key-1");
    EXPECT_TRUE(verifyApiKey(h1, h2));
}

TEST(VerifyApiKeyTest, NonMatchingHashes) {
    auto h1 = hashApiKey("test-key-1");
    auto h2 = hashApiKey("test-key-2");
    EXPECT_FALSE(verifyApiKey(h1, h2));
}

TEST(VerifyApiKeyTest, DifferentLengths) {
    EXPECT_FALSE(verifyApiKey("abc", "ab"));
    EXPECT_FALSE(verifyApiKey("abc", "abcd"));
}

TEST(RoundTripTest, GenerateHashVerify) {
    auto key = generateApiKey();
    auto prefix = extractKeyPrefix(key);
    auto hash = hashApiKey(key);

    EXPECT_EQ(prefix.size(), 8u);
    EXPECT_EQ(hash.size(), 64u);
    EXPECT_TRUE(verifyApiKey(hash, hashApiKey(key)));
    EXPECT_FALSE(verifyApiKey(hash, hashApiKey(key + "x")));
}
