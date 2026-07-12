#pragma once

// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic B.1.
//
// CR2 creative design — FNV-1a 64-bit + modulo 100:
//   memory-bank/creative/creative-rollout-scope-matcher.md
//
// Pure stateless matching API shared between control-plane rollout
// evaluation and data-plane per-request routing. The namespace lives
// under aegisgate::common (not aegisgate::) so the header can be
// included by both layers without leaking control-plane symbols to
// the data-plane hot path.
//
// Performance budget: single matches() call ≤ 5μs @ 10 globs + 10 regions.
// Thread-safety: all functions are pure and trivially thread-safe.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aegisgate::common {

// Three runtime inputs resolved per request. `sticky_value` is expected
// to already be picked by the caller using the rollout's `sticky_key`
// with the fallback chain documented in the creative doc §6
// (sticky_key → tenant_id → request_id → empty).
struct ScopeContext {
    std::string_view tenant_id;
    std::string_view region;
    std::string_view sticky_value;
};

// 64-bit FNV-1a. Exposed so tests (and future tooling) can pin the
// algorithm without peeking at internals.
std::uint64_t fnv1a64(std::string_view s) noexcept;

// Empty list → match-all. A glob of exactly "*" matches all; otherwise
// a trailing "*" in the glob is treated as prefix match; anything else
// is an exact match. Multiple globs OR together.
bool tenantMatches(const std::vector<std::string>& globs,
                    std::string_view tenant_id) noexcept;

// Empty list → match-all. Otherwise exact membership.
bool regionMatches(const std::vector<std::string>& regions,
                    std::string_view region) noexcept;

// Consistent-hash bucket test:
//   p ≤ 0 or empty sticky + 0<p<100 → false (conservative)
//   p ≥ 100 → true
//   otherwise fnv1a64(sticky) % 100 < p
// Monotonicity: for any sticky_value, if `p` matches, all `p' > p` also match.
bool percentageMatches(std::string_view sticky_value, int percentage) noexcept;

// Composite AND of the three predicates. Fields are passed individually
// (rather than via a struct reference) so callers don't need to adapt
// between control-plane POCO and data-plane field sources.
bool matches(const std::vector<std::string>& tenant_globs,
             const std::vector<std::string>& regions,
             int percentage,
             const ScopeContext& ctx) noexcept;

} // namespace aegisgate::common
