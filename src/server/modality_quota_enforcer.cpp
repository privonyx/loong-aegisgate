#include "server/modality_quota_enforcer.h"

#include "aegisgate/error_codes.h"
#include "multimodal/modality_rate_limiter.h"
#include "observe/metrics.h"

namespace aegisgate {

std::optional<GatewayError> enforceModalityQuota(
    Modality m,
    const std::string& identity,
    ModalityRateLimiter* limiter,
    MetricsRegistry& metrics) {
    // SR-NEW4: fail-open in three explicit cases.
    if (limiter == nullptr) return std::nullopt;
    if (m == Modality::Unknown) return std::nullopt;
    if (!limiter->hasQuota(m)) return std::nullopt;

    if (limiter->allow(m, identity, /*cost=*/1.0)) {
        return std::nullopt;
    }

    // Blocked: bump the per-modality counter (D3) and return AEGIS-2005.
    LabelSet label;
    label.labels = {{"modality", modalityToString(m)}};
    metrics.modalityRateLimitedTotal().inc(label);

    return GatewayError{
        /*http_status=*/toHttpStatus(ErrorCode::ModalityQuotaExceeded),
        /*error_code=*/toAegisCode(ErrorCode::ModalityQuotaExceeded),
        /*error_type=*/toErrorType(ErrorCode::ModalityQuotaExceeded),
        /*message=*/toDefaultMessage(ErrorCode::ModalityQuotaExceeded),
        /*internal_detail=*/"modality=" + modalityToString(m) +
                            " identity=" + identity,
    };
}

} // namespace aegisgate
