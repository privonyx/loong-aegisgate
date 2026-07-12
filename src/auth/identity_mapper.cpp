#include "auth/identity_mapper.h"
#include <openssl/rand.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace aegisgate {

IdentityMapper::IdentityMapper(PersistentStore* store) : store_(store) {}

std::string IdentityMapper::extractClaim(const nlohmann::json& claims,
                                          const nlohmann::json& claim_mapping,
                                          const std::string& field) const {
    std::string claim_key = claim_mapping.value(field, "");
    if (claim_key.empty()) return "";
    return claims.value(claim_key, std::string{});
}

std::vector<std::string> IdentityMapper::extractGroups(const nlohmann::json& claims,
                                                        const nlohmann::json& claim_mapping) const {
    std::string claim_key = claim_mapping.value("groups", "groups");
    if (!claims.contains(claim_key) || !claims[claim_key].is_array()) return {};

    std::vector<std::string> groups;
    for (const auto& g : claims[claim_key]) {
        if (g.is_string()) groups.push_back(g.get<std::string>());
    }
    return groups;
}

std::optional<MappedIdentity> IdentityMapper::mapIdentity(const nlohmann::json& claims,
                                                           const SsoProvider& provider) {
    std::string sub = claims.value("sub", std::string{});
    if (sub.empty()) {
        spdlog::warn("IdentityMapper: missing 'sub' claim");
        return std::nullopt;
    }

    nlohmann::json claim_mapping = nlohmann::json::object();
    if (!provider.claim_mapping_json.empty()) {
        claim_mapping = nlohmann::json::parse(provider.claim_mapping_json, nullptr, false);
        if (claim_mapping.is_discarded()) claim_mapping = nlohmann::json::object();
    }

    std::string email = extractClaim(claims, claim_mapping, "email");
    std::string username = extractClaim(claims, claim_mapping, "username");
    std::string display_name = extractClaim(claims, claim_mapping, "display_name");

    auto existing = store_->getIdentityMapping(sub, provider.issuer_url);
    if (existing) {
        store_->updateIdentityMappingLastLogin(existing->id, nowIso());

        auto user = store_->getUser(existing->user_id);
        if (!user || user->status != "active") {
            spdlog::warn("IdentityMapper: user {} not found or inactive for mapping {}",
                         existing->user_id, existing->id);
            return std::nullopt;
        }

        existing->last_login_at = nowIso();
        return MappedIdentity{*user, *existing, false};
    }

    if (!provider.jit_provisioning) {
        spdlog::info("IdentityMapper: no mapping for sub={}, JIT disabled", sub);
        return std::nullopt;
    }

    nlohmann::json grm = nlohmann::json::object();
    if (!provider.group_role_mapping_json.empty()) {
        grm = nlohmann::json::parse(provider.group_role_mapping_json, nullptr, false);
        if (grm.is_discarded()) grm = nlohmann::json::object();
    }

    auto groups = extractGroups(claims, claim_mapping);
    Role role = applyGroupRoleMapping(groups, grm, provider.default_role);

    User user;
    user.id = generateId();
    user.tenant_id = provider.tenant_id;
    user.username = email.empty() ? sub : email;
    user.display_name = display_name.empty() ? user.username : display_name;
    user.role = role;
    user.status = "active";
    user.created_at = nowIso();
    user.updated_at = user.created_at;

    if (!store_->insertUser(user)) {
        spdlog::error("IdentityMapper: failed to insert JIT user for sub={}", sub);
        return std::nullopt;
    }

    IdentityMapping mapping;
    mapping.id = generateId();
    mapping.tenant_id = provider.tenant_id;
    mapping.external_subject = sub;
    mapping.external_issuer = provider.issuer_url;
    mapping.user_id = user.id;
    mapping.email = email;
    mapping.last_login_at = nowIso();
    mapping.created_at = mapping.last_login_at;

    if (!store_->insertIdentityMapping(mapping)) {
        spdlog::error("IdentityMapper: failed to insert identity mapping for sub={}", sub);
        return std::nullopt;
    }

    spdlog::info("IdentityMapper: JIT provisioned user {} for sub={}", user.id, sub);
    return MappedIdentity{user, mapping, true};
}

Role IdentityMapper::applyGroupRoleMapping(const std::vector<std::string>& groups,
                                            const nlohmann::json& group_role_mapping,
                                            const std::string& default_role) {
    Role highest = roleFromString(default_role).value_or(Role::Viewer);

    for (const auto& group : groups) {
        if (group_role_mapping.contains(group)) {
            std::string role_str = group_role_mapping[group].get<std::string>();
            auto mapped = roleFromString(role_str);
            if (mapped && static_cast<int>(*mapped) > static_cast<int>(highest)) {
                highest = *mapped;
            }
        }
    }

    return highest;
}

std::string IdentityMapper::generateId() const {
    unsigned char buf[16];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        throw std::runtime_error("RAND_bytes failed generating ID");
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto b : buf) oss << std::setw(2) << static_cast<int>(b);
    return oss.str();
}

std::string IdentityMapper::nowIso() const {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&tt, &utc);
    std::ostringstream oss;
    oss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

} // namespace aegisgate
