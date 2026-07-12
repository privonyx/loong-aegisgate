#include "api_controller.h"
#include "gateway_runtime.h"
#include "response_headers.h"
#include "sse_response.h"
#include "observe/metrics.h"
#include "guardrail/audit.h"
#include "observe/tracing.h"
#include <aegisgate/error_codes.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#if __has_include("openapi_spec.h")
#include "openapi_spec.h"
#define AEGISGATE_HAS_OPENAPI_SPEC 1
#endif
#if __has_include("version.h")
#include "version.h"
#endif
#ifndef AEGISGATE_VERSION
#define AEGISGATE_VERSION "0.0.0-dev"
#endif

namespace aegisgate {

std::string ApiController::extractBearerToken(const drogon::HttpRequestPtr& req) {
    auto auth = req->getHeader("Authorization");
    const std::string prefix = "Bearer ";
    if (auth.size() > prefix.size() &&
        auth.substr(0, prefix.size()) == prefix) {
        return auth.substr(prefix.size());
    }
    return "";
}

drogon::HttpResponsePtr ApiController::makeErrorResponse(
    ErrorCode code, const std::string& message_override) {
    auto msg = message_override.empty()
        ? std::string(toDefaultMessage(code)) : message_override;
    nlohmann::json err;
    err["error"]["code"] = toAegisCode(code);
    err["error"]["type"] = toErrorType(code);
    err["error"]["message"] = msg;
    err["error"]["doc_url"] = toDocUrl(code);

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(toHttpStatus(code)));
    resp->setContentTypeString("application/json");
    resp->addHeader("X-AegisGate-Version", AEGISGATE_VERSION);
    resp->setBody(err.dump());
    return resp;
}

drogon::HttpResponsePtr ApiController::makeErrorResponseRaw(
    int status, const std::string& aegis_code, const std::string& error_type,
    const std::string& message) {
    nlohmann::json err;
    err["error"]["code"] = aegis_code;
    err["error"]["type"] = error_type;
    err["error"]["message"] = message;

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    resp->setContentTypeString("application/json");
    resp->addHeader("X-AegisGate-Version", AEGISGATE_VERSION);
    resp->setBody(err.dump());
    return resp;
}

void ApiController::chatCompletions(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    auto& runtime = GatewayRuntime::instance();

    if (!runtime.isInitialized()) {
        callback(makeErrorResponse(ErrorCode::NotInitialized));
        return;
    }

    if (runtime.isShuttingDown()) {
        callback(makeErrorResponseRaw(503, "shutting_down", "server_error",
            "Server is shutting down — retry with another instance"));
        return;
    }

    // Auth check
    auto api_key = extractBearerToken(req);
    if (!runtime.validateApiKey(api_key)) {
        callback(makeErrorResponse(ErrorCode::InvalidApiKey));
        return;
    }

    static constexpr size_t kMaxRequestBodySize = 1024 * 1024; // 1 MiB
    if (req->body().size() > kMaxRequestBodySize) {
        callback(makeErrorResponse(ErrorCode::PayloadTooLarge));
        return;
    }

    ChatRequest chat_req;
    try {
        auto j = nlohmann::json::parse(req->body());
        chat_req.model = j.value("model", "");
        if (j.contains("messages")) {
            for (const auto& m : j["messages"]) {
                Message msg;
                msg.role = m.value("role", "user");
                if (m.contains("content") && !m["content"].is_null()) {
                    if (m["content"].is_string()) {
                        msg.content = m["content"].get<std::string>();
                    } else if (m["content"].is_array()) {
                        // P2-#4: preserve vision/multimodal parts; extract text
                        // so guardrails can scan it. Without this the image_url
                        // parts were silently dropped at the gateway entry.
                        msg.content_parts = m["content"];
                        msg.content = extractMultimodalText(m["content"]);
                    }
                }
                if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
                    for (const auto& tc : m["tool_calls"]) {
                        ToolCall tool_call;
                        tool_call.id = tc.value("id", "");
                        tool_call.type = tc.value("type", "function");
                        if (tc.contains("function"))
                            tool_call.function = tc["function"];
                        msg.tool_calls.push_back(std::move(tool_call));
                    }
                }
                msg.tool_call_id = m.value("tool_call_id", "");
                msg.name = m.value("name", "");
                chat_req.messages.push_back(std::move(msg));
            }
        }
        if (j.contains("temperature"))
            chat_req.temperature = j["temperature"].get<double>();
        if (j.contains("max_tokens"))
            chat_req.max_tokens = j["max_tokens"].get<int>();
        chat_req.stream = j.value("stream", false);
        if (j.contains("extra"))
            chat_req.extra = j["extra"];
        if (j.contains("tools"))
            chat_req.tools = j["tools"];
        if (j.contains("tool_choice"))
            chat_req.tool_choice = j["tool_choice"];
    } catch (const std::exception& e) {
        callback(makeErrorResponse(ErrorCode::InvalidRequest,
            std::string("Failed to parse request: ") + e.what()));
        return;
    }

    if (chat_req.messages.empty()) {
        callback(makeErrorResponse(ErrorCode::MissingRequiredField,
            "messages field is required and must not be empty"));
        return;
    }

    // TASK-20260709-01 / REV20260707-I5 D7: whitelist template override header.
    std::unordered_map<std::string, std::string> request_headers;
    {
        auto tpl = std::string(req->getHeader("X-AegisGate-Template"));
        if (!tpl.empty()) {
            request_headers["X-AegisGate-Template"] = std::move(tpl);
        }
    }

