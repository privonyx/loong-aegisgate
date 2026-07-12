// Phase 11.1 TASK-20260523-01 — Epic 2.1 GuardFeedbackPayload tests.

#include "guardrail/feedback/guard_feedback_payload.h"

#include <gtest/gtest.h>

using aegisgate::guard::GuardFeedbackLabel;
using aegisgate::guard::GuardFeedbackPayload;
using aegisgate::guard::parseLabel;
using aegisgate::guard::validateGuardFeedbackPayload;

TEST(GuardFeedbackPayloadTest, ParseLabelKnownVariants) {
    EXPECT_EQ(parseLabel("false_positive"), GuardFeedbackLabel::FalsePositive);
    EXPECT_EQ(parseLabel("false_negative"), GuardFeedbackLabel::FalseNegative);
    EXPECT_EQ(parseLabel("confirmed_block"), GuardFeedbackLabel::ConfirmedBlock);
    EXPECT_EQ(parseLabel("confirmed_allow"), GuardFeedbackLabel::ConfirmedAllow);
    EXPECT_EQ(parseLabel("UNKNOWN_LABEL"), std::nullopt);
}

TEST(GuardFeedbackPayloadTest, ValidPayloadRoundTrip) {
    nlohmann::json j = {
        {"request_id", "req-123"},
        {"trace_id", "trace-abc"},
        {"label", "false_positive"},
        {"reviewer_user_id", "user-1"},
        {"reviewer_role", "security_admin"},
        {"comment", "Manual review: legitimate technical question."},
        {"original_text_redacted", "How do I [REDACTED] my SSL cert?"},
    };
    auto res = validateGuardFeedbackPayload(j);
    ASSERT_TRUE(res.ok) << res.error_code << ": " << res.detail;
    EXPECT_EQ(res.payload.request_id, "req-123");
    EXPECT_EQ(res.payload.label, GuardFeedbackLabel::FalsePositive);
    EXPECT_EQ(res.payload.reviewer_role, "security_admin");
}

TEST(GuardFeedbackPayloadTest, RejectsMissingRequiredField) {
    nlohmann::json j = {
        {"request_id", "req-123"},
        // missing label
        {"reviewer_user_id", "user-1"},
        {"reviewer_role", "security_admin"},
    };
    auto res = validateGuardFeedbackPayload(j);
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error_code, "missing_field");
}

TEST(GuardFeedbackPayloadTest, RejectsUnknownLabel) {
    nlohmann::json j = {
        {"request_id", "req-123"},
        {"label", "definitely-not-a-label"},
        {"reviewer_user_id", "user-1"},
        {"reviewer_role", "security_admin"},
    };
    auto res = validateGuardFeedbackPayload(j);
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error_code, "invalid_label");
}

TEST(GuardFeedbackPayloadTest, RejectsUnauthorizedRole) {
    // D4=C decision: feedback only accepted from privileged roles.
    nlohmann::json j = {
        {"request_id", "req-123"},
        {"label", "false_positive"},
        {"reviewer_user_id", "user-1"},
        {"reviewer_role", "end_user"},  // not in allowlist
    };
    auto res = validateGuardFeedbackPayload(j);
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error_code, "unauthorized_role");
}

TEST(GuardFeedbackPayloadTest, ToJsonCanonicalShape) {
    GuardFeedbackPayload p;
    p.request_id = "req-1";
    p.trace_id = "trace-1";
    p.label = GuardFeedbackLabel::ConfirmedBlock;
    p.reviewer_user_id = "user-1";
    p.reviewer_role = "security_admin";
    p.comment = "Genuine prompt injection attempt.";
    p.original_text_redacted = "[REDACTED]";

    auto j = p.toJson();
    EXPECT_EQ(j["label"], "confirmed_block");
    EXPECT_EQ(j["reviewer_role"], "security_admin");
    EXPECT_TRUE(j.contains("request_id"));
    EXPECT_TRUE(j.contains("trace_id"));
    EXPECT_TRUE(j.contains("comment"));
    EXPECT_TRUE(j.contains("original_text_redacted"));
}
