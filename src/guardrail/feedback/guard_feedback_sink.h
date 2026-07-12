#pragma once

// Phase 11.1 TASK-20260523-01 — Epic 2.2 GuardFeedbackSink.
//
// One-stop ingestion path for validated GuardFeedbackPayload records:
//   1. PIIFilter::mask() applied to free-text fields (SR2 dependency)
//   2. AuditLogger::logAction with action="guard_feedback" (SR5 dependency)
//   3. FeedbackBus::publish under topic="guard.feedback" so the trainer
//      and any observer subscribers can react.
//
// Failure modes are surfaced as structured SinkIngestResult so the admin
// endpoint can map error_code -> HTTP status.

#include "guardrail/feedback/guard_feedback_payload.h"

#include <memory>
#include <string>

namespace aegisgate {
class PIIFilter;
class AuditLogger;
class FeedbackBus;
}  // namespace aegisgate

namespace aegisgate::guard {

struct GuardFeedbackSinkDeps {
    std::shared_ptr<aegisgate::PIIFilter> pii;
    std::shared_ptr<aegisgate::AuditLogger> audit;
    std::shared_ptr<aegisgate::FeedbackBus> bus;
};

struct SinkIngestResult {
    bool ok = false;
    std::string error_code;
    std::string detail;
};

class GuardFeedbackSink {
public:
    explicit GuardFeedbackSink(GuardFeedbackSinkDeps deps);

    // Ingest one validated payload. `tenant_id` is sourced from the
    // authenticated session, NOT from the payload (defense against an
    // attacker forging a different tenant's ID — D4=C / T05).
    SinkIngestResult ingest(const GuardFeedbackPayload& payload,
                            const std::string& tenant_id);

private:
    GuardFeedbackSinkDeps deps_;
};

}  // namespace aegisgate::guard
