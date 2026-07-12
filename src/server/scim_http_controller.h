#pragma once
#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>
#include "auth/scim_service.h"
#include "gateway/rate_limiter.h"
#include <optional>
#include <string>
#include <functional>

namespace aegisgate {

class ScimHttpController : public drogon::HttpController<ScimHttpController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ScimHttpController::listUsers,
                  "/scim/v2/Users", drogon::Get);
    ADD_METHOD_TO(ScimHttpController::createUser,
                  "/scim/v2/Users", drogon::Post);
    ADD_METHOD_TO(ScimHttpController::getUser,
                  "/scim/v2/Users/{id}", drogon::Get);
    ADD_METHOD_TO(ScimHttpController::updateUser,
                  "/scim/v2/Users/{id}", drogon::Put);
    ADD_METHOD_TO(ScimHttpController::deleteUser,
                  "/scim/v2/Users/{id}", drogon::Delete);

    ADD_METHOD_TO(ScimHttpController::listGroups,
                  "/scim/v2/Groups", drogon::Get);
    ADD_METHOD_TO(ScimHttpController::createGroup,
                  "/scim/v2/Groups", drogon::Post);
    ADD_METHOD_TO(ScimHttpController::getGroup,
                  "/scim/v2/Groups/{id}", drogon::Get);
    ADD_METHOD_TO(ScimHttpController::updateGroup,
                  "/scim/v2/Groups/{id}", drogon::Put);
    ADD_METHOD_TO(ScimHttpController::deleteGroup,
                  "/scim/v2/Groups/{id}", drogon::Delete);
    METHOD_LIST_END

    void listUsers(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void createUser(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void getUser(const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                 std::string id);
    void updateUser(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                    std::string id);
    void deleteUser(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                    std::string id);

    void listGroups(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void createGroup(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void getGroup(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                  std::string id);
    void updateGroup(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     std::string id);
    void deleteGroup(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     std::string id);

private:
    std::optional<std::string> authenticateScim(const drogon::HttpRequestPtr& req);
    drogon::HttpResponsePtr makeScimResponse(int status, const nlohmann::json& body);
    drogon::HttpResponsePtr makeScimError(int status, const std::string& detail);
    bool checkRateLimit(const std::string& tenant_id);

    static ScimService& scimService();
    static RateLimiter& rateLimiter();

    static constexpr double kScimRateMaxTokens = 50.0;
    static constexpr double kScimRateRefillRate = 5.0;
};

} // namespace aegisgate