#ifdef AEGISGATE_ENABLE_OTEL
    opentelemetry::nostd::shared_ptr<otel_trace::Span> root_span;
    std::unique_ptr<otel_trace::Scope> root_scope;
    if (Tracing::instance().isEnabled()) {
        std::unordered_map<std::string, std::string> trace_hdrs;
        auto tp = std::string(req->getHeader("traceparent"));
        if (!tp.empty()) trace_hdrs["traceparent"] = tp;
        auto ts = std::string(req->getHeader("tracestate"));
        if (!ts.empty()) trace_hdrs["tracestate"] = ts;
        auto trace_ctx = Tracing::instance().extractContext(trace_hdrs);

        auto tracer = Tracing::instance().tracer();
        otel_trace::StartSpanOptions opts;
        opts.parent = trace_ctx;
        root_span = tracer->StartSpan("aegisgate.request", {
            {"aegisgate.model", std::string(chat_req.model)},
            {"aegisgate.is_streaming", chat_req.stream},
        }, opts);
        root_scope = std::make_unique<otel_trace::Scope>(root_span);
    }
#endif

    // Streaming branch
    if (chat_req.stream) {
        auto stream_model = chat_req.model;
#ifdef AEGISGATE_ENABLE_OTEL
        root_scope.reset();
#endif
        auto resp = drogon::HttpResponse::newAsyncStreamResponse(
            [chat_req_copy = std::move(chat_req), api_key, stream_model,
             request_headers
#ifdef AEGISGATE_ENABLE_OTEL
             , root_span
#endif
            ]
            (drogon::ResponseStreamPtr stream) mutable {
                auto& rt = GatewayRuntime::instance();
                auto writer = std::make_shared<SseResponseWriter>(std::move(stream));

#ifdef AEGISGATE_ENABLE_OTEL
                std::unique_ptr<otel_trace::Scope> stream_scope;
                if (root_span) {
                    stream_scope = std::make_unique<otel_trace::Scope>(root_span);
                }
#endif

                rt.processStreamingRequest(
                    std::move(chat_req_copy), api_key,
                    [writer, stream_model](const StreamDelta& delta) {
                        writer->writeDelta(delta, stream_model);
                    },
                    [writer, stream_model
#ifdef AEGISGATE_ENABLE_OTEL
                     , root_span
#endif
                    ](const TokenUsage& usage, int tokens_saved) {
                        writer->writeDone(usage, stream_model, tokens_saved);
#ifdef AEGISGATE_ENABLE_OTEL
                        if (root_span) root_span->End();
#endif
                    },
                    [writer
#ifdef AEGISGATE_ENABLE_OTEL
                     , root_span
#endif
                    ](const GatewayError& err) {
                        writer->writeError(err);
#ifdef AEGISGATE_ENABLE_OTEL
                        if (root_span) {
                            root_span->SetStatus(otel_trace::StatusCode::kError, err.message);
                            root_span->End();
                        }
#endif
                    },
                    request_headers);
            });
        resp->setContentTypeString("text/event-stream");
        resp->addHeader("Cache-Control", "no-cache");
        resp->addHeader("X-Accel-Buffering", "no");
        resp->addHeader("X-AegisGate-Version", AEGISGATE_VERSION);
        callback(resp);
        return;
    }

    // Non-streaming path
    auto result = runtime.processRequest(std::move(chat_req), api_key, request_headers);

