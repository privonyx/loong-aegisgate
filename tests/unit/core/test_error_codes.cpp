#include <gtest/gtest.h>
#include <aegisgate/error_codes.h>
#include <string>
#include <vector>

using namespace aegisgate;

TEST(ErrorCodeTest, AegisCodeFormat) {
    EXPECT_EQ(toAegisCode(ErrorCode::InvalidApiKey), "AEGIS-1001");
    EXPECT_EQ(toAegisCode(ErrorCode::RateLimitExceeded), "AEGIS-2001");
    EXPECT_EQ(toAegisCode(ErrorCode::InjectionDetected), "AEGIS-3001");
    EXPECT_EQ(toAegisCode(ErrorCode::NoModelAvailable), "AEGIS-4001");
    EXPECT_EQ(toAegisCode(ErrorCode::InvalidRequest), "AEGIS-5001");
    EXPECT_EQ(toAegisCode(ErrorCode::NotInitialized), "AEGIS-9001");
}

TEST(ErrorCodeTest, ErrorTypeMapping) {
    EXPECT_STREQ(toErrorType(ErrorCode::InvalidApiKey), "authentication_error");
    EXPECT_STREQ(toErrorType(ErrorCode::InvalidAdminKey), "authentication_error");
    EXPECT_STREQ(toErrorType(ErrorCode::RateLimitExceeded), "rate_limit_error");
    EXPECT_STREQ(toErrorType(ErrorCode::AbuseDetected), "rate_limit_error");
    EXPECT_STREQ(toErrorType(ErrorCode::InjectionDetected), "security_error");
    EXPECT_STREQ(toErrorType(ErrorCode::ContentFiltered), "security_error");
    EXPECT_STREQ(toErrorType(ErrorCode::NoModelAvailable), "routing_error");
    EXPECT_STREQ(toErrorType(ErrorCode::UpstreamError), "routing_error");
    EXPECT_STREQ(toErrorType(ErrorCode::InvalidRequest), "validation_error");
    EXPECT_STREQ(toErrorType(ErrorCode::PayloadTooLarge), "validation_error");
    EXPECT_STREQ(toErrorType(ErrorCode::NotInitialized), "system_error");
    EXPECT_STREQ(toErrorType(ErrorCode::InternalError), "system_error");
}

TEST(ErrorCodeTest, HttpStatusMapping) {
    EXPECT_EQ(toHttpStatus(ErrorCode::InvalidApiKey), 401);
    EXPECT_EQ(toHttpStatus(ErrorCode::InsufficientPermissions), 403);
    EXPECT_EQ(toHttpStatus(ErrorCode::RateLimitExceeded), 429);
    EXPECT_EQ(toHttpStatus(ErrorCode::InjectionDetected), 403);
    EXPECT_EQ(toHttpStatus(ErrorCode::NoModelAvailable), 503);
    EXPECT_EQ(toHttpStatus(ErrorCode::UpstreamTimeout), 504);
    EXPECT_EQ(toHttpStatus(ErrorCode::UpstreamError), 502);
    EXPECT_EQ(toHttpStatus(ErrorCode::InvalidRequest), 400);
    EXPECT_EQ(toHttpStatus(ErrorCode::PayloadTooLarge), 413);
    EXPECT_EQ(toHttpStatus(ErrorCode::NotInitialized), 503);
    EXPECT_EQ(toHttpStatus(ErrorCode::InternalError), 500);
}

TEST(ErrorCodeTest, DefaultMessageNotEmpty) {
    std::vector<ErrorCode> all_codes = {
        ErrorCode::InvalidApiKey, ErrorCode::InsufficientPermissions,
        ErrorCode::InvalidAdminKey, ErrorCode::RateLimitExceeded,
        ErrorCode::QuotaExceeded, ErrorCode::AbuseDetected,
        ErrorCode::InjectionDetected, ErrorCode::PiiBlocked,
        ErrorCode::TopicViolation, ErrorCode::ContentFiltered,
        ErrorCode::EncodingAttack, ErrorCode::NoModelAvailable,
        ErrorCode::CircuitBreakerOpen, ErrorCode::UpstreamTimeout,
        ErrorCode::UpstreamError, ErrorCode::InvalidRequest,
        ErrorCode::PayloadTooLarge, ErrorCode::MissingRequiredField,
        ErrorCode::NotInitialized, ErrorCode::InternalError,
        ErrorCode::CacheUnavailable,
    };
    for (auto code : all_codes) {
        auto msg = toDefaultMessage(code);
        EXPECT_NE(std::string(msg), "") << "Empty message for " << toAegisCode(code);
        EXPECT_NE(std::string(msg), "Unknown error") << "Missing message for " << toAegisCode(code);
    }
}

TEST(ErrorCodeTest, DocUrlFormat) {
    EXPECT_EQ(toDocUrl(ErrorCode::InvalidApiKey),
              "https://aegisgate.dev/docs/errors#AEGIS-1001");
    EXPECT_EQ(toDocUrl(ErrorCode::InternalError),
              "https://aegisgate.dev/docs/errors#AEGIS-9002");
}
