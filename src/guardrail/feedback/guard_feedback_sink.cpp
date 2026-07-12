#include "guardrail/feedback/guard_feedback_sink.h"

#include "aegisgate/feedback_event.h"
#include "guardrail/audit.h"
#include "guardrail/inbound/pii_filter.h"
#include "observe/feedback_bus.h"

#include <chrono>
#include <utility>

namespace aegisgate::guard {

GuardFeedbackSink::GuardFeedbackSink(GuardFeedbackSinkDeps deps)
    : deps_(std::move(deps)) {}

SinkIngestResult GuardFeedbackSink::ingest(const GuardFeedbackPayload& payload,
                                            const std::string& tenant_id) {
    if (!deps_.audit) {
        return {false, "audit_unavailable", "AuditLogger not wired"};
    }

    GuardFeedbackPayload masked = payload;
    if (deps_.pii) {
        masked.comment = deps_.pii->mask(masked.comment);
        masked.original_text_redacted =
            deps_.pii->mask(masked.original_text_redacted);
    }

    // SR5: full reviewer + label + masked text persisted via the chain-hashed
    // audit table. action keeps the stable taxonomy used by trace search.
    auto detail = masked.toJson();
    detail["tenant_id"] = tenant_id;
    deps_.audit->logAction(/*request_id=*/masked.request_id,
                            /*tenant_id=*/tenant_id,
                            /*stage=*/"AdaptiveGuard",
                            /*action=*/"guard_feedback",
                            /*detail=*/detail.dump());

    if (deps_.bus) {
        aegisgate::FeedbackEvent ev;
        ev.type = aegisgate::FeedbackEventType::GuardFeedback;
        ev.topic = "guard.feedback";
        ev.request_id = masked.request_id;
        ev.tenant_id = tenant_id;
        ev.source = "GuardFeedbackSink";
        ev.timestamp = std::chrono::system_clock::now();
        ev.payload = masked.toJson();
        deps_.bus->publish(std::move(ev));
    }

    return {true, {}, {}};
}

}  // namespace aegisgate::guard
