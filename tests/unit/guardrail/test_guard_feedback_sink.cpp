// Phase 11.1 TASK-20260523-01 — Epic 2.2 GuardFeedbackSink tests.
//
// Asserts:
//   1. Sink masks PII before forwarding to AuditLogger + FeedbackBus.
//   2. AuditLogger records action="guard_feedback" with chain_hash continuity.
//   3. Bus publish emits topic="guard.feedback" with the (post-mask) payload.
//   4. Throws structured error when AuditLogger missing (SR5 dependency check).

#include "guardrail/audit.h"
#include "guardrail/feedback/guard_feedback_payload.h"
#include "guardrail/feedback/guard_feedback_sink.h"
#include "guardrail/inbound/pii_filter.h"
#include "observe/feedback_bus.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

using aegisgate::AuditLogger;
using aegisgate::FeedbackBus;
using aegisgate::FeedbackEvent;
using aegisgate::PIIFilter;
using aegisgate::guard::GuardFeedbackLabel;
using aegisgate::guard::GuardFeedbackPayload;
using aegisgate::guard::GuardFeedbackSink;
using aegisgate::guard::SinkIngestResult;

namespace {

GuardFeedbackPayload payloadWithSecret() {
    GuardFeedbackPayload p;
    p.request_id = "req-100";
    p.label = GuardFeedbackLabel::FalsePositive;
    p.reviewer_user_id = "user-1";
    p.reviewer_role = "security_admin";
    // Plaintext email — the default PIIFilter pattern set should mask it.
    p.comment = "Reach out to alice@example.com to confirm.";
    p.original_text_redacted = "Email bob@example.com about my SSN";
    return p;
}

std::shared_ptr<FeedbackBus> makeEnabledBus() {
    aegisgate::FeedbackBusConfig cfg;
    cfg.enabled = true;
    auto bus = std::make_shared<FeedbackBus>(cfg);
    bus->start();
    return bus;
}

}  // namespace

TEST(GuardFeedbackSinkTest, MasksPiiBeforeAudit) {
    auto pii = std::make_shared<PIIFilter>();
    auto audit = std::make_shared<AuditLogger>();
    auto bus = makeEnabledBus();

    GuardFeedbackSink sink({pii, audit, bus});
    auto p = payloadWithSecret();

    auto res = sink.ingest(p, "tenant-A");
    ASSERT_TRUE(res.ok) << res.error_code;

    audit->flush();
    auto entries = audit->entries();
    ASSERT_FALSE(entries.empty());
    const auto& last = entries.back();
    EXPECT_EQ(last.action, "guard_feedback");
    EXPECT_EQ(last.stage_name, "AdaptiveGuard");
    EXPECT_EQ(last.tenant_id, "tenant-A");
    EXPECT_EQ(last.detail.find("alice@example.com"), std::string::npos)
        << "raw email must NOT appear in audit detail. detail=" << last.detail;
    EXPECT_EQ(last.detail.find("bob@example.com"), std::string::npos)
        << "raw email must NOT appear in audit detail. detail=" << last.detail;
    bus->shutdown();
}

TEST(GuardFeedbackSinkTest, ChainHashAdvances) {
    auto pii = std::make_shared<PIIFilter>();
    auto audit = std::make_shared<AuditLogger>();
    auto bus = makeEnabledBus();

    GuardFeedbackSink sink({pii, audit, bus});

    auto p = payloadWithSecret();
    ASSERT_TRUE(sink.ingest(p, "tenant-A").ok);
    p.request_id = "req-101";
    ASSERT_TRUE(sink.ingest(p, "tenant-A").ok);
    audit->flush();

    auto entries = audit->entries();
    ASSERT_GE(entries.size(), 2u);
    // chain_hash must differ — each new entry depends on the previous hash.
    EXPECT_NE(entries[entries.size() - 1].chain_hash,
              entries[entries.size() - 2].chain_hash);
    bus->shutdown();
}

TEST(GuardFeedbackSinkTest, PublishesOnFeedbackBus) {
    auto pii = std::make_shared<PIIFilter>();
    auto audit = std::make_shared<AuditLogger>();
    auto bus = makeEnabledBus();

    std::atomic<int> received{0};
    std::string captured_topic;
    bus->subscribe(
        [&](const FeedbackEvent& ev) {
            captured_topic = ev.topic;
            received.fetch_add(1);
        },
        /*topic_filter=*/"guard.");

    GuardFeedbackSink sink({pii, audit, bus});
    ASSERT_TRUE(sink.ingest(payloadWithSecret(), "tenant-B").ok);
    bus->flush();

    EXPECT_EQ(received.load(), 1);
    EXPECT_EQ(captured_topic, "guard.feedback");
    bus->shutdown();
}

TEST(GuardFeedbackSinkTest, MissingAuditLoggerRejected) {
    auto pii = std::make_shared<PIIFilter>();
    auto bus = makeEnabledBus();

    GuardFeedbackSink sink({pii, /*audit=*/nullptr, bus});
    auto res = sink.ingest(payloadWithSecret(), "tenant-A");
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error_code, "audit_unavailable");
    bus->shutdown();
}
