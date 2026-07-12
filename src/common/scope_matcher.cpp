#include "common/scope_matcher.h"

namespace aegisgate::common {

namespace {

// FNV-1a 64-bit reference constants.
constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime       = 1099511628211ULL;

// Returns true if `glob` is a trailing-"*" prefix-match glob.
bool isPrefixGlob(const std::string& g, std::string_view& prefix) noexcept {
    if (!g.empty() && g.back() == '*') {
        prefix = std::string_view(g.data(), g.size() - 1);
        return true;
    }
    return false;
}

} // namespace

std::uint64_t fnv1a64(std::string_view s) noexcept {
    std::uint64_t h = kFnvOffsetBasis;
    for (unsigned char c : s) {
        h ^= static_cast<std::uint64_t>(c);
        h *= kFnvPrime;
    }
    return h;
}

bool tenantMatches(const std::vector<std::string>& globs,
                    std::string_view tenant_id) noexcept {
    if (globs.empty()) return true;
    for (const auto& g : globs) {
        if (g == "*") return true;
        std::string_view prefix;
        if (isPrefixGlob(g, prefix)) {
            if (tenant_id.size() >= prefix.size() &&
                tenant_id.substr(0, prefix.size()) == prefix) {
                return true;
            }
            continue;
        }
        if (std::string_view(g) == tenant_id) return true;
    }
    return false;
}

bool regionMatches(const std::vector<std::string>& regions,
                    std::string_view region) noexcept {
    if (regions.empty()) return true;
    for (const auto& r : regions) {
        if (std::string_view(r) == region) return true;
    }
    return false;
}

bool percentageMatches(std::string_view sticky_value, int percentage) noexcept {
    if (percentage <= 0) return false;
    if (percentage >= 100) return true;
    if (sticky_value.empty()) return false;
    return (fnv1a64(sticky_value) % 100ULL) <
           static_cast<std::uint64_t>(percentage);
}

bool matches(const std::vector<std::string>& tenant_globs,
             const std::vector<std::string>& regions,
             int percentage,
             const ScopeContext& ctx) noexcept {
    return tenantMatches(tenant_globs, ctx.tenant_id)
        && regionMatches(regions, ctx.region)
        && percentageMatches(ctx.sticky_value, percentage);
}

} // namespace aegisgate::common
