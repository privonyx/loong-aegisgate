#include "auth/scim_service.h"
#include "core/crypto.h"
#include "guardrail/audit.h"
#include <openssl/rand.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace aegisgate {

ScimService::ScimService(PersistentStore* store, AuditLogger* audit)
    : store_(store), audit_(audit) {}

void ScimService::auditWrite(const std::string& tenant_id,
                             const std::string& action,
                             const std::string& detail) {
    if (!audit_) return;
    // request_id="scim" 标识来源；stage="ScimService" 便于审计检索 IdP 同步追溯。
    audit_->logAction("scim", tenant_id, "ScimService", action, detail);
}

std::string ScimService::generateId() {
    unsigned char buf[16];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        throw std::runtime_error("RAND_bytes failed generating ID");
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto b : buf) oss << std::setw(2) << static_cast<int>(b);
    return oss.str();
}

std::string ScimService::nowTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&tt, &utc);
    std::ostringstream oss;
    oss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::optional<std::string> ScimService::authenticateToken(const std::string& bearer_token) const {
    std::string hash = crypto::sha256(bearer_token);
    auto token = store_->getScimTokenByHash(hash);
    if (!token) return std::nullopt;

    if (!token->expires_at.empty()) {
        std::string now = nowTimestamp();
        if (token->expires_at <= now) return std::nullopt;
    }

    return token->tenant_id;
}

// --- Users ---

nlohmann::json ScimService::createUser(const std::string& tenant_id,
                                        const nlohmann::json& scim_resource) {
    std::string username = scim_resource.value("userName", "");
    if (username.empty()) {
        return scimError(400, "userName is required");
    }

    auto existing = store_->getUserByUsername(tenant_id, username);
    if (existing) {
        return scimError(409, "User already exists");
    }

    std::string display_name = scim_resource.value("displayName", username);
    std::string email;
    if (scim_resource.contains("emails") && scim_resource["emails"].is_array() &&
        !scim_resource["emails"].empty()) {
        email = scim_resource["emails"][0].value("value", "");
    }

    std::string ts = nowTimestamp();

    User user;
    user.id = generateId();
    user.tenant_id = tenant_id;
    user.username = username;
    user.display_name = display_name;
    user.role = Role::Viewer;
    user.status = "active";
    user.created_at = ts;
    user.updated_at = ts;

    if (!store_->insertUser(user)) {
        return scimError(500, "Failed to create user");
    }

    IdentityMapping mapping;
    mapping.id = generateId();
    mapping.tenant_id = tenant_id;
    mapping.external_subject = "scim:" + user.id;
    mapping.external_issuer = "scim";
    mapping.user_id = user.id;
    mapping.email = email;
    mapping.last_login_at = ts;
    mapping.created_at = ts;
    store_->insertIdentityMapping(mapping);

    auditWrite(tenant_id, "scim.create_user", "user_id=" + user.id + " username=" + username);
    return userToScimJson(user);
}

nlohmann::json ScimService::getUser(const std::string& tenant_id, const std::string& id) {
    auto user = store_->getUser(id);
    if (!user || user->tenant_id != tenant_id) {
        return scimError(404, "User not found");
    }
    return userToScimJson(*user);
}

nlohmann::json ScimService::updateUser(const std::string& tenant_id, const std::string& id,
                                        const nlohmann::json& scim_resource) {
    auto user = store_->getUser(id);
    if (!user || user->tenant_id != tenant_id) {
        return scimError(404, "User not found");
    }

    if (scim_resource.contains("userName")) {
        user->username = scim_resource["userName"].get<std::string>();
    }
    if (scim_resource.contains("displayName")) {
        user->display_name = scim_resource["displayName"].get<std::string>();
    }
    if (scim_resource.contains("active")) {
        user->status = scim_resource["active"].get<bool>() ? "active" : "inactive";
    }
    user->updated_at = nowTimestamp();

    if (!store_->updateUser(*user)) {
        return scimError(500, "Failed to update user");
    }
    auditWrite(tenant_id, "scim.update_user", "user_id=" + id);
    return userToScimJson(*user);
}

nlohmann::json ScimService::deleteUser(const std::string& tenant_id, const std::string& id) {
    auto user = store_->getUser(id);
    if (!user || user->tenant_id != tenant_id) {
        return scimError(404, "User not found");
    }

    user->status = "inactive";
    user->updated_at = nowTimestamp();
    store_->updateUser(*user);

    auditWrite(tenant_id, "scim.delete_user", "user_id=" + id + " (deactivated)");
    return nlohmann::json::object();
}

