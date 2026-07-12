#pragma once
#include <drogon/HttpController.h>
#include <aegisgate/error_codes.h>
#include <functional>

namespace aegisgate {

class ApiController : public drogon::HttpController<ApiController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ApiController::chatCompletions,
                  "/v1/chat/completions", drogon::Post);
    ADD_METHOD_TO(ApiController::agentRun,
                  "/v1/agent", drogon::Post);
    ADD_METHOD_TO(ApiController::workflowRun,
                  "/v1/workflow", drogon::Post);
    ADD_METHOD_TO(ApiController::embeddings,
                  "/v1/embeddings", drogon::Post);
    ADD_METHOD_TO(ApiController::imageGenerations,
                  "/v1/images/generations", drogon::Post);
    ADD_METHOD_TO(ApiController::audioTranscriptions,
                  "/v1/audio/transcriptions", drogon::Post);
    ADD_METHOD_TO(ApiController::audioTranslations,
                  "/v1/audio/translations", drogon::Post);
    ADD_METHOD_TO(ApiController::audioSpeech,
                  "/v1/audio/speech", drogon::Post);
    ADD_METHOD_TO(ApiController::listModels,
                  "/v1/models", drogon::Get);
    ADD_METHOD_TO(ApiController::healthCheck,
                  "/health", drogon::Get);
    ADD_METHOD_TO(ApiController::healthLive,
                  "/health/live", drogon::Get);
    ADD_METHOD_TO(ApiController::healthReady,
                  "/health/ready", drogon::Get);
    ADD_METHOD_TO(ApiController::metrics,
                  "/metrics", drogon::Get);
    ADD_METHOD_TO(ApiController::reloadConfig,
                  "/admin/reload", drogon::Post);
    ADD_METHOD_TO(ApiController::cacheStats,
                  "/cache/stats", drogon::Get);
    ADD_METHOD_TO(ApiController::cacheImport,
                  "/admin/cache/import", drogon::Post);
    ADD_METHOD_TO(ApiController::logStream,
                  "/admin/logs/stream", drogon::Get);
    ADD_METHOD_TO(ApiController::openApiSpec,
                  "/openapi.yaml", drogon::Get);
    ADD_METHOD_TO(ApiController::openApiDocs,
                  "/docs", drogon::Get);
    METHOD_LIST_END

    void chatCompletions(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void agentRun(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void workflowRun(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void embeddings(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void imageGenerations(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void audioTranscriptions(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void audioTranslations(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void audioSpeech(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void listModels(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void healthCheck(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void healthLive(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void healthReady(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void metrics(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void reloadConfig(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void cacheStats(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void cacheImport(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void logStream(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void openApiSpec(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void openApiDocs(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    static std::string extractBearerToken(const drogon::HttpRequestPtr& req);
    static drogon::HttpResponsePtr makeErrorResponse(
        ErrorCode code, const std::string& message_override = "");
    static drogon::HttpResponsePtr makeErrorResponseRaw(
        int status, const std::string& aegis_code, const std::string& error_type,
        const std::string& message);

    void handleProxyEndpoint(
        const std::string& endpoint,
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace aegisgate
