#pragma once
#include "auth/auth_models.h"
#include "storage/persistent_store.h"
#include <nlohmann/json.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aegisgate {

class AuditLogger;

struct ScimGroup {
    std::string id;
    std::string display_name;
    std::string tenant_id;
    std::vector<std::string> member_ids;
    std::string created_at;
    std::string updated_at;
};

class ScimService {
public:
    // TASK-20260604-01 P0-D/D4=A：注入 AuditLogger（可空，向后兼容）。
    // audit 为 nullptr 时写操作不落审计但不崩溃（既有单参调用方保持工作）。
    explicit ScimService(PersistentStore* store, AuditLogger* audit = nullptr);

    std::optional<std::string> authenticateToken(const std::string& bearer_token) const;

    nlohmann::json createUser(const std::string& tenant_id, const nlohmann::json& scim_resource);
    nlohmann::json getUser(const std::string& tenant_id, const std::string& id);
    nlohmann::json updateUser(const std::string& tenant_id, const std::string& id,
                               const nlohmann::json& scim_resource);
    nlohmann::json deleteUser(const std::string& tenant_id, const std::string& id);
    nlohmann::json listUsers(const std::string& tenant_id,
                              const std::string& filter = "",
                              int startIndex = 1, int count = 100);

    nlohmann::json createGroup(const std::string& tenant_id, const nlohmann::json& scim_resource);
    nlohmann::json getGroup(const std::string& tenant_id, const std::string& id);
    nlohmann::json updateGroup(const std::string& tenant_id, const std::string& id,
                                const nlohmann::json& scim_resource);
    nlohmann::json deleteGroup(const std::string& tenant_id, const std::string& id);
    nlohmann::json listGroups(const std::string& tenant_id,
                               const std::string& filter = "",
                               int startIndex = 1, int count = 100);

    static nlohmann::json scimError(int status, const std::string& detail);

private:
    nlohmann::json userToScimJson(const User& user) const;
    nlohmann::json groupToScimJson(const ScimGroup& group) const;

    struct FilterExpr {
        std::string attribute;
        std::string op;
        std::string value;
    };
    static std::optional<FilterExpr> parseFilter(const std::string& filter);

    static std::string generateId();
    static std::string nowTimestamp();

    // SR-1：SCIM 写操作审计。action 形如 scim.create_user；仅成功路径调用。
    void auditWrite(const std::string& tenant_id, const std::string& action,
                    const std::string& detail);

    PersistentStore* store_;
    AuditLogger* audit_;

    std::unordered_map<std::string, ScimGroup> groups_;
    mutable std::mutex groups_mutex_;
};

} // namespace aegisgate
