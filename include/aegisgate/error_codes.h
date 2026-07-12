#pragma once
#include <string>
#include <stdexcept>

namespace aegisgate {

enum class ErrorCode {
    // AEGIS-1xxx: Authentication & Authorization
    InvalidApiKey       = 1001,
    InsufficientPermissions = 1002,
    InvalidAdminKey     = 1003,
    ApiKeyExpired       = 1004,
    ApiKeyRevoked       = 1005,
    AccountDisabled     = 1006,
    TenantSuspended     = 1007,
    CrossTenantDenied   = 1008,

    // AEGIS-101x: SSO
    SsoConfigMissing    = 1010,
    SsoCallbackInvalid  = 1011,
    SsoTokenInvalid     = 1012,
    SsoProviderError    = 1013,
    SsoAccountNotMapped = 1014,

    // AEGIS-102x: MFA
    MfaRequired         = 1020,
    MfaInvalidCode      = 1021,
    MfaNotSetup         = 1022,
    MfaRecoveryUsed     = 1023,

    // AEGIS-103x: Session
    SessionExpired      = 1030,
    SessionNotFound     = 1031,
    SessionLimitReached = 1032,

    // AEGIS-104x: SCIM
    ScimTokenInvalid    = 1040,

    // AEGIS-2xxx: Rate Limiting & Quota
    RateLimitExceeded   = 2001,
    QuotaExceeded       = 2002,
    AbuseDetected       = 2003,
    CostLimitExceeded   = 2004,
    ModalityQuotaExceeded = 2005,  // Phase 6.1 Epic 5.1c (B1, TASK-20260515-01)

    // AEGIS-3xxx: Security & Safety
    InjectionDetected   = 3001,
    PiiBlocked          = 3002,
    TopicViolation      = 3003,
    ContentFiltered     = 3004,
    EncodingAttack      = 3005,

    // AEGIS-4xxx: Routing & Model
    NoModelAvailable    = 4001,
    CircuitBreakerOpen  = 4002,
    UpstreamTimeout     = 4003,
    UpstreamError       = 4004,
    ModelNotAllowed     = 4005,
    UnsupportedEndpoint = 4006,
    NoHealthyKeys       = 4007,  // P0-3: provider's API-key pool exhausted/unhealthy

    // AEGIS-5xxx: Request Validation
    InvalidRequest      = 5001,
    PayloadTooLarge     = 5002,
    MissingRequiredField = 5003,
    TenantNameExists    = 5010,
    UsernameExists      = 5011,

    // AEGIS-6xxx: Autonomy & Cost Governance (Phase 11.5, TASK-20260518-02 E4.3)
    BudgetExceeded      = 6001,  // BudgetGuardStage: tenant 24h or per-req cap
    AutonomyDisabled    = 6002,  // workflow refused: AEGISGATE_DISABLE_AUTONOMY=1
    ApprovalNotFound    = 6003,  // admin API: proposal id unknown
    ApprovalStateInvalid = 6004, // admin API: state transition not allowed
    PayloadTampered     = 6005,  // workflow.apply(): SHA-256 mismatch (T01)

