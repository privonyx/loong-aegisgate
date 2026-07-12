#pragma once

// TASK-20260518-02 Phase 11.5 — Epic 1.0
//
// ApprovalProposalRecord — POCO mirror of AutonomyApprovalWorkflow proposals
// for the persistence layer. Lives under src/storage/ (sibling of
// rollout_record.h, but co-located with PersistentStore) because the
// approval workflow is a cross-cutting infrastructure used by 5 future
// Phase 11 sub-phases (11.1/11.2/11.3/11.4/11.5).
//
// Fields stay as plain strings / int64_t so this header has zero
// dependency on nlohmann::json / spdlog / enum types. The richer
// in-memory model lives in src/observe/autonomy/approval_proposal.h
// (created in Epic 1.2) and translates back-and-forth.
//
// Schema text (CREATE TABLE / INDEX) lives in approval_proposal_schema.h,
// kept separate so the POCO header stays SQL-free.

#include <cstdint>
#include <string>
#include <vector>

namespace aegisgate {

struct ApprovalProposalRecord {
    std::string id;                    // ULID (26 chars)
    std::string source;                // toString(AutonomySource): "CostOptimizer" / "AutoRecovery" / ...
    std::string subject;               // human-readable summary, <= 200 chars
    std::string payload_json;          // apply 时读取的具体变更（JSON-encoded）
    std::string decision_trace_json;   // C4 5 字段（JSON-encoded），PIIFilter 兜底后入库
    std::int64_t proposed_at_ms = 0;
    std::string proposer_user_id;      // 通常 "system"
    std::string state;                 // toString(ApprovalState): "PROPOSED" / "APPROVED" / ...
    std::string reviewer_user_id;      // "manual:<user_id>" / "auto:low_risk" / "auto:all"
    std::int64_t reviewed_at_ms = 0;
    std::string reject_reason;
    std::string payload_sha256;        // propose 时计算，apply 时校验（防 T01 篡改）
};

// Query parameters for listApprovalProposals. Empty filters = unbounded.
struct ApprovalProposalQuery {
    std::string state_filter;      // empty = every state
    std::string source_filter;     // empty = every source
    int         limit  = 100;      // server-side caps at 1000
    int         offset = 0;
};

} // namespace aegisgate
