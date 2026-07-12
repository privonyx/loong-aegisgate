#pragma once

// Phase 11.1 TASK-20260523-01 — Epic 2.1 GuardFeedbackPayload.
//
// Canonical schema for FeedbackEventType::GuardFeedback (topic
// "guard.feedback"). Defines:
//   * the 4 supported labels (D3-related shape)
//   * the role allowlist for D4=C "RBAC-restricted feedback ingestion"
//   * validate() that returns structured error_code so the admin endpoint
//     can map 1:1 to 400/403 responses.
//
// Design reference: docs/specs/2026-05-23-phase11.1-adaptive-guard-design.md §5.

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aegisgate::guard {

enum class GuardFeedbackLabel {
    FalsePositive = 0,
    FalseNegative = 1,
    ConfirmedBlock = 2,
    ConfirmedAllow = 3,
};

inline std::string_view labelToString(GuardFeedbackLabel l) {
    switch (l) {
        case GuardFeedbackLabel::FalsePositive:  return "false_positive";
        case GuardFeedbackLabel::FalseNegative:  return "false_negative";
        case GuardFeedbackLabel::ConfirmedBlock: return "confirmed_block";
        case GuardFeedbackLabel::ConfirmedAllow: return "confirmed_allow";
    }
    return "unknown";
}

inline std::optional<GuardFeedbackLabel> parseLabel(std::string_view s) {
    if (s == "false_positive")  return GuardFeedbackLabel::FalsePositive;
    if (s == "false_negative")  return GuardFeedbackLabel::FalseNegative;
    if (s == "confirmed_block") return GuardFeedbackLabel::ConfirmedBlock;
    if (s == "confirmed_allow") return GuardFeedbackLabel::ConfirmedAllow;
    return std::nullopt;
}

// D4=C decision: the set of roles allowed to submit feedback. Order is
// stable because audit fingerprints embed this list.
inline const std::vector<std::string>& allowedReviewerRoles() {
    static const std::vector<std::string> kAllowed = {
        "security_admin",
        "platform_admin",
        "trust_safety",
    };
    return kAllowed;
}

inline bool isReviewerRoleAllowed(std::string_view role) {
    for (const auto& r : allowedReviewerRoles()) {
        if (role == r) return true;
    }
    return false;
}

struct GuardFeedbackPayload {
    std::string request_id;
    std::string trace_id;
    GuardFeedbackLabel label = GuardFeedbackLabel::ConfirmedBlock;
    std::string reviewer_user_id;
    std::string reviewer_role;
    std::string comment;                 // free text, will be PII-masked
    std::string original_text_redacted;  // pre-masked by caller

    nlohmann::json toJson() const {
        return {
            {"request_id", request_id},
            {"trace_id", trace_id},
            {"label", labelToString(label)},
            {"reviewer_user_id", reviewer_user_id},
            {"reviewer_role", reviewer_role},
            {"comment", comment},
            {"original_text_redacted", original_text_redacted},
        };
    }
};

struct ValidateResult {
    bool ok = false;
    std::string error_code;
    std::string detail;
    GuardFeedbackPayload payload;
};

inline ValidateResult validateGuardFeedbackPayload(const nlohmann::json& j) {
    ValidateResult r;
    if (!j.is_object()) {
        r.error_code = "invalid_json";
        return r;
    }
    auto requireString = [&](const char* k) -> std::optional<std::string> {
        if (!j.contains(k) || !j[k].is_string()) {
            r.error_code = "missing_field";
            r.detail = k;
            return std::nullopt;
        }
        return j[k].get<std::string>();
    };
    auto req_id = requireString("request_id");
    if (!req_id) return r;
    auto label  = requireString("label");
    if (!label) return r;
    auto reviewer = requireString("reviewer_user_id");
    if (!reviewer) return r;
    auto role = requireString("reviewer_role");
    if (!role) return r;

    auto parsed = parseLabel(*label);
    if (!parsed) {
        r.error_code = "invalid_label";
        r.detail = *label;
        return r;
    }
    if (!isReviewerRoleAllowed(*role)) {
        r.error_code = "unauthorized_role";
        r.detail = *role;
        return r;
    }

    r.payload.request_id = std::move(*req_id);
    r.payload.label = *parsed;
    r.payload.reviewer_user_id = std::move(*reviewer);
    r.payload.reviewer_role = std::move(*role);
    if (j.contains("trace_id") && j["trace_id"].is_string()) {
        r.payload.trace_id = j["trace_id"].get<std::string>();
    }
    if (j.contains("comment") && j["comment"].is_string()) {
        r.payload.comment = j["comment"].get<std::string>();
    }
    if (j.contains("original_text_redacted") && j["original_text_redacted"].is_string()) {
        r.payload.original_text_redacted = j["original_text_redacted"].get<std::string>();
    }
    r.ok = true;
    return r;
}

}  // namespace aegisgate::guard