    // AEGIS-9xxx: System
    NotInitialized      = 9001,
    InternalError       = 9002,
    CacheUnavailable    = 9003,
};

inline std::string toAegisCode(ErrorCode code) {
    return "AEGIS-" + std::to_string(static_cast<int>(code));
}

inline const char* toErrorType(ErrorCode code) {
    int category = static_cast<int>(code) / 1000;
    switch (category) {
        case 1: return "authentication_error";
        case 2: return "rate_limit_error";
        case 3: return "security_error";
        case 4: return "routing_error";
        case 5: return "validation_error";
        case 6: return "autonomy_error";
        case 9: return "system_error";
        default: return "unknown_error";
    }
}

inline int toHttpStatus(ErrorCode code) {
    switch (code) {
        case ErrorCode::InvalidApiKey:          return 401;
        case ErrorCode::InsufficientPermissions: return 403;
        case ErrorCode::InvalidAdminKey:        return 401;
        case ErrorCode::ApiKeyExpired:          return 401;
        case ErrorCode::ApiKeyRevoked:          return 401;
        case ErrorCode::AccountDisabled:        return 403;
        case ErrorCode::TenantSuspended:        return 403;
        case ErrorCode::CrossTenantDenied:      return 403;
        case ErrorCode::SsoConfigMissing:       return 404;
        case ErrorCode::SsoCallbackInvalid:     return 400;
        case ErrorCode::SsoTokenInvalid:        return 401;
        case ErrorCode::SsoProviderError:       return 502;
        case ErrorCode::SsoAccountNotMapped:    return 403;
        case ErrorCode::MfaRequired:            return 403;
        case ErrorCode::MfaInvalidCode:         return 401;
        case ErrorCode::MfaNotSetup:            return 400;
        case ErrorCode::MfaRecoveryUsed:        return 410;
        case ErrorCode::SessionExpired:         return 401;
        case ErrorCode::SessionNotFound:        return 401;
        case ErrorCode::SessionLimitReached:    return 429;
        case ErrorCode::ScimTokenInvalid:       return 401;
        case ErrorCode::RateLimitExceeded:      return 429;
        case ErrorCode::QuotaExceeded:          return 429;
        case ErrorCode::AbuseDetected:          return 429;
        case ErrorCode::CostLimitExceeded:      return 429;
        case ErrorCode::ModalityQuotaExceeded:  return 429;
        case ErrorCode::InjectionDetected:      return 403;
        case ErrorCode::PiiBlocked:             return 403;
        case ErrorCode::TopicViolation:         return 403;
        case ErrorCode::ContentFiltered:        return 403;
        case ErrorCode::EncodingAttack:         return 403;
        case ErrorCode::NoModelAvailable:       return 503;
        case ErrorCode::CircuitBreakerOpen:     return 503;
        case ErrorCode::UpstreamTimeout:        return 504;
        case ErrorCode::UpstreamError:          return 502;
        case ErrorCode::NoHealthyKeys:          return 503;
        case ErrorCode::ModelNotAllowed:        return 403;
        case ErrorCode::UnsupportedEndpoint:    return 404;
        case ErrorCode::InvalidRequest:         return 400;
        case ErrorCode::PayloadTooLarge:        return 413;
        case ErrorCode::MissingRequiredField:   return 400;
        case ErrorCode::TenantNameExists:       return 409;
        case ErrorCode::UsernameExists:         return 409;
        case ErrorCode::BudgetExceeded:         return 429;
        case ErrorCode::AutonomyDisabled:       return 503;
        case ErrorCode::ApprovalNotFound:       return 404;
        case ErrorCode::ApprovalStateInvalid:   return 409;
        case ErrorCode::PayloadTampered:        return 422;
        case ErrorCode::NotInitialized:         return 503;
        case ErrorCode::InternalError:          return 500;
        case ErrorCode::CacheUnavailable:       return 503;
    }
    return 500;
}

inline const char* toDefaultMessage(ErrorCode code) {
    switch (code) {
        case ErrorCode::InvalidApiKey:          return "Invalid or missing API key";
        case ErrorCode::InsufficientPermissions: return "Insufficient permissions";
        case ErrorCode::InvalidAdminKey:        return "Invalid or missing admin API key";
        case ErrorCode::ApiKeyExpired:          return "API key has expired";
        case ErrorCode::ApiKeyRevoked:          return "API key has been revoked";
        case ErrorCode::AccountDisabled:        return "User account is disabled";
        case ErrorCode::TenantSuspended:        return "Tenant has been suspended";
        case ErrorCode::CrossTenantDenied:      return "Cross-tenant access denied";
        case ErrorCode::SsoConfigMissing:       return "SSO not configured for this tenant";
        case ErrorCode::SsoCallbackInvalid:     return "Invalid SSO callback parameters";
        case ErrorCode::SsoTokenInvalid:        return "SSO token signature verification failed";
        case ErrorCode::SsoProviderError:       return "SSO identity provider communication error";
        case ErrorCode::SsoAccountNotMapped:    return "External identity not mapped to a local account";
        case ErrorCode::MfaRequired:            return "Multi-factor authentication required";
        case ErrorCode::MfaInvalidCode:         return "Invalid MFA verification code";
        case ErrorCode::MfaNotSetup:            return "MFA has not been set up for this account";
        case ErrorCode::MfaRecoveryUsed:        return "Recovery code already used";
        case ErrorCode::SessionExpired:         return "Session has expired";
        case ErrorCode::SessionNotFound:        return "Session not found";
        case ErrorCode::SessionLimitReached:    return "Maximum concurrent sessions reached";
        case ErrorCode::ScimTokenInvalid:       return "Invalid or expired SCIM token";
        case ErrorCode::RateLimitExceeded:      return "Rate limit exceeded";
        case ErrorCode::QuotaExceeded:          return "Quota exceeded";
        case ErrorCode::AbuseDetected:          return "Temporarily blocked due to repeated security violations";
        case ErrorCode::CostLimitExceeded:      return "Tenant cost limit exceeded";
        case ErrorCode::ModalityQuotaExceeded:  return "Modality quota exceeded";
        case ErrorCode::InjectionDetected:      return "Request blocked: injection attack detected";
        case ErrorCode::PiiBlocked:             return "Request blocked: sensitive information detected";
        case ErrorCode::TopicViolation:         return "Request blocked: topic not allowed";
        case ErrorCode::ContentFiltered:        return "Response blocked by output guardrail";
        case ErrorCode::EncodingAttack:         return "Request blocked: encoding attack detected";
        case ErrorCode::NoModelAvailable:       return "No model available for routing";
        case ErrorCode::CircuitBreakerOpen:     return "Service temporarily unavailable (circuit breaker open)";
        case ErrorCode::UpstreamTimeout:        return "Upstream model request timed out";
        case ErrorCode::UpstreamError:          return "All upstream models failed";
        case ErrorCode::NoHealthyKeys:          return "No healthy API keys available for the selected provider";
        case ErrorCode::ModelNotAllowed:        return "Model not in tenant whitelist";
        case ErrorCode::UnsupportedEndpoint:    return "Endpoint not supported by the target provider";
        case ErrorCode::InvalidRequest:         return "Invalid request body";
        case ErrorCode::PayloadTooLarge:        return "Request body exceeds maximum allowed size";
        case ErrorCode::MissingRequiredField:   return "Required field is missing";
        case ErrorCode::TenantNameExists:       return "Tenant name already exists";
        case ErrorCode::UsernameExists:         return "Username already exists in this tenant";
        case ErrorCode::BudgetExceeded:         return "Tenant budget exceeded; request downgraded to economy tier";
        case ErrorCode::AutonomyDisabled:       return "Autonomy workflow is disabled via kill switch or config";
        case ErrorCode::ApprovalNotFound:       return "Approval proposal not found";
        case ErrorCode::ApprovalStateInvalid:   return "Approval state transition not allowed from current state";
        case ErrorCode::PayloadTampered:        return "Approval payload integrity check failed (SHA-256 mismatch)";
        case ErrorCode::NotInitialized:         return "Gateway not initialized";
        case ErrorCode::InternalError:          return "Internal server error";
        case ErrorCode::CacheUnavailable:       return "Semantic cache is not enabled";
    }
    return "Unknown error";
}

inline std::string toDocUrl(ErrorCode code) {
    return "https://aegisgate.dev/docs/errors#" + toAegisCode(code);
}

// P0-3: thrown by a connector when its API-key pool is exhausted (all distinct
// healthy keys tried and failed within a single request). Carries a distinct
// type so FallbackManager / GatewayRuntime can surface AEGIS-4007 (503) instead
// of collapsing the condition into a generic 502 UpstreamError.
class NoHealthyKeysError : public std::runtime_error {
public:
    explicit NoHealthyKeysError(const std::string& msg)
        : std::runtime_error(msg) {}
};

// P1-A: thrown when an upstream returns a definitive (non-200) HTTP status.
// Carries the original upstream status so FallbackManager / GatewayRuntime can
// surface the real condition (429 back-off, 5xx unavailable, 4xx client error,
// 408/504 timeout) to the caller instead of collapsing everything into 502.
class UpstreamStatusError : public std::runtime_error {
public:
    UpstreamStatusError(int upstream_status, const std::string& msg)
        : std::runtime_error(msg), upstream_status_(upstream_status) {}
    int upstreamStatus() const { return upstream_status_; }
private:
    int upstream_status_;
};

// P1-A: thrown by FallbackManager when every routing candidate was skipped
// because its circuit breaker is open (nothing was actually attempted) — a
// distinct condition from "attempted and failed". Surfaces AEGIS-4002 (503).
class CircuitBreakerOpenError : public std::runtime_error {
public:
    explicit CircuitBreakerOpenError(const std::string& msg)
        : std::runtime_error(msg) {}
};

// P1-A: map an upstream HTTP status to the gateway-facing (http_status, code)
// pair. 408/504 are timeouts (504 / UpstreamTimeout); everything else is passed
// through verbatim under the generic UpstreamError code so callers see the real
// status (429 → 429, 503 → 503, 4xx → 4xx). Defends against bogus < 400 inputs.
struct UpstreamMapping { int http_status; ErrorCode code; };
inline UpstreamMapping mapUpstreamStatus(int upstream_status) {
    if (upstream_status == 408 || upstream_status == 504)
        return {504, ErrorCode::UpstreamTimeout};
    if (upstream_status < 400 || upstream_status > 599)
        return {502, ErrorCode::UpstreamError};
    return {upstream_status, ErrorCode::UpstreamError};
}

} // namespace aegisgate