#ifdef AEGISGATE_ENABLE_OTEL
    if (root_span) {
        root_span->SetAttribute("aegisgate.request_id",
            result.success ? result.response.id : std::string(""));
        root_span->SetAttribute("aegisgate.cache_hit", false);
        if (!result.success) {
            root_span->SetStatus(otel_trace::StatusCode::kError, result.error.message);
        }
        root_span->End();
    }
#endif

    if (!result.success) {
        callback(makeErrorResponseRaw(
            result.error.http_status,
            result.error.error_code,
            result.error.error_type,
            result.error.message));
        return;
    }

    // Build OpenAI-compatible response
    nlohmann::json resp_json;
    resp_json["id"] = result.response.id;
    resp_json["object"] = "chat.completion";
    resp_json["model"] = result.response.model;

    nlohmann::json msg_json;
    msg_json["role"] = "assistant";
    if (!result.response.tool_calls.empty()) {
        msg_json["content"] = result.response.content.empty()
            ? nlohmann::json(nullptr) : nlohmann::json(result.response.content);
        nlohmann::json tcs = nlohmann::json::array();
        for (const auto& tc : result.response.tool_calls) {
            nlohmann::json tcj;
            to_json(tcj, tc);
            tcs.push_back(tcj);
        }
        msg_json["tool_calls"] = tcs;
    } else {
        msg_json["content"] = result.response.content;
    }

    resp_json["choices"] = nlohmann::json::array();
    resp_json["choices"].push_back({
        {"index", 0},
        {"message", msg_json},
        {"finish_reason", result.response.finish_reason}
    });
    resp_json["usage"] = {
        {"prompt_tokens", result.response.usage.prompt_tokens},
        {"completion_tokens", result.response.usage.completion_tokens},
        {"total_tokens", result.response.usage.total_tokens}
    };

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeString("application/json");
    if (result.tokens_saved > 0) {
        resp->addHeader("X-AegisGate-Tokens-Saved",
                        std::to_string(result.tokens_saved));
    }
    // P1-8/P1-9: surface budget-downgrade / A/B-variant headers to the client.
    applyResponseHeaders(*resp, result.response_headers);
    resp->addHeader("X-AegisGate-Version", AEGISGATE_VERSION);
    resp->setBody(resp_json.dump());
    callback(resp);
}

