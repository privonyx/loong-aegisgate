#include "gateway/geo_router.h"
#include "gateway/connector/registry.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace aegisgate {

namespace {

constexpr const char* kRegionTagPrefix = "region:";
constexpr const char* kExtraHeaders = "headers";
constexpr const char* kExtraClientIp = "client_ip";
constexpr const char* kExtraClientRegion = "client_region";
constexpr const char* kExtraResidency = "residency";
constexpr const char* kExtraGeoAllowed = "_geo_allowed_models";
constexpr const char* kExtraGeoClientRegion = "_geo_client_region";
constexpr const char* kExtraGeoSelectedRegion = "_geo_selected_region";
constexpr const char* kRegionUnknown = "unknown";

std::string toLower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string trim(std::string s) {
    auto notspace = [](unsigned char ch) { return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    return s;
}

std::optional<uint32_t> parseIpv4(const std::string& s) {
    std::array<uint32_t, 4> octets{};
    size_t idx = 0;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            if (i == start) return std::nullopt;
            if (idx >= 4) return std::nullopt;
            const auto piece = s.substr(start, i - start);
            if (piece.size() > 3) return std::nullopt;
            for (char c : piece) {
                if (!std::isdigit(static_cast<unsigned char>(c))) {
                    return std::nullopt;
                }
            }
            const auto v = std::stoul(piece);
            if (v > 255) return std::nullopt;
            octets[idx++] = static_cast<uint32_t>(v);
            start = i + 1;
        }
    }
    if (idx != 4) return std::nullopt;
    return (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
}

bool ipInCidrV4(uint32_t ip_be, const std::string& cidr) {
    const auto slash = cidr.find('/');
    if (slash == std::string::npos) return false;
    const auto net_str = cidr.substr(0, slash);
    const auto prefix_str = cidr.substr(slash + 1);
    auto net = parseIpv4(net_str);
    if (!net) return false;
    int prefix = 0;
    for (char c : prefix_str) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        prefix = prefix * 10 + (c - '0');
    }
    if (prefix_str.empty() || prefix < 0 || prefix > 32) return false;
    if (prefix == 0) return true;
    const uint32_t mask = prefix == 32 ? 0xFFFFFFFFu
                                       : static_cast<uint32_t>(~((1u << (32 - prefix)) - 1));
    return (ip_be & mask) == (*net & mask);
}

// Reads a string field from a nlohmann::json object, tolerating missing keys
// and non-string types.
std::string jsonFieldString(const nlohmann::json& j, const std::string& key) {
    if (!j.is_object() || !j.contains(key)) return "";
    const auto& v = j[key];
    if (v.is_string()) return v.get<std::string>();
    return "";
}

} // namespace

GeoConfig::Affinity GeoConfig::parseAffinity(const std::string& s) {
    const auto lc = toLower(trim(s));
    if (lc == "strict") return Affinity::Strict;
    if (lc == "any") return Affinity::Any;
    return Affinity::Prefer;
}

GeoRouter::GeoRouter(std::unique_ptr<Router> base, GeoConfig config)
    : base_(std::move(base)), config_(std::move(config)) {}

std::vector<std::string> GeoRouter::modelRegions(const ModelInfo& info) {
    std::vector<std::string> regions;
    const std::string prefix = kRegionTagPrefix;
    for (const auto& tag : info.tags) {
        if (tag.size() > prefix.size() &&
            tag.compare(0, prefix.size(), prefix) == 0) {
            regions.emplace_back(tag.substr(prefix.size()));
        }
    }
    return regions;
}

std::string GeoRouter::normalizeRegion(std::string region) const {
    region = toLower(trim(std::move(region)));
    auto it = config_.region_aliases.find(region);
    if (it != config_.region_aliases.end()) {
        region = toLower(trim(it->second));
    }
    return region;
}

std::string GeoRouter::lookupRegionByIp(const std::string& ip) const {
    const auto parsed = parseIpv4(ip);
    if (!parsed) return "";
    for (const auto& [cidr, region] : config_.ip_region_map) {
        if (ipInCidrV4(*parsed, cidr)) {
            return region;
        }
    }
    return "";
}

