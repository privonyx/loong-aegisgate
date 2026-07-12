#include <gtest/gtest.h>
#include "gateway/rate_limiter.h"

using namespace aegisgate;

class LoginRateLimitTest : public ::testing::Test {
protected:
    static constexpr double kMaxTokens = 10.0;
    static constexpr double kRefillRate = 0.1;

    RateLimiter limiter_{{kMaxTokens, kRefillRate}};
};

TEST_F(LoginRateLimitTest, AllowsUpToMaxAttempts) {
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(limiter_.allow("192.168.1.1")) << "Attempt " << i << " should be allowed";
    }
}

TEST_F(LoginRateLimitTest, RejectsAfterMaxAttempts) {
    for (int i = 0; i < 10; ++i) {
        limiter_.allow("192.168.1.1");
    }
    EXPECT_FALSE(limiter_.allow("192.168.1.1"));
}

TEST_F(LoginRateLimitTest, DifferentIPsAreIndependent) {
    for (int i = 0; i < 10; ++i) {
        limiter_.allow("10.0.0.1");
    }
    EXPECT_FALSE(limiter_.allow("10.0.0.1"));
    EXPECT_TRUE(limiter_.allow("10.0.0.2"));
}

TEST_F(LoginRateLimitTest, RemainingDecreasesCorrectly) {
    EXPECT_DOUBLE_EQ(limiter_.remaining("10.0.0.50"), 10.0);
    limiter_.allow("10.0.0.50");
    EXPECT_NEAR(limiter_.remaining("10.0.0.50"), 9.0, 0.2);
}