void ApiController::agentRun(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    auto& runtime = GatewayRuntime::instance();

    if (!runtime.isInitialized()) {
        callback(makeErrorResponse(ErrorCode::NotInitialized));
        return;
    }
    if (runtime.isShuttingDown()) {
        callback(makeErrorResponseRaw(503, "shutting_down", "server_error",
            "Server is shutting down — retry with another instance"));
        return;
    }

    auto api_key = extractBearerToken(req);

    static constexpr size_t kMaxRequestBodySize = 1024 * 1024; // 1 MiB
    if (req->body().size() > kMaxRequestBodySize) {
        callback(makeErrorResponse(ErrorCode::PayloadTooLarge));
        return;
    }

    std::string model;
    std::string input;
    int max_steps = 0;
    try {
        auto j = nlohmann::json::parse(req->body());
        model = j.value("model", "");
        input = j.value("input", "");
        max_steps = j.value("max_steps", 0);
    } catch (const std::exception& e) {
        callback(makeErrorResponse(ErrorCode::InvalidRequest,
            std::string("Failed to parse request: ") + e.what()));
        return;
    }

    if (input.empty()) {
        callback(makeErrorResponse(ErrorCode::MissingRequiredField,
            "input field is required and must not be empty"));
        return;
    }

    auto result = runtime.processAgentRequest(model, input, max_steps, api_key);

    if (!result.success) {
        callback(makeErrorResponseRaw(
            result.error.http_status,
            result.error.error_code,
            result.error.error_type,
            result.error.message));
        return;
    }

    nlohmann::json resp_json;
    resp_json["success"] = result.run.success;
    resp_json["final_answer"] = result.run.final_answer;
    resp_json["total_steps"] = result.run.total_steps;
    resp_json["total_duration_ms"] = result.run.total_duration_ms;
    if (!result.run.error.empty()) resp_json["error"] = result.run.error;
    resp_json["steps"] = nlohmann::json::array();
    for (const auto& s : result.run.steps) {
        nlohmann::json sj;
        sj["step_number"] = s.step_number;
        sj["thought"] = s.thought;
        if (!s.tool_id.empty()) sj["tool_id"] = s.tool_id;
        if (s.tool_result.has_value()) {
            // 截断工具输出，避免响应膨胀（元数据 + 有界预览）。
            std::string out = s.tool_result->output;
            static constexpr size_t kMaxPreview = 4096;
            if (out.size() > kMaxPreview) out.resize(kMaxPreview);
            sj["tool_result"] = out;
        }
        resp_json["steps"].push_back(std::move(sj));
    }

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeString("application/json");
    resp->addHeader("X-AegisGate-Version", AEGISGATE_VERSION);
    resp->setBody(resp_json.dump());
    callback(resp);
}

void ApiController::workflowRun(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    auto& runtime = GatewayRuntime::instance();

    if (!runtime.isInitialized()) {
        callback(makeErrorResponse(ErrorCode::NotInitialized));
        return;
    }
    if (runtime.isShuttingDown()) {
        callback(makeErrorResponseRaw(503, "shutting_down", "server_error",
            "Server is shutting down — retry with another instance"));
        return;
    }

    auto api_key = extractBearerToken(req);

    static constexpr size_t kMaxRequestBodySize = 1024 * 1024; // 1 MiB
    if (req->body().size() > kMaxRequestBodySize) {
        callback(makeErrorResponse(ErrorCode::PayloadTooLarge));
        return;
    }

    nlohmann::json workflow_json;
    nlohmann::json context_json;
    try {
        auto j = nlohmann::json::parse(req->body());
        if (!j.contains("workflow")) {
            callback(makeErrorResponse(ErrorCode::MissingRequiredField,
                "workflow field is required"));
            return;
        }
        workflow_json = j["workflow"];
        if (j.contains("context")) context_json = j["context"];
    } catch (const std::exception& e) {
        callback(makeErrorResponse(ErrorCode::InvalidRequest,
            std::string("Failed to parse request: ") + e.what()));
        return;
    }

    auto result = runtime.processWorkflowRequest(workflow_json, context_json, api_key);

    if (!result.success && result.error.http_status != 0) {
        callback(makeErrorResponseRaw(
            result.error.http_status,
            result.error.error_code,
            result.error.error_type,
            result.error.message));
        return;
    }

    nlohmann::json resp_json;
    resp_json["ok"] = result.exec.ok;
    resp_json["run_id"] = result.run_id;
    resp_json["final_status"] = workflow::toString(result.exec.final_status);
    resp_json["completed_nodes"] = result.exec.completed_nodes;
    resp_json["failed_nodes"] = result.exec.failed_nodes;
    if (!result.exec.error_message.empty())
        resp_json["error"] = result.exec.error_message;

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeString("application/json");
    resp->addHeader("X-AegisGate-Version", AEGISGATE_VERSION);
    resp->setBody(resp_json.dump());
    callback(resp);
}

