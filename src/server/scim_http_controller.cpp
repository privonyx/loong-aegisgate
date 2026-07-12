#include "server/scim_http_controller.h"
#include "server/gateway_runtime.h"
#include <spdlog/spdlog.h>

namespace aegisgate {

static constexpr const char* kScimContentType = "application/scim+json;charset=utf-8";

ScimService& ScimHttpController::scimService() {
    auto* svc = GatewayRuntime::instance().scimService();
    if (!svc) {
        static ScimService fallback(nullptr);
        return fallback;
    }
    return *svc;
}

RateLimiter& ScimHttpController::rateLimiter() {
    static RateLimiter limiter({kScimRateMaxTokens, kScimRateRefillRate});
    return limiter;
}

std::optional<std::string> ScimHttpController::authenticateScim(
    const drogon::HttpRequestPtr& req) {
    auto auth_header = std::string(req->getHeader("Authorization"));
    if (auth_header.size() <= 7 ||
        auth_header.substr(0, 7) != "Bearer ") {
        return std::nullopt;
    }
    auto token = auth_header.substr(7);
    return scimService().authenticateToken(token);
}

drogon::HttpResponsePtr ScimHttpController::makeScimResponse(
    int status, const nlohmann::json& body) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    resp->setContentTypeString(kScimContentType);
    resp->setBody(body.dump());
    return resp;
}

drogon::HttpResponsePtr ScimHttpController::makeScimError(
    int status, const std::string& detail) {
    return makeScimResponse(status, ScimService::scimError(status, detail));
}

bool ScimHttpController::checkRateLimit(const std::string& tenant_id) {
    return rateLimiter().allow(tenant_id);
}

// ---------------------------------------------------------------------------
// Users
// ---------------------------------------------------------------------------

void ScimHttpController::listUsers(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    auto tenant_id = authenticateScim(req);
    if (!tenant_id) {
        callback(makeScimError(401, "Invalid or missing SCIM token"));
        return;
    }
    if (!checkRateLimit(*tenant_id)) {
        callback(makeScimError(429, "Rate limit exceeded"));
        return;
    }

    auto filter = std::string(req->getParameter("filter"));
    int start_index = 1, count = 100;
    auto si = req->getParameter("startIndex");
    auto cnt = req->getParameter("count");
    if (!si.empty()) try { start_index = std::stoi(std::string(si)); } catch (...) {}
    if (!cnt.empty()) try { count = std::stoi(std::string(cnt)); } catch (...) {}

    auto result = scimService().listUsers(*tenant_id, filter, start_index, count);
    callback(makeScimResponse(200, result));
}

void ScimHttpController::createUser(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    auto tenant_id = authenticateScim(req);
    if (!tenant_id) {
        callback(makeScimError(401, "Invalid or missing SCIM token"));
        return;
    }
    if (!checkRateLimit(*tenant_id)) {
        callback(makeScimError(429, "Rate limit exceeded"));
        return;
    }

    auto body = nlohmann::json::parse(
        req->bodyData(), req->bodyData() + req->bodyLength(), nullptr, false);
    if (body.is_discarded()) {
        callback(makeScimError(400, "Invalid JSON body"));
        return;
    }

    auto result = scimService().createUser(*tenant_id, body);
    int status = result.contains("status")
        ? std::stoi(result["status"].get<std::string>()) : 201;
    callback(makeScimResponse(status, result));
}