std::string GeoRouter::inferClientRegion(const RequestContext& ctx) const {
    const auto& extra = ctx.chat_request.extra;

    // 1 & 2: configured headers in declared order.
    if (extra.contains(kExtraHeaders) && extra[kExtraHeaders].is_object()) {
        const auto& headers = extra[kExtraHeaders];
        for (const auto& hname : config_.header_names) {
            const auto v = jsonFieldString(headers, hname);
            if (!v.empty()) {
                return normalizeRegion(v);
            }
        }
    }

    // 3: extra["client_region"] from SDK or admin overrides.
    const auto sdk_region = jsonFieldString(extra, kExtraClientRegion);
    if (!sdk_region.empty()) {
        return normalizeRegion(sdk_region);
    }

    // 4: IP → CIDR mapping.
    const auto client_ip = jsonFieldString(extra, kExtraClientIp);
    if (!client_ip.empty()) {
        const auto r = lookupRegionByIp(client_ip);
        if (!r.empty()) {
            return normalizeRegion(r);
        }
    }

    // 5: configured default.
    return normalizeRegion(config_.default_client_region);
}

bool GeoRouter::isResidencyStrict(const RequestContext& ctx) const {
    const auto res = jsonFieldString(ctx.chat_request.extra, kExtraResidency);
    return toLower(trim(res)) == "strict";
}

std::vector<std::string> GeoRouter::filterModelsByRegion(
    const ConnectorRegistry& registry,
    const std::string& client_region,
    bool residency_strict) const {
    std::vector<std::string> allowed;
    for (const auto& info : registry.allModelInfos()) {
        const auto regions = modelRegions(info);
        if (regions.empty()) {
            if (residency_strict) continue;   // residency strict: unregionalized excluded
            // Under Strict affinity (without residency), unregionalized models
            // are also excluded because we cannot prove compliance.
            if (!residency_strict && config_.affinity == GeoConfig::Affinity::Strict) {
                continue;
            }
            allowed.push_back(info.id);
            continue;
        }
        bool match = false;
        for (const auto& r : regions) {
            if (toLower(trim(r)) == client_region) { match = true; break; }
        }
        if (match) {
            allowed.push_back(info.id);
        }
    }
    return allowed;
}

bool GeoRouter::isAllowed(const std::string& model,
                          const std::vector<std::string>& allowed) {
    for (const auto& m : allowed) {
        if (m == model) return true;
    }
    return false;
}

std::string GeoRouter::regionOfModel(const ConnectorRegistry& registry,
                                     const std::string& model,
                                     const std::string& prefer_region) {
    const auto* info = registry.findModelInfo(model);
    if (!info) return "";
    const auto regions = modelRegions(*info);
    if (regions.empty()) return kRegionUnknown;
    for (const auto& r : regions) {
        if (r == prefer_region) return r;
    }
    return regions.front();
}

std::string GeoRouter::selectModel(RequestContext& ctx,
                                    const ConnectorRegistry& registry) {
    if (!config_.enabled) {
        return base_ ? base_->selectModel(ctx, registry) : std::string{};
    }
    if (!base_) {
        spdlog::error("GeoRouter: no base router configured");
        return "";
    }

    const auto client_region = inferClientRegion(ctx);
    ctx.chat_request.extra[kExtraGeoClientRegion] = client_region;

    const bool residency_strict = isResidencyStrict(ctx);
    const bool effective_strict =
        residency_strict || config_.affinity == GeoConfig::Affinity::Strict;

    auto allowed = filterModelsByRegion(registry, client_region, residency_strict);

    if (allowed.empty()) {
        if (effective_strict) {
            spdlog::warn("GeoRouter: strict policy with no candidates for region '{}'",
                         client_region);
            ctx.chat_request.extra[kExtraGeoAllowed] = nlohmann::json::array();
            return "";
        }
        // Prefer/Any: fall back to full candidate set.
        for (const auto& info : registry.allModelInfos()) {
            allowed.push_back(info.id);
        }
        spdlog::debug("GeoRouter: no regional match; prefer/any falling back ({} models)",
                      allowed.size());
    }

    ctx.chat_request.extra[kExtraGeoAllowed] = allowed;

    auto picked = base_->selectModel(ctx, registry);

    if (picked.empty()) {
        // base router returned empty (e.g. no default in empty registry).
        return picked;
    }

    if (effective_strict && !isAllowed(picked, allowed)) {
        spdlog::warn("GeoRouter: base returned '{}' outside allowed set; re-picking (strict)",
                     picked);
        picked = allowed.front();
    }

    ctx.chat_request.extra[kExtraGeoSelectedRegion] =
        regionOfModel(registry, picked, client_region);
    return picked;
}

} // namespace aegisgate
