#include <gtest/gtest.h>
#include "core/crypto.h"

using namespace aegisgate::crypto;

TEST(CryptoTest, Sha256ProducesCorrectLength) {
    auto hash = sha256("test");
    EXPECT_EQ(hash.size(), 64u);
}

TEST(CryptoTest, Sha256Deterministic) {
    auto h1 = sha256("hello");
    auto h2 = sha256("hello");
    EXPECT_EQ(h1, h2);
}

TEST(CryptoTest, Sha256DifferentInputDifferentOutput) {
    auto h1 = sha256("abc");
    auto h2 = sha256("def");
    EXPECT_NE(h1, h2);
}

TEST(CryptoTest, ConstantTimeEqualsTrue) {
    EXPECT_TRUE(constantTimeEquals("abc", "abc"));
    EXPECT_TRUE(constantTimeEquals("", ""));
}

TEST(CryptoTest, ConstantTimeEqualsFalse) {
    EXPECT_FALSE(constantTimeEquals("abc", "abd"));
    EXPECT_FALSE(constantTimeEquals("abc", "ab"));
    EXPECT_FALSE(constantTimeEquals("abc", "abcd"));
}
