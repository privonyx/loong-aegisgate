#include "observe/autonomy/approval_proposal.h"

#include "core/crypto.h"
#include "guardrail/inbound/pii_filter.h"

#include <map>
#include <nlohmann/json.hpp>

namespace aegisgate::autonomy {

namespace {

// Canonicalise a JSON value so that two semantically equal payloads always
// hash to the same digest regardless of insertion order. Object keys are
// sorted lexicographically; arrays preserve order (semantics depend on it).
nlohmann::json canonicalise(const nlohmann::json& v) {
    using json = nlohmann::json;
    if (v.is_object()) {
        std::map<std::string, json> sorted;
        for (auto it = v.begin(); it != v.end(); ++it) {
            sorted.emplace(it.key(), canonicalise(it.value()));
        }
        json out = json::object();
        for (auto& kv : sorted) out[kv.first] = std::move(kv.second);
        return out;
    }
    if (v.is_array()) {
        json out = json::array();
        for (const auto& el : v) out.push_back(canonicalise(el));
        return out;
    }
    return v;
}

} // namespace

// --- Record bridge ---------------------------------------------------------

ApprovalProposalRecord toRecord(const ApprovalProposal& p) {
    ApprovalProposalRecord r;
    r.id                  = p.id;
    r.source              = toString(p.source);
    r.subject             = p.subject;
    r.payload_json        = p.payload.dump();
    r.decision_trace_json = p.decision_trace.dump();
    r.proposed_at_ms      = p.proposed_at_ms;
    r.proposer_user_id    = p.proposer_user_id;
    r.state               = toString(p.state);
    r.reviewer_user_id    = p.reviewer_user_id;
    r.reviewed_at_ms      = p.reviewed_at_ms;
    r.reject_reason       = p.reject_reason;
    r.payload_sha256      = p.payload_sha256;
    return r;
}

std::optional<ApprovalProposal> fromRecord(const ApprovalProposalRecord& rec) {
    auto src = autonomySourceFromString(rec.source);
    if (!src) return std::nullopt;
    auto st = approvalStateFromString(rec.state);
    if (!st) return std::nullopt;

    ApprovalProposal p;
    p.id                  = rec.id;
    p.source              = *src;
    p.subject             = rec.subject;
    // payload / decision_trace tolerate empty strings by defaulting to {} —
    // SQLite/PG schemas declare DEFAULT '{}' so this only matters for
    // hand-crafted records.
    try {
        p.payload = rec.payload_json.empty()
                      ? nlohmann::json::object()
                      : nlohmann::json::parse(rec.payload_json);
        p.decision_trace = rec.decision_trace_json.empty()
                             ? nlohmann::json::object()
                             : nlohmann::json::parse(rec.decision_trace_json);
    } catch (const nlohmann::json::parse_error&) {
        return std::nullopt;
    }
    p.proposed_at_ms      = rec.proposed_at_ms;
    p.proposer_user_id    = rec.proposer_user_id;
    p.state               = *st;
    p.reviewer_user_id    = rec.reviewer_user_id;
    p.reviewed_at_ms      = rec.reviewed_at_ms;
    p.reject_reason       = rec.reject_reason;
    p.payload_sha256      = rec.payload_sha256;
    return p;
}

// --- payload_sha256 helpers ------------------------------------------------

std::string computePayloadSha256(const nlohmann::json& payload) {
    return crypto::sha256(canonicalise(payload).dump());
}

bool verifyPayloadSha256(const ApprovalProposal& p) {
    if (p.payload_sha256.empty()) return false;
    return crypto::constantTimeEquals(p.payload_sha256,
                                       computePayloadSha256(p.payload));
}

// --- decision_trace helpers ------------------------------------------------

namespace {
bool isStringField(const nlohmann::json& v) { return v.is_string(); }
bool isInt64Field(const nlohmann::json& v) {
    return v.is_number_integer() || v.is_number_unsigned();
}
} // namespace

DecisionTraceValidation validateDecisionTrace(const nlohmann::json& trace) {
    DecisionTraceValidation r;
    if (!trace.is_object()) {
        r.ok = false;
        for (auto f : kDecisionTraceRequiredFields)
            r.missing_fields.emplace_back(f);
        return r;
    }

    // Required string fields (first three) + required int field.
    for (auto f : {std::string_view{"source_id"},
                   std::string_view{"algorithm_name"},
                   std::string_view{"input_hash_sha256"}}) {
        auto it = trace.find(f);
        if (it == trace.end()) {
            r.missing_fields.emplace_back(f);
        } else if (!isStringField(*it) ||
                   it->get_ref<const std::string&>().empty()) {
            r.wrong_type_fields.emplace_back(f);
        }
    }
    {
        auto it = trace.find("proposed_at_ms");
        if (it == trace.end()) {
            r.missing_fields.emplace_back("proposed_at_ms");
        } else if (!isInt64Field(*it)) {
            r.wrong_type_fields.emplace_back("proposed_at_ms");
        }
    }
    // notes is optional, but if present must be string.
    {
        auto it = trace.find(kDecisionTraceOptionalNotes);
        if (it != trace.end() && !isStringField(*it)) {
            r.wrong_type_fields.emplace_back(std::string(kDecisionTraceOptionalNotes));
        }
    }
    r.ok = r.missing_fields.empty() && r.wrong_type_fields.empty();
    return r;
}

void maskDecisionTraceInPlace(nlohmann::json& trace, const PIIFilter* filter) {
    if (!filter || !trace.is_object()) return;
    for (auto it = trace.begin(); it != trace.end(); ++it) {
        if (it->is_string()) {
            *it = filter->mask(it->get<std::string>());
        }
    }
}

} // namespace aegisgate::autonomy
