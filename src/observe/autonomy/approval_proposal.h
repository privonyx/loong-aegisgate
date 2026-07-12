#pragma once

// Phase 11.5 AutonomyApprovalWorkflow (TASK-20260518-02) — Epic 1.2.
//
// ApprovalProposal — the canonical in-memory record passed between
// CostOptimizer / BudgetGuardStage / AutonomyApprovalWorkflow / Admin API.
// It mirrors aegisgate::ApprovalProposalRecord one-to-one but carries the
// rich types (enums + nlohmann::json) the runtime cares about.
//
// Authoritative wire schema lives in approval_proposal_record.h
// (src/storage/) for the persistence layer; this header owns the runtime
// view. The two are bridged by toRecord() / fromRecord() so that the
// PersistentStore base header (and anything that includes it) does NOT
// have to pull in nlohmann::json.
//
// Design references:
//   docs/specs/2026-05-18-phase11.5-cost-autonomy-design.md §3.2 / §4.2
//   memory-bank/creative/creative-phase11.5-cost-autonomy.md §1 / §2

#include "observe/autonomy/approval_state.h"
#include "storage/approval_proposal_record.h"

#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aegisgate {
class PIIFilter;  // forward-declared; only needed at call sites that mask.
}

namespace aegisgate::autonomy {

// Plan §3.2 decision_trace required field set. notes is optional.
inline constexpr std::array<std::string_view, 4>
    kDecisionTraceRequiredFields = {
        "source_id", "algorithm_name", "input_hash_sha256", "proposed_at_ms",
    };
inline constexpr std::string_view kDecisionTraceOptionalNotes = "notes";

struct ApprovalProposal {
    std::string                    id;
    AutonomySource                 source = AutonomySource::CostOptimizer;
    std::string                    subject;
    nlohmann::json                 payload          = nlohmann::json::object();
    nlohmann::json                 decision_trace   = nlohmann::json::object();
    std::int64_t                   proposed_at_ms   = 0;
    std::string                    proposer_user_id = "system";
    ApprovalState                  state            = ApprovalState::PROPOSED;
    std::string                    reviewer_user_id;
    std::int64_t                   reviewed_at_ms   = 0;
    std::string                    reject_reason;
    std::string                    payload_sha256;
};

// --- Bridge to/from POCO record (storage layer) ----------------------------

// Construct a POCO suitable for PersistentStore::insertApprovalProposal.
ApprovalProposalRecord toRecord(const ApprovalProposal& p);

// Reverse direction; returns std::nullopt when the record carries an enum
// string the runtime no longer recognises (forward-compat guard).
std::optional<ApprovalProposal> fromRecord(const ApprovalProposalRecord& rec);

// --- payload_sha256 helpers ------------------------------------------------

// Stable hex-encoded sha256 over a canonical-form dump of `payload`.
// Same input -> same hash on every machine; nlohmann::json::dump produces
// the keys in insertion order, so we sort and re-dump first.
std::string computePayloadSha256(const nlohmann::json& payload);

// True iff p.payload_sha256 matches computePayloadSha256(p.payload). Used
// by AutonomyApprovalWorkflow::apply() to defend against T01 tampering of
// payload between propose and apply.
bool verifyPayloadSha256(const ApprovalProposal& p);

// --- decision_trace helpers ------------------------------------------------

struct DecisionTraceValidation {
    bool                     ok = false;
    std::vector<std::string> missing_fields;
    std::vector<std::string> wrong_type_fields;
};

// Validate that decision_trace contains the 4 required fields with the
// expected JSON types. notes is optional but if present must be a string.
DecisionTraceValidation validateDecisionTrace(const nlohmann::json& trace);

// Mutate `trace` in place applying PIIFilter::mask to every string value
// (top-level for now; design §3.2 keeps decision_trace shallow on purpose).
// No-op when filter == nullptr so call sites can keep the dependency
// optional.
void maskDecisionTraceInPlace(nlohmann::json& trace, const PIIFilter* filter);

} // namespace aegisgate::autonomy
