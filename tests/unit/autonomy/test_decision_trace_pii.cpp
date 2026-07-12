// Phase 11.5 TASK-20260518-02 Epic 1.6 — decision_trace PII fallback tests.
//
// Plan §D Task 1.6 — 3 mandatory scenarios:
//   1. decision_trace 含 email → 入库后 masked
//   2. decision_trace 含 phone → 入库后 masked
//   3. decision_trace 含 ssn (custom pattern) → audit 写入时 masked
//
// All three drive AutonomyApprovalWorkflow::propose() so the test pins the
// production code path (E1.5 wired the call to maskDecisionTraceInPlace).
//
// T07 mitigation — defence in depth even if upstream callers forget to mask.

#include "observe/autonomy/approval_workflow.h"

#include "guardrail/audit.h"
#include "guardrail/inbound/pii_filter.h"
#include "observe/autonomy/approval_queue.h"
#include "storage/memory_persistent_store.h"

#include <gtest/gtest.h>
#include <memory>

using namespace aegisgate;
using namespace aegisgate::autonomy;

namespace {

ApprovalProposal makePropWithTraceNotes(const std::string& notes) {
    ApprovalProposal p;
    p.source         = AutonomySource::CostOptimizer;
    p.subject        = "pii-fallback";
    p.payload        = nlohmann::json{{"k", "v"}};
    p.decision_trace = nlohmann::json{
        {"source_id",         "cost_optimizer"},
        {"algorithm_name",    "v1"},
        {"input_hash_sha256", std::string(64, 'a')},
        {"proposed_at_ms",    1716030000000LL},
        {"notes",             notes}};
    return p;
}

struct PiiFixture {
    std::shared_ptr<MemoryPersistentStore> store;
    std::shared_ptr<ApprovalQueue>         queue;
    std::shared_ptr<AuditLogger>           audit;
    std::shared_ptr<PIIFilter>             filter;
    std::shared_ptr<AutonomyApprovalWorkflow> wf;

    PiiFixture(std::shared_ptr<PIIFilter> f = nullptr)
        : store(std::make_shared<MemoryPersistentStore>()),
          audit(std::make_shared<AuditLogger>()),
          filter(std::move(f)) {
        store->initialize();
        queue = std::make_shared<ApprovalQueue>(store.get());
        queue->initialize();
        wf = std::make_shared<AutonomyApprovalWorkflow>(queue, audit, filter);
        wf->setAutonomyEnabledOverride(true);
    }
};

} // namespace

TEST(DecisionTracePiiTest, EmailMaskedInStorage) {
    PiiFixture f;  // default PIIFilter (includes "email")
    auto id = f.wf->propose(makePropWithTraceNotes(
        "operator alice@example.com requested cost cut"));
    ASSERT_FALSE(id.empty());

    auto stored = f.queue->get(id);
    ASSERT_TRUE(stored.has_value());
    const auto notes = stored->decision_trace.value("notes", std::string{});
    EXPECT_NE(notes.find("[EMAIL]"), std::string::npos)
        << "expected [EMAIL] placeholder, got: " << notes;
    EXPECT_EQ(notes.find("alice@example.com"), std::string::npos)
        << "raw email must not survive the propose path";
}

TEST(DecisionTracePiiTest, PhoneMaskedInStorage) {
    PiiFixture f;
    auto id = f.wf->propose(makePropWithTraceNotes(
        "alert dispatched to 13800138000 for review"));
    ASSERT_FALSE(id.empty());

    auto stored = f.queue->get(id);
    ASSERT_TRUE(stored.has_value());
    const auto notes = stored->decision_trace.value("notes", std::string{});
    EXPECT_NE(notes.find("[PHONE]"), std::string::npos)
        << "expected [PHONE] placeholder, got: " << notes;
    EXPECT_EQ(notes.find("13800138000"), std::string::npos);
}

TEST(DecisionTracePiiTest, SsnMaskedInAuditLog) {
    // SSN isn't in the default pattern set; inject a US-style SSN regex.
    auto pii = std::make_shared<PIIFilter>();
    pii->addPattern("ssn", "\\b\\d{3}-\\d{2}-\\d{4}\\b", "[SSN]");
    PiiFixture f(pii);

    auto id = f.wf->propose(makePropWithTraceNotes(
        "ticket owner ssn 123-45-6789, please redact"));
    ASSERT_FALSE(id.empty());

    // Audit entry written under "propose" action must carry masked detail.
    auto entries = f.audit->entries();
    ASSERT_FALSE(entries.empty());
    bool found_masked = false;
    bool raw_leaked   = false;
    for (const auto& e : entries) {
        if (e.request_id != id) continue;
        if (e.detail.find("[SSN]") != std::string::npos)        found_masked = true;
        if (e.detail.find("123-45-6789") != std::string::npos)  raw_leaked   = true;
    }
    EXPECT_TRUE(found_masked) << "audit detail must contain [SSN]";
    EXPECT_FALSE(raw_leaked)  << "raw SSN must not appear in audit detail";

    // Also confirm the storage row is masked (defence in depth).
    auto stored = f.queue->get(id);
    ASSERT_TRUE(stored.has_value());
    const auto notes = stored->decision_trace.value("notes", std::string{});
    EXPECT_NE(notes.find("[SSN]"), std::string::npos);
    EXPECT_EQ(notes.find("123-45-6789"), std::string::npos);
}
