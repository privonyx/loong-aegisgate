// Phase 11.5 TASK-20260518-02 Epic 1.2 — ApprovalProposal tests.
//
// Coverage:
//   * round-trip ApprovalProposal <-> ApprovalProposalRecord (storage bridge)
//   * computePayloadSha256 deterministic + order-independent
//   * verifyPayloadSha256 catches T01 tampering
//   * decision_trace 5-field validation (required + optional `notes`)
//   * PIIFilter mask-in-place applies to top-level string values only

#include "observe/autonomy/approval_proposal.h"

#include "guardrail/inbound/pii_filter.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace aegisgate;
using namespace aegisgate::autonomy;
using nlohmann::json;

namespace {

ApprovalProposal makeFixture() {
    ApprovalProposal p;
    p.id              = "01HNPROP000000000000000A1";
    p.source          = AutonomySource::CostOptimizer;
    p.subject         = "Downgrade tenant t1 from premium to standard";
    p.payload         = json{{"tenant_id", "t1"},
                              {"from", "premium"},
                              {"to", "standard"}};
    p.decision_trace  = json{{"source_id", "cost_optimizer"},
                              {"algorithm_name", "cheapest_alternative_v1"},
                              {"input_hash_sha256",
                               "0123456789abcdef0123456789abcdef"
                               "0123456789abcdef0123456789abcdef"},
                              {"proposed_at_ms", 1716030000000LL},
                              {"notes", "Cost spike on tenant t1"}};
    p.proposed_at_ms  = 1716030000000LL;
    p.proposer_user_id = "system";
    p.state           = ApprovalState::PROPOSED;
    p.payload_sha256  = computePayloadSha256(p.payload);
    return p;
}

} // namespace

// ---------- round-trip ----------------------------------------------------

TEST(ApprovalProposalTest, RecordRoundtripPreservesAllFields) {
    auto p = makeFixture();
    auto back = fromRecord(toRecord(p));
    ASSERT_TRUE(back.has_value());

    EXPECT_EQ(back->id,                p.id);
    EXPECT_EQ(back->source,            p.source);
    EXPECT_EQ(back->subject,           p.subject);
    EXPECT_EQ(back->payload,           p.payload);
    EXPECT_EQ(back->decision_trace,    p.decision_trace);
    EXPECT_EQ(back->proposed_at_ms,    p.proposed_at_ms);
    EXPECT_EQ(back->proposer_user_id,  p.proposer_user_id);
    EXPECT_EQ(back->state,             p.state);
    EXPECT_EQ(back->payload_sha256,    p.payload_sha256);
}

TEST(ApprovalProposalTest, RecordWithUnknownEnumReturnsNullopt) {
    auto rec = toRecord(makeFixture());
    rec.source = "NewSubPhase";  // unknown to the runtime
    EXPECT_FALSE(fromRecord(rec).has_value());

    rec.source = "CostOptimizer";
    rec.state  = "ZOMBIE";
    EXPECT_FALSE(fromRecord(rec).has_value());
}

TEST(ApprovalProposalTest, RecordWithMalformedJsonReturnsNullopt) {
    auto rec = toRecord(makeFixture());
    rec.payload_json = "{not-json";
    EXPECT_FALSE(fromRecord(rec).has_value());
}

// ---------- sha256 --------------------------------------------------------

TEST(ApprovalProposalTest, Sha256IsDeterministicAcrossKeyOrder) {
    json a = {{"a", 1}, {"b", "two"}, {"c", json::array({1, 2, 3})}};
    json b = {{"c", json::array({1, 2, 3})}, {"a", 1}, {"b", "two"}};
    EXPECT_EQ(computePayloadSha256(a), computePayloadSha256(b));
}

TEST(ApprovalProposalTest, Sha256DiffersWhenContentChanges) {
    json a = {{"tier", "premium"}};
    json b = {{"tier", "standard"}};
    EXPECT_NE(computePayloadSha256(a), computePayloadSha256(b));
}

TEST(ApprovalProposalTest, VerifyPayloadSha256RejectsTampering) {
    auto p = makeFixture();
    EXPECT_TRUE(verifyPayloadSha256(p));

    // Simulate T01: attacker swaps the payload between propose and apply
    // but cannot re-mint payload_sha256 (server holds the only writer).
    p.payload["to"] = "premium";  // upgrade attempt
    EXPECT_FALSE(verifyPayloadSha256(p));
}

TEST(ApprovalProposalTest, VerifyPayloadSha256RejectsEmpty) {
    ApprovalProposal p;
    p.payload = json::object();
    p.payload_sha256.clear();  // never computed
    EXPECT_FALSE(verifyPayloadSha256(p));
}

// ---------- decision_trace validation -------------------------------------

TEST(ApprovalProposalTest, DecisionTraceAcceptsFullPayload) {
    auto v = validateDecisionTrace(makeFixture().decision_trace);
    EXPECT_TRUE(v.ok);
    EXPECT_TRUE(v.missing_fields.empty());
    EXPECT_TRUE(v.wrong_type_fields.empty());
}

TEST(ApprovalProposalTest, DecisionTraceAcceptsWithoutOptionalNotes) {
    auto p = makeFixture();
    p.decision_trace.erase("notes");
    auto v = validateDecisionTrace(p.decision_trace);
    EXPECT_TRUE(v.ok) << "notes is optional";
}

TEST(ApprovalProposalTest, DecisionTraceFlagsMissingRequired) {
    json trace = {{"source_id", "cost_optimizer"}};  // 3 of 4 missing
    auto v = validateDecisionTrace(trace);
    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.missing_fields.size(), 3u);
}

TEST(ApprovalProposalTest, DecisionTraceFlagsWrongType) {
    json trace = {{"source_id", 42},                      // int, want string
                  {"algorithm_name", "cheapest_v1"},
                  {"input_hash_sha256", "abcd"},
                  {"proposed_at_ms", "not-a-number"}};    // string, want int
    auto v = validateDecisionTrace(trace);
    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.wrong_type_fields.size(), 2u);
}

TEST(ApprovalProposalTest, DecisionTraceRejectsNonObject) {
    auto v = validateDecisionTrace(json::array({1, 2}));
    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.missing_fields.size(), 4u);
}

// ---------- PIIFilter mask in place ---------------------------------------

TEST(ApprovalProposalTest, MaskInPlaceRedactsTopLevelStringPii) {
    PIIFilter filter;  // ctor loads default patterns (incl. email / phone)
    json trace = {{"source_id", "cost_optimizer"},
                  {"algorithm_name", "v1"},
                  {"input_hash_sha256", "abcd"},
                  {"proposed_at_ms", 1716030000000LL},
                  {"notes", "Contact ops@example.com or +14155551234"}};
    maskDecisionTraceInPlace(trace, &filter);

    // Hard contract: identifiers must not appear verbatim post-mask.
    EXPECT_EQ(trace["notes"].get<std::string>().find("ops@example.com"),
              std::string::npos);
    EXPECT_EQ(trace["notes"].get<std::string>().find("+14155551234"),
              std::string::npos);
    // Non-PII strings stay intact.
    EXPECT_EQ(trace["algorithm_name"], "v1");
    // Numeric field untouched.
    EXPECT_EQ(trace["proposed_at_ms"], 1716030000000LL);
}

TEST(ApprovalProposalTest, MaskInPlaceWithNullFilterIsNoop) {
    json trace = {{"notes", "ops@example.com"}};
    maskDecisionTraceInPlace(trace, /*filter=*/nullptr);
    EXPECT_EQ(trace["notes"], "ops@example.com");
}
