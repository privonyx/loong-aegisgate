#pragma once
#include "auth/auth_models.h"
#include "storage/persistent_store.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace aegisgate {

struct MappedIdentity {
    User user;
    IdentityMapping mapping;
    bool newly_created = false;
};

class IdentityMapper {
public:
    explicit IdentityMapper(PersistentStore* store);

    std::optional<MappedIdentity> mapIdentity(const nlohmann::json& claims,
                                               const SsoProvider& provider);

    Role applyGroupRoleMapping(const std::vector<std::string>& groups,
                               const nlohmann::json& group_role_mapping,
                               const std::string& default_role);

private:
    std::string extractClaim(const nlohmann::json& claims,
                              const nlohmann::json& claim_mapping,
                              const std::string& field) const;

    std::vector<std::string> extractGroups(const nlohmann::json& claims,
                                            const nlohmann::json& claim_mapping) const;

    std::string generateId() const;
    std::string nowIso() const;

    PersistentStore* store_;
};

} // namespace aegisgate