void ScimHttpController::getUser(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {

    auto tenant_id = authenticateScim(req);
    if (!tenant_id) {
        callback(makeScimError(401, "Invalid or missing SCIM token"));
        return;
    }
    if (!checkRateLimit(*tenant_id)) {
        callback(makeScimError(429, "Rate limit exceeded"));
        return;
    }

    auto result = scimService().getUser(*tenant_id, id);
    int status = result.contains("status")
        ? std::stoi(result["status"].get<std::string>()) : 200;
    callback(makeScimResponse(status, result));
}

void ScimHttpController::updateUser(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {

    auto tenant_id = authenticateScim(req);
    if (!tenant_id) {
        callback(makeScimError(401, "Invalid or missing SCIM token"));
        return;
    }
    if (!checkRateLimit(*tenant_id)) {
        callback(makeScimError(429, "Rate limit exceeded"));
        return;
    }

    auto body = nlohmann::json::parse(
        req->bodyData(), req->bodyData() + req->bodyLength(), nullptr, false);
    if (body.is_discarded()) {
        callback(makeScimError(400, "Invalid JSON body"));
        return;
    }

    auto result = scimService().updateUser(*tenant_id, id, body);
    int status = result.contains("status")
        ? std::stoi(result["status"].get<std::string>()) : 200;
    callback(makeScimResponse(status, result));
}

void ScimHttpController::deleteUser(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {

    auto tenant_id = authenticateScim(req);
    if (!tenant_id) {
        callback(makeScimError(401, "Invalid or missing SCIM token"));
        return;
    }
    if (!checkRateLimit(*tenant_id)) {
        callback(makeScimError(429, "Rate limit exceeded"));
        return;
    }

    auto result = scimService().deleteUser(*tenant_id, id);
    if (result.contains("status")) {
        int status = std::stoi(result["status"].get<std::string>());
        callback(makeScimResponse(status, result));
    } else {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k204NoContent);
        resp->setContentTypeString(kScimContentType);
        callback(resp);
    }
}

// ---------------------------------------------------------------------------
// Groups
// ---------------------------------------------------------------------------

void ScimHttpController::listGroups(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    auto tenant_id = authenticateScim(req);
    if (!tenant_id) {
        callback(makeScimError(401, "Invalid or missing SCIM token"));
        return;
    }
    if (!checkRateLimit(*tenant_id)) {
        callback(makeScimError(429, "Rate limit exceeded"));
        return;
    }

    auto filter = std::string(req->getParameter("filter"));
    int start_index = 1, count = 100;
    auto si = req->getParameter("startIndex");
    auto cnt = req->getParameter("count");
    if (!si.empty()) try { start_index = std::stoi(std::string(si)); } catch (...) {}
    if (!cnt.empty()) try { count = std::stoi(std::string(cnt)); } catch (...) {}

    auto result = scimService().listGroups(*tenant_id, filter, start_index, count);
    callback(makeScimResponse(200, result));
}

void ScimHttpController::createGroup(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    auto tenant_id = authenticateScim(req);
    if (!tenant_id) {
        callback(makeScimError(401, "Invalid or missing SCIM token"));
        return;
    }
    if (!checkRateLimit(*tenant_id)) {
        callback(makeScimError(429, "Rate limit exceeded"));
        return;
    }

    auto body = nlohmann::json::parse(
        req->bodyData(), req->bodyData() + req->bodyLength(), nullptr, false);
    if (body.is_discarded()) {
        callback(makeScimError(400, "Invalid JSON body"));
        return;
    }

    auto result = scimService().createGroup(*tenant_id, body);
    int status = result.contains("status")
        ? std::stoi(result["status"].get<std::string>()) : 201;
    callback(makeScimResponse(status, result));
}

void ScimHttpController::getGroup(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {

    auto tenant_id = authenticateScim(req);
    if (!tenant_id) {
        callback(makeScimError(401, "Invalid or missing SCIM token"));
        return;
    }
    if (!checkRateLimit(*tenant_id)) {
        callback(makeScimError(429, "Rate limit exceeded"));
        return;
    }

    auto result = scimService().getGroup(*tenant_id, id);
    int status = result.contains("status")
        ? std::stoi(result["status"].get<std::string>()) : 200;
    callback(makeScimResponse(status, result));
}

void ScimHttpController::updateGroup(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {

    auto tenant_id = authenticateScim(req);
    if (!tenant_id) {
        callback(makeScimError(401, "Invalid or missing SCIM token"));
        return;
    }
    if (!checkRateLimit(*tenant_id)) {
        callback(makeScimError(429, "Rate limit exceeded"));
        return;
    }

    auto body = nlohmann::json::parse(
        req->bodyData(), req->bodyData() + req->bodyLength(), nullptr, false);
    if (body.is_discarded()) {
        callback(makeScimError(400, "Invalid JSON body"));
        return;
    }

    auto result = scimService().updateGroup(*tenant_id, id, body);
    int status = result.contains("status")
        ? std::stoi(result["status"].get<std::string>()) : 200;
    callback(makeScimResponse(status, result));
}

void ScimHttpController::deleteGroup(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {

    auto tenant_id = authenticateScim(req);
    if (!tenant_id) {
        callback(makeScimError(401, "Invalid or missing SCIM token"));
        return;
    }
    if (!checkRateLimit(*tenant_id)) {
        callback(makeScimError(429, "Rate limit exceeded"));
        return;
    }

    auto result = scimService().deleteGroup(*tenant_id, id);
    if (result.contains("status")) {
        int status = std::stoi(result["status"].get<std::string>());
        callback(makeScimResponse(status, result));
    } else {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k204NoContent);
        resp->setContentTypeString(kScimContentType);
        callback(resp);
    }
}

} // namespace aegisgate
