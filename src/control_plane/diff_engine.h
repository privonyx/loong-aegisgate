#pragma once

// Phase 9.3 Epic 3 Task 3.4 — unified-diff engine.
//
// MVP implementation delegates to system `diff -u` and returns the hunk body
// with per-file header lines stripped so the output is self-contained. If
// `diff` is unavailable (minimal container, Windows CI) the fallback is a
// naive line-by-line compare with `-`/`+` markers so listVersionsDiff never
// hard-fails.
//
// Future work: semantic-yaml tree diff (design §3, C1) plugs in behind the
// same interface; the gRPC layer negotiates format via DiffFormat and for the
// MVP unconditionally returns unified_diff + empty `changes`.

#include <string>

namespace aegisgate {

class DiffEngine {
public:
    DiffEngine() = default;

    // Produces a unified diff of `from` vs `to`. Returns an empty string when
    // the inputs are byte-identical. Never throws; any system failure falls
    // back to the naive in-process implementation.
    std::string unifiedDiff(const std::string& from,
                            const std::string& to) const;

private:
    std::string runSystemDiff(const std::string& from,
                              const std::string& to) const;
    std::string naiveDiff(const std::string& from,
                          const std::string& to) const;
};

} // namespace aegisgate
