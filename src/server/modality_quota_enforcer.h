#pragma once
// Phase 6.1 Epic 5.1c (B1, TASK-20260515-01).
//
// Pure helper extracted from GatewayRuntime::processProxyRequest so the
// per-modality quota enforcement path can be unit-tested without spinning
// up Drogon's event loop (A11 framework-glue TDD pattern).
//
// Contract:
//   - Returns std::nullopt → request is allowed (or no enforcement applies)
//   - Returns GatewayError(429, AEGIS-2005) → request must be blocked
//
// Side effects:
//   - On block: increments MetricsRegistry::modalityRateLimitedTotal()
//     with label {"modality", modalityToString(m)}
//   - No I/O, no logging — caller is responsible for audit
//
// Fail-open semantics (SR-NEW4 in spec):
//   - limiter == nullptr            → allow (no enforcement configured)
//   - !limiter->hasQuota(m)         → allow (no quota for this modality)
//   - m == Modality::Unknown        → allow (legacy/unmapped endpoint)

#include "aegisgate/types.h"
#include "multimodal/modality.h"
#include <optional>
#include <string>

namespace aegisgate {

class ModalityRateLimiter;
class MetricsRegistry;

std::optional<GatewayError> enforceModalityQuota(
    Modality m,
    const std::string& identity,
    ModalityRateLimiter* limiter,
    MetricsRegistry& metrics);

} // namespace aegisgate