void ApiController::handleProxyEndpoint(
    const std::string& endpoint,
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    auto& runtime = GatewayRuntime::instance();

    if (!runtime.isInitialized()) {
        callback(makeErrorResponse(ErrorCode::NotInitialized));
        return;
    }

    if (runtime.isShuttingDown()) {
        callback(makeErrorResponseRaw(503, "shutting_down", "server_error",
            "Server is shutting down — retry with another instance"));
        return;
    }

    auto api_key = extractBearerToken(req);
    if (!runtime.validateApiKey(api_key)) {
        callback(makeErrorResponse(ErrorCode::InvalidApiKey));
        return;
    }

    static constexpr size_t kMaxProxyBodySize = 25 * 1024 * 1024; // 25 MiB for audio/images
    if (req->body().size() > kMaxProxyBodySize) {
        callback(makeErrorResponse(ErrorCode::PayloadTooLarge));
        return;
    }

    ProxyRequest proxy_req;
    proxy_req.endpoint = endpoint;
    proxy_req.raw_body = std::string(req->body());
    proxy_req.content_type = std::string(req->getHeader("Content-Type"));
    if (proxy_req.content_type.empty()) {
        proxy_req.content_type = "application/json";
    }

    if (proxy_req.content_type.find("application/json") != std::string::npos) {
        try {
            auto j = nlohmann::json::parse(proxy_req.raw_body);
            proxy_req.model = j.value("model", "");
        } catch (...) {
            // model extraction is best-effort
        }
    }

    auto result = runtime.processProxyRequest(std::move(proxy_req), api_key);

    auto resp = drogon::HttpResponse::newHttpResponse();
    if (result.success) {
        resp->setStatusCode(static_cast<drogon::HttpStatusCode>(result.response.http_status));
        resp->setContentTypeString(result.response.content_type);
        resp->setBody(std::move(result.response.body));
    } else if (!result.response.body.empty()) {
        resp->setStatusCode(static_cast<drogon::HttpStatusCode>(result.response.http_status));
        resp->setContentTypeString(result.response.content_type);
        resp->setBody(std::move(result.response.body));
    } else {
        resp->setStatusCode(static_cast<drogon::HttpStatusCode>(result.error.http_status));
        resp->setContentTypeString("application/json");
        nlohmann::json err;
        err["error"]["code"] = result.error.error_code;
        err["error"]["type"] = result.error.error_type;
        err["error"]["message"] = result.error.message;
        resp->setBody(err.dump());
    }
    resp->addHeader("X-AegisGate-Version", AEGISGATE_VERSION);
    callback(resp);
}

void ApiController::embeddings(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    handleProxyEndpoint("/v1/embeddings", req, std::move(callback));
}

void ApiController::imageGenerations(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    handleProxyEndpoint("/v1/images/generations", req, std::move(callback));
}

void ApiController::audioTranscriptions(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    handleProxyEndpoint("/v1/audio/transcriptions", req, std::move(callback));
}

void ApiController::audioTranslations(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    handleProxyEndpoint("/v1/audio/translations", req, std::move(callback));
}

void ApiController::audioSpeech(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    handleProxyEndpoint("/v1/audio/speech", req, std::move(callback));
}

void ApiController::listModels(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    auto& runtime = GatewayRuntime::instance();

    if (!runtime.isInitialized()) {
        callback(makeErrorResponse(ErrorCode::NotInitialized));
        return;
    }

    if (runtime.isShuttingDown()) {
        callback(makeErrorResponseRaw(503, "shutting_down", "server_error",
            "Server is shutting down — retry with another instance"));
        return;
    }

    auto api_key = extractBearerToken(req);
    if (!runtime.authorizeApiRequest(api_key)) {
        callback(makeErrorResponse(ErrorCode::InvalidApiKey));
        return;
    }

    nlohmann::json resp_json;
    resp_json["object"] = "list";
    resp_json["data"] = nlohmann::json::array();

    for (const auto& model : runtime.registeredModels()) {
        nlohmann::json m;
        m["id"] = model.id;
        m["object"] = "model";
        m["owned_by"] = model.provider;
        m["permission"] = nlohmann::json::array();
        resp_json["data"].push_back(m);
    }

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeString("application/json");
    resp->addHeader("X-AegisGate-Version", AEGISGATE_VERSION);
    resp->setBody(resp_json.dump());
    callback(resp);
}

void ApiController::healthCheck(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    healthReady(req, std::move(callback));
}