nlohmann::json ScimService::listUsers(const std::string& tenant_id,
                                       const std::string& filter,
                                       int startIndex, int count) {
    auto all = store_->listUsers(tenant_id, 10000, 0);

    auto fexpr = parseFilter(filter);
    if (fexpr) {
        std::vector<User> filtered;
        for (const auto& u : all) {
            if (fexpr->attribute == "userName" && fexpr->op == "eq" &&
                u.username == fexpr->value) {
                filtered.push_back(u);
            }
        }
        all = std::move(filtered);
    }

    int total = static_cast<int>(all.size());
    int offset = startIndex - 1;
    if (offset < 0) offset = 0;

    nlohmann::json resources = nlohmann::json::array();
    for (int i = offset; i < total && i < offset + count; ++i) {
        resources.push_back(userToScimJson(all[i]));
    }

    return {
        {"schemas", {"urn:ietf:params:scim:api:messages:2.0:ListResponse"}},
        {"totalResults", total},
        {"startIndex", startIndex},
        {"itemsPerPage", count},
        {"Resources", resources}
    };
}

// --- Groups ---

nlohmann::json ScimService::createGroup(const std::string& tenant_id,
                                         const nlohmann::json& scim_resource) {
    std::string display_name = scim_resource.value("displayName", "");
    if (display_name.empty()) {
        return scimError(400, "displayName is required");
    }

    std::string id = generateId();
    std::vector<std::string> member_ids;
    if (scim_resource.contains("members") && scim_resource["members"].is_array()) {
        for (const auto& m : scim_resource["members"]) {
            if (m.contains("value")) {
                member_ids.push_back(m["value"].get<std::string>());
            }
        }
    }

    std::lock_guard<std::mutex> lock(groups_mutex_);
    if (store_ && store_->insertScimGroup(id, tenant_id, display_name)) {
        if (!member_ids.empty()) {
            store_->updateScimGroup(id, display_name, member_ids);
        }
        auto rec = store_->getScimGroup(id);
        if (rec) {
            ScimGroup group{rec->id, rec->display_name, rec->tenant_id,
                           member_ids, rec->created_at, rec->updated_at};
            auditWrite(tenant_id, "scim.create_group",
                       "group_id=" + id + " name=" + display_name);
            return groupToScimJson(group);
        }
    }

    ScimGroup group;
    group.id = id;
    group.tenant_id = tenant_id;
    group.display_name = display_name;
    group.member_ids = member_ids;
    group.created_at = nowTimestamp();
    group.updated_at = group.created_at;
    groups_[id] = group;
    auditWrite(tenant_id, "scim.create_group",
               "group_id=" + id + " name=" + display_name);
    return groupToScimJson(group);
}

nlohmann::json ScimService::getGroup(const std::string& tenant_id, const std::string& id) {
    std::lock_guard<std::mutex> lock(groups_mutex_);
    if (store_) {
        auto rec = store_->getScimGroup(id);
        if (!rec || rec->tenant_id != tenant_id) {
            return scimError(404, "Group not found");
        }
        auto members = store_->getScimGroupMembers(id);
        ScimGroup group{rec->id, rec->display_name, rec->tenant_id,
                       members, rec->created_at, rec->updated_at};
        return groupToScimJson(group);
    }
    auto it = groups_.find(id);
    if (it == groups_.end() || it->second.tenant_id != tenant_id) {
        return scimError(404, "Group not found");
    }
    return groupToScimJson(it->second);
}

nlohmann::json ScimService::updateGroup(const std::string& tenant_id, const std::string& id,
                                         const nlohmann::json& scim_resource) {
    std::lock_guard<std::mutex> lock(groups_mutex_);

    std::string display_name;
    std::vector<std::string> member_ids;

    if (store_) {
        auto rec = store_->getScimGroup(id);
        if (!rec || rec->tenant_id != tenant_id) {
            return scimError(404, "Group not found");
        }
        display_name = scim_resource.contains("displayName")
            ? scim_resource["displayName"].get<std::string>()
            : rec->display_name;
        if (scim_resource.contains("members") && scim_resource["members"].is_array()) {
            for (const auto& m : scim_resource["members"]) {
                if (m.contains("value")) member_ids.push_back(m["value"].get<std::string>());
            }
        } else {
            member_ids = store_->getScimGroupMembers(id);
        }
        store_->updateScimGroup(id, display_name, member_ids);
        auto updated = store_->getScimGroup(id);
        if (updated) {
            ScimGroup group{updated->id, updated->display_name, updated->tenant_id,
                           member_ids, updated->created_at, updated->updated_at};
            auditWrite(tenant_id, "scim.update_group", "group_id=" + id);
            return groupToScimJson(group);
        }
    }

    auto it = groups_.find(id);
    if (it == groups_.end() || it->second.tenant_id != tenant_id) {
        return scimError(404, "Group not found");
    }
    if (scim_resource.contains("displayName")) {
        it->second.display_name = scim_resource["displayName"].get<std::string>();
    }
    if (scim_resource.contains("members") && scim_resource["members"].is_array()) {
        it->second.member_ids.clear();
        for (const auto& m : scim_resource["members"]) {
            if (m.contains("value")) it->second.member_ids.push_back(m["value"].get<std::string>());
        }
    }
    it->second.updated_at = nowTimestamp();
    auditWrite(tenant_id, "scim.update_group", "group_id=" + id);
    return groupToScimJson(it->second);
}

