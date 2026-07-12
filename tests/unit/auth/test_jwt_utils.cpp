#include <gtest/gtest.h>
#include "auth/jwt_utils.h"

using namespace aegisgate;

class JwtUtilsTest : public ::testing::Test {
protected:
    const std::string secret_ = "test-secret-key-at-least-32-bytes!!";
};

TEST_F(JwtUtilsTest, SignAndVerifyRoundTrip) {
    JwtPayload payload{"user123", "tenant456", "super_admin"};
    auto token = JwtUtils::sign(payload, secret_, 3600);
    ASSERT_FALSE(token.empty());

    auto result = JwtUtils::verify(token, secret_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->user_id, "user123");
    EXPECT_EQ(result->tenant_id, "tenant456");
    EXPECT_EQ(result->role, "super_admin");
}

TEST_F(JwtUtilsTest, ExpiredTokenRejected) {
    JwtPayload payload{"u", "t", "viewer"};
    auto token = JwtUtils::sign(payload, secret_, -1);
    auto result = JwtUtils::verify(token, secret_);
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtUtilsTest, TamperedTokenRejected) {
    JwtPayload payload{"u", "t", "viewer"};
    auto token = JwtUtils::sign(payload, secret_);
    auto dot = token.find('.');
    token[dot + 1] = (token[dot + 1] == 'A') ? 'B' : 'A';
    auto result = JwtUtils::verify(token, secret_);
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtUtilsTest, WrongSecretRejected) {
    JwtPayload payload{"u", "t", "viewer"};
    auto token = JwtUtils::sign(payload, secret_);
    auto result = JwtUtils::verify(token, "wrong-secret-key-at-least-32-bytes!");
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtUtilsTest, MalformedTokenRejected) {
    EXPECT_FALSE(JwtUtils::verify("not.a.jwt", secret_).has_value());
    EXPECT_FALSE(JwtUtils::verify("", secret_).has_value());
    EXPECT_FALSE(JwtUtils::verify("onlyonepart", secret_).has_value());
}

TEST_F(JwtUtilsTest, EmptyFieldsPreserved) {
    JwtPayload payload{"", "", "viewer"};
    auto token = JwtUtils::sign(payload, secret_, 3600);
    auto result = JwtUtils::verify(token, secret_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->user_id, "");
    EXPECT_EQ(result->tenant_id, "");
    EXPECT_EQ(result->role, "viewer");
}