void ApiController::healthLive(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    auto resp = drogon::HttpResponse::newHttpJsonResponse(
        Json::Value(Json::objectValue));
    auto& body = *resp->getJsonObject();
    body["status"] = "alive";
    resp->setStatusCode(drogon::k200OK);
    resp->addHeader("X-AegisGate-Version", AEGISGATE_VERSION);
    callback(resp);
}

void ApiController::healthReady(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    auto& runtime = GatewayRuntime::instance();

    bool runtime_ok = runtime.isInitialized();
    bool shutting_down = runtime.isShuttingDown();
    bool persistent_ok = true;
    bool cache_ok = true;
    std::string persistent_backend = "none";
    std::string cache_backend = "none";

    if (runtime_ok) {
        auto& pipe = runtime.pipeline();
        if (pipe.persistent_store) {
            // P1-B: active liveness probe (SELECT 1 / PING for pooled backends)
            // so a DB/Redis outage after startup is not masked.
            persistent_ok = pipe.persistent_store->isReady();
            persistent_backend = pipe.persistent_store->backendName();
        }
        if (pipe.cache_store) {
            cache_ok = pipe.cache_store->isReady();
            cache_backend = pipe.cache_store->backendName();
        }
    }

    bool storage_ok = persistent_ok && cache_ok;
    bool ready = runtime_ok && !shutting_down && storage_ok;

    auto resp = drogon::HttpResponse::newHttpJsonResponse(
        Json::Value(Json::objectValue));
    auto& body = *resp->getJsonObject();
    body["status"] = ready ? "ready" : (storage_ok ? "degraded" : "not_ready");
    body["version"] = AEGISGATE_VERSION;
    body["checks"]["runtime"] = runtime_ok;
    body["checks"]["shutting_down"] = shutting_down;
    body["checks"]["persistent_store"]["healthy"] = persistent_ok;
    body["checks"]["persistent_store"]["backend"] = persistent_backend;
    body["checks"]["cache_store"]["healthy"] = cache_ok;
    body["checks"]["cache_store"]["backend"] = cache_backend;
    resp->setStatusCode(ready ? drogon::k200OK
                              : drogon::k503ServiceUnavailable);
    resp->addHeader("X-AegisGate-Version", AEGISGATE_VERSION);
    callback(resp);
}

void ApiController::metrics(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    auto& runtime = GatewayRuntime::instance();
    auto api_key = extractBearerToken(req);
    if (!runtime.authorizeApiRequest(api_key)) {
        callback(makeErrorResponse(ErrorCode::InvalidApiKey));
        return;
    }

    if (runtime.isInitialized()) {
        auto* fm = runtime.fallbackManager();
        if (fm) {
            auto& gauge = MetricsRegistry::instance().circuitBreakerState();
            gauge.reset();
            // REV20260707-S4 D1 Option B: exportMetrics is now non-const
            // (it applies the Open->HalfOpen timeout transition), so use
            // the mutable accessor.
            fm->mutableCircuitBreaker().exportMetrics(gauge);
        }
    }

    auto output = MetricsRegistry::instance().exposeAll();
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeString("text/plain; version=0.0.4; charset=utf-8");
    resp->setBody(output);
    callback(resp);
}

void ApiController::reloadConfig(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    auto& runtime = GatewayRuntime::instance();

    auto api_key = extractBearerToken(req);
    if (!runtime.validateAdminKey(api_key)) {
        callback(makeErrorResponse(ErrorCode::InvalidAdminKey));
        return;
    }

    bool ok = runtime.reloadConfig();

    auto resp = drogon::HttpResponse::newHttpJsonResponse(
        Json::Value(Json::objectValue));
    auto& body = *resp->getJsonObject();
    if (ok) {
        body["status"] = "reloaded";
        body["message"] = "Configuration reloaded successfully";
        resp->setStatusCode(drogon::k200OK);
    } else {
        body["status"] = "error";
        body["message"] = "Failed to reload configuration";
        resp->setStatusCode(drogon::k500InternalServerError);
    }
    callback(resp);
}