nlohmann::json ScimService::deleteGroup(const std::string& tenant_id, const std::string& id) {
    std::lock_guard<std::mutex> lock(groups_mutex_);
    if (store_) {
        auto rec = store_->getScimGroup(id);
        if (!rec || rec->tenant_id != tenant_id) {
            return scimError(404, "Group not found");
        }
        store_->deleteScimGroup(id);
        auditWrite(tenant_id, "scim.delete_group", "group_id=" + id);
        return nlohmann::json::object();
    }
    auto it = groups_.find(id);
    if (it == groups_.end() || it->second.tenant_id != tenant_id) {
        return scimError(404, "Group not found");
    }
    groups_.erase(it);
    auditWrite(tenant_id, "scim.delete_group", "group_id=" + id);
    return nlohmann::json::object();
}

nlohmann::json ScimService::listGroups(const std::string& tenant_id,
                                        const std::string& filter,
                                        int startIndex, int count) {
    std::lock_guard<std::mutex> lock(groups_mutex_);
    auto fexpr = parseFilter(filter);

    std::vector<ScimGroup> matching;

    if (store_) {
        auto records = store_->listScimGroups(tenant_id);
        for (const auto& rec : records) {
            auto members = store_->getScimGroupMembers(rec.id);
            ScimGroup g{rec.id, rec.display_name, rec.tenant_id,
                       members, rec.created_at, rec.updated_at};
            if (fexpr) {
                if (fexpr->attribute == "displayName" && fexpr->op == "eq" &&
                    g.display_name == fexpr->value) {
                    matching.push_back(std::move(g));
                }
            } else {
                matching.push_back(std::move(g));
            }
        }
    } else {
        for (const auto& [gid, g] : groups_) {
            if (g.tenant_id != tenant_id) continue;
            if (fexpr) {
                if (fexpr->attribute == "displayName" && fexpr->op == "eq" &&
                    g.display_name == fexpr->value) {
                    matching.push_back(g);
                }
            } else {
                matching.push_back(g);
            }
        }
    }

    int total = static_cast<int>(matching.size());
    int offset = startIndex - 1;
    if (offset < 0) offset = 0;

    nlohmann::json resources = nlohmann::json::array();
    for (int i = offset; i < total && i < offset + count; ++i) {
        resources.push_back(groupToScimJson(matching[i]));
    }

    return {
        {"schemas", {"urn:ietf:params:scim:api:messages:2.0:ListResponse"}},
        {"totalResults", total},
        {"startIndex", startIndex},
        {"itemsPerPage", count},
        {"Resources", resources}
    };
}

// --- Helpers ---

nlohmann::json ScimService::scimError(int status, const std::string& detail) {
    return {
        {"schemas", {"urn:ietf:params:scim:api:messages:2.0:Error"}},
        {"status", std::to_string(status)},
        {"detail", detail}
    };
}

nlohmann::json ScimService::userToScimJson(const User& user) const {
    return {
        {"schemas", {"urn:ietf:params:scim:schemas:core:2.0:User"}},
        {"id", user.id},
        {"userName", user.username},
        {"displayName", user.display_name},
        {"active", user.status == "active"},
        {"emails", nlohmann::json::array()},
        {"groups", nlohmann::json::array()},
        {"meta", {
            {"resourceType", "User"},
            {"created", user.created_at},
            {"lastModified", user.updated_at}
        }}
    };
}

nlohmann::json ScimService::groupToScimJson(const ScimGroup& group) const {
    nlohmann::json members = nlohmann::json::array();
    for (const auto& mid : group.member_ids) {
        members.push_back({{"value", mid}});
    }

    return {
        {"schemas", {"urn:ietf:params:scim:schemas:core:2.0:Group"}},
        {"id", group.id},
        {"displayName", group.display_name},
        {"members", members},
        {"meta", {
            {"resourceType", "Group"},
            {"created", group.created_at},
            {"lastModified", group.updated_at}
        }}
    };
}

std::optional<ScimService::FilterExpr> ScimService::parseFilter(const std::string& filter) {
    if (filter.empty()) return std::nullopt;

    // Parse: attribute op "value"
    auto eq_pos = filter.find(" eq ");
    if (eq_pos == std::string::npos) return std::nullopt;

    FilterExpr expr;
    expr.attribute = filter.substr(0, eq_pos);
    expr.op = "eq";

    std::string val_part = filter.substr(eq_pos + 4);
    // Strip surrounding quotes
    if (val_part.size() >= 2 && val_part.front() == '"' && val_part.back() == '"') {
        val_part = val_part.substr(1, val_part.size() - 2);
    }
    expr.value = val_part;
    return expr;
}

} // namespace aegisgate
