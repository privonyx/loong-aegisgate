#pragma once

// Phase 9.3 Epic 3 Task 3.3 — SensitiveScanner (SR4).
//
// Scans raw yaml text for high-confidence secret patterns before it is
// persisted. Findings are returned with enough context for the caller to emit
// a structured error (`SENSITIVE_FIELD_DETECTED`) without echoing the secret
// itself — the scanner never copies matched values.
//
// The MVP patterns live in sensitive_scanner.cpp and are conservative: we
// prefer occasional false positives (developer pastes a suspicious literal)
// over any chance of leaking a real credential into persistent storage.

#include <cstddef>
#include <string>
#include <vector>

namespace aegisgate {

struct SensitiveFinding {
    int         line = 0;          // 1-indexed
    int         column = 0;        // 1-indexed, points at start of value
    std::string field_name;        // e.g. "api_key"
    std::string rule_id;           // which pattern fired; informational only
};

class SensitiveScanner {
public:
    SensitiveScanner() = default;

    // Returns the findings in document order. Empty vector means "looks clean".
    // Never throws; regex failures fall back to no match.
    std::vector<SensitiveFinding> scan(const std::string& yaml_text) const;
};

} // namespace aegisgate