void ApiController::cacheStats(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    auto& runtime = GatewayRuntime::instance();
    auto api_key = extractBearerToken(req);
    if (!runtime.authorizeApiRequest(api_key)) {
        callback(makeErrorResponse(ErrorCode::InvalidApiKey));
        return;
    }

    auto* cache = runtime.pipeline().semantic_cache;
    if (!cache) {
        callback(makeErrorResponse(ErrorCode::CacheUnavailable));
        return;
    }

    auto stats = cache->getStats();
    nlohmann::json j;
    j["hit_count"] = stats.hit_count;
    j["miss_count"] = stats.miss_count;
    j["put_count"] = stats.put_count;
    j["entry_count"] = stats.entry_count;
    j["hit_rate"] = stats.hit_rate;
    j["current_threshold"] = stats.current_threshold;

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeString("application/json");
    resp->addHeader("X-AegisGate-Version", AEGISGATE_VERSION);
    resp->setBody(j.dump());
    callback(resp);
}

void ApiController::cacheImport(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    auto& runtime = GatewayRuntime::instance();
    auto api_key = extractBearerToken(req);
    if (!runtime.validateAdminKey(api_key)) {
        callback(makeErrorResponse(ErrorCode::InvalidAdminKey));
        return;
    }

    auto* cache = runtime.pipeline().semantic_cache;
    if (!cache) {
        callback(makeErrorResponse(ErrorCode::CacheUnavailable));
        return;
    }

    auto imported = cache->importFromJson(std::string(req->body()));

    nlohmann::json j;
    j["imported"] = imported;
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeString("application/json");
    resp->setBody(j.dump());
    callback(resp);
}

void ApiController::logStream(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    auto& runtime = GatewayRuntime::instance();
    auto api_key = extractBearerToken(req);
    if (!runtime.validateAdminKey(api_key)) {
        callback(makeErrorResponse(ErrorCode::InvalidAdminKey));
        return;
    }

    auto level_filter = req->getParameter("level");

    auto* audit = runtime.pipeline().audit_logger;
    if (!audit) {
        callback(makeErrorResponse(ErrorCode::InternalError, "Audit logger not available"));
        return;
    }

    auto resp = drogon::HttpResponse::newAsyncStreamResponse(
        [audit, level_filter](drogon::ResponseStreamPtr stream) {
            auto stream_ptr = std::make_shared<drogon::ResponseStreamPtr>(std::move(stream));
            auto sub_id = std::make_shared<size_t>(0);
            *sub_id = audit->subscribe(
                [stream_ptr, level_filter, sub_id, audit](const AuditEntry& entry) {
                    if (!level_filter.empty() && entry.action != level_filter) {
                        return;
                    }
                    nlohmann::json j;
                    j["timestamp"] = entry.timestamp;
                    j["request_id"] = entry.request_id;
                    j["tenant_id"] = entry.tenant_id;
                    j["stage"] = entry.stage_name;
                    j["action"] = entry.action;
                    j["detail"] = entry.detail;
                    auto data = "data: " + j.dump() + "\n\n";
                    if (*stream_ptr && !(*stream_ptr)->send(data)) {
                        audit->unsubscribe(*sub_id);
                    }
                });
        });
    resp->setContentTypeString("text/event-stream");
    resp->addHeader("Cache-Control", "no-cache");
    resp->addHeader("Connection", "keep-alive");
    callback(resp);
}

void ApiController::openApiSpec(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
#ifdef AEGISGATE_HAS_OPENAPI_SPEC
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeString("application/yaml");
    resp->setBody(std::string(kOpenApiSpec));
    callback(resp);
#else
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k404NotFound);
    resp->setContentTypeString("text/plain");
    resp->setBody("OpenAPI spec not available in this build");
    callback(resp);
#endif
}

void ApiController::openApiDocs(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    static const std::string html = R"(<!DOCTYPE html>
<html>
<head>
  <title>AegisGate API Documentation</title>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
</head>
<body>
  <script id="api-reference" data-url="/openapi.yaml"></script>
  <script src="https://cdn.jsdelivr.net/npm/@scalar/api-reference"></script>
</body>
</html>)";
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeString("text/html");
    resp->setBody(html);
    callback(resp);
}

} // namespace aegisgate
