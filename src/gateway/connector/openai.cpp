#include "openai.h"
#include "default_upstream.h"
#include "observe/tracing.h"
#include <aegisgate/error_codes.h>
#include <future>
#include <spdlog/spdlog.h>
#include <sstream>
#include <unordered_set>

namespace aegisgate {

OpenAIConnector::OpenAIConnector(const ProviderConfig& config,
                                 std::unique_ptr<UpstreamClient> upstream)
    : config_(config), balancer_(config.api_keys),
      upstream_(upstream ? std::move(upstream)
                         : makeDefaultUpstreamClient(config.base_url)) {}

std::string OpenAIConnector::provider() const {
    return config_.name;
}

bool OpenAIConnector::supportsModel(const std::string& model) const {
    for (const auto& m : config_.models) {
        if (m.id == model) return true;
    }
    return false;
}

ModelCapabilities OpenAIConnector::capabilities(const std::string& /*model*/) const {
    return {{Capability::Streaming, Capability::Tools, Capability::ResponseFormat,
             Capability::Logprobs, Capability::Vision, Capability::SystemMessage,
             Capability::Temperature, Capability::TopP, Capability::MaxTokens}};
}

std::string OpenAIConnector::chatEndpoint() const {
    return "/chat/completions";
}

static const std::unordered_set<std::string> kOpenAIProxyEndpoints = {
    "/v1/embeddings",
    "/v1/images/generations",
    "/v1/audio/transcriptions",
    "/v1/audio/translations",
    "/v1/audio/speech",
};

bool OpenAIConnector::supportsEndpoint(const std::string& endpoint) const {
    return kOpenAIProxyEndpoints.count(endpoint) > 0;
}

ProxyResponse OpenAIConnector::proxyRequest(const ProxyRequest& req) {
    auto key = balancer_.nextKey();
    if (key.empty()) {
        throw std::runtime_error("No healthy API keys for " + config_.name);
    }

    UpstreamRequest ureq;
    ureq.method = "POST";
    ureq.path = req.endpoint;
    ureq.body = req.raw_body;
    ureq.headers["Authorization"] = authHeader(key);
    ureq.headers["Content-Type"] = req.content_type;
    ureq.timeout_seconds = config_.timeout_ms / 1000.0;

    std::promise<std::pair<int, std::string>> promise;
    auto future = promise.get_future();

    upstream_->send(std::move(ureq),
        [&promise](int status, std::string respBody) {
            promise.set_value({status, std::move(respBody)});
        },
        [&promise, &key, this](std::string err) {
            balancer_.reportFailure(key);
            promise.set_exception(
                std::make_exception_ptr(std::runtime_error(std::move(err))));
        });

    auto [status, respBody] = future.get();

    if (status < 200 || status >= 300) {
        balancer_.reportFailure(key);
    } else {
        balancer_.reportSuccess(key);
    }

    ProxyResponse resp;
    resp.http_status = status;
    resp.body = std::move(respBody);
    resp.content_type = "application/json";
    if (req.endpoint == "/v1/audio/speech") {
        resp.content_type = "audio/mpeg";
    }
    return resp;
}

std::string OpenAIConnector::authHeader(const std::string& key) const {
    return "Bearer " + key;
}

nlohmann::json OpenAIConnector::buildRequestBody(const ChatRequest& req) {
    nlohmann::json body;
    body["model"] = req.model;

    auto& msgs = body["messages"];
    msgs = nlohmann::json::array();
    for (const auto& m : req.messages) {
        nlohmann::json mj;
        to_json(mj, m);
        msgs.push_back(mj);
    }

    if (req.temperature.has_value()) {
        body["temperature"] = req.temperature.value();
    }
    if (req.max_tokens.has_value()) {
        body["max_tokens"] = req.max_tokens.value();
    }
    if (req.stream) {
        body["stream"] = true;
    }
    if (!req.tools.is_null() && !req.tools.empty()) {
        body["tools"] = req.tools;
    }
    if (!req.tool_choice.is_null()) {
        body["tool_choice"] = req.tool_choice;
    }

    return body;
}

ChatResponse OpenAIConnector::parseResponse(const nlohmann::json& body) {
    ChatResponse resp;
    resp.id = body.value("id", "");
    resp.model = body.value("model", "");

    if (body.contains("choices") && !body["choices"].empty()) {
        const auto& choice = body["choices"][0];
        if (choice.contains("message")) {
            const auto& msg = choice["message"];
            if (msg.contains("content") && !msg["content"].is_null()) {
                resp.content = msg["content"].get<std::string>();
            }
            if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
                for (const auto& tc : msg["tool_calls"]) {
                    ToolCall tool_call;
                    tool_call.id = tc.value("id", "");
                    tool_call.type = tc.value("type", "function");
                    if (tc.contains("function"))
                        tool_call.function = tc["function"];
                    resp.tool_calls.push_back(std::move(tool_call));
                }
            }
        }
        resp.finish_reason = choice.value("finish_reason", "");
    }

    if (body.contains("usage")) {
        resp.usage = parseUsage(body);
    }

    return resp;
}

StreamDelta OpenAIConnector::parseStreamChunk(std::string_view line) {
    StreamDelta result;
    const std::string_view prefix = "data: ";
    if (line.substr(0, prefix.size()) != prefix) {
        return result;
    }

    auto data = line.substr(prefix.size());
    if (data == "[DONE]") {
        return result;
    }

    try {
        auto j = nlohmann::json::parse(data);
        if (j.contains("choices") && !j["choices"].empty()) {
            const auto& choice = j["choices"][0];
            const auto& delta = choice["delta"];
            if (delta.contains("content") && !delta["content"].is_null()) {
                result.content = delta["content"].get<std::string>();
            }
            if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                result.tool_calls_delta = delta["tool_calls"];
            }
            if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                result.finish_reason = choice["finish_reason"].get<std::string>();
            }
        }
    } catch (const nlohmann::json::exception& e) {
        spdlog::warn("Failed to parse SSE chunk: {}", e.what());
    }
    return result;
}

TokenUsage OpenAIConnector::parseUsage(const nlohmann::json& body) {
    TokenUsage usage;
    if (body.contains("usage")) {
        const auto& u = body["usage"];
        usage.prompt_tokens = u.value("prompt_tokens", 0);
        usage.completion_tokens = u.value("completion_tokens", 0);
        usage.total_tokens = u.value("total_tokens", 0);
    }
    return usage;
}

ChatResponse OpenAIConnector::complete(const ChatRequest& req) {
    auto body = buildRequestBody(req);
    const std::string body_str = body.dump();

    // P0-3: retry across distinct healthy keys within a single request. The
    // balancer's WRR keeps returning the same key until 3 consecutive failures
    // mark it unhealthy, so we track tried keys ourselves and stop once the
    // balancer cycles back to one we already tried (= distinct pool exhausted).
    std::unordered_set<std::string> tried;
    std::string last_error = "no healthy API keys";
    // P1-A: remember the last definitive upstream HTTP status (0 = none seen,
    // i.e. only transport errors / empty pool). On exhaustion this decides
    // whether we surface the real upstream status or a pure capacity error.
    int last_status = 0;
    // P1-E: bound same-request key retries so a large key pool can't be hammered
    // on a single request. max_attempts = initial try + max_retries retries.
    const size_t max_attempts =
        config_.max_retries > 0 ? static_cast<size_t>(config_.max_retries) + 1 : 1;

    while (true) {
        auto key = balancer_.nextKey();
        if (key.empty() || tried.count(key) || tried.size() >= max_attempts) {
            if (last_status > 0) {
                throw UpstreamStatusError(
                    last_status,
                    "Upstream " + config_.name + " failed after " +
                    std::to_string(tried.size()) + " key(s): " + last_error);
            }
            throw NoHealthyKeysError(
                "No healthy API keys for " + config_.name + " (tried " +
                std::to_string(tried.size()) + " key(s)): " + last_error);
        }
        tried.insert(key);

        UpstreamRequest ureq;
        ureq.method = "POST";
        ureq.path = chatEndpoint();
        ureq.body = body_str;
        ureq.headers["Authorization"] = authHeader(key);
        ureq.headers["Content-Type"] = "application/json";
        ureq.timeout_seconds = config_.timeout_ms / 1000.0;
        Tracing::instance().injectIfEnabled(ureq.headers);

        // Sentinel status -1 carries a transport-level error through the same
        // channel as HTTP responses, so the retry decision lives in one place.
        std::promise<std::pair<int, std::string>> promise;
        auto future = promise.get_future();
        upstream_->send(std::move(ureq),
            [&promise](int status, std::string respBody) {
                promise.set_value({status, std::move(respBody)});
            },
            [&promise](std::string err) {
                promise.set_value({-1, std::move(err)});
            });

        auto [status, respBody] = future.get();

        if (status == 200) {
            balancer_.reportSuccess(key);
            try {
                return parseResponse(nlohmann::json::parse(respBody));
            } catch (const nlohmann::json::parse_error& e) {
                throw std::runtime_error(
                    std::string("Failed to parse upstream JSON: ") + e.what());
            }
        }

        balancer_.reportFailure(key);
        if (status == -1) {
            last_error = "transport error: " + respBody;
        } else {
            last_status = status;
            last_error =
                "Upstream error " + std::to_string(status) + ": " + respBody;
            // Non-retryable client errors (bad request / payload) are the
            // caller's fault — another key won't help, surface the real status.
            if (!isRetryableStatus(status)) {
                throw UpstreamStatusError(status, last_error);
            }
        }
        // Retryable (transport / 401 / 403 / 408 / 409 / 429 / 5xx): try next key.
    }
}

void OpenAIConnector::streamComplete(
    const ChatRequest& req,
    std::function<void(const StreamDelta&)> onDelta,
    std::function<void(const TokenUsage&)> onDone,
    std::function<void(const GatewayError&)> onError) {

    // REV20260707-I14 (D2 Option A): entry point sets up shared retry state
    // and delegates to attemptStream. shared_ptr ownership crosses all three
    // sendStreaming callbacks (onChunk / onDone / onError) plus recursive
    // attemptStream calls; ref-count keeps state alive until the last
    // callback fires.
    auto body = buildRequestBody(req);
    body["stream"] = true;
    auto body_str = body.dump();

    auto state = std::make_shared<StreamRetryState>();
    state->max_attempts = config_.max_retries > 0
        ? static_cast<size_t>(config_.max_retries) + 1 : 1;

    attemptStream(req, std::move(body_str), state,
                  std::move(onDelta), std::move(onDone), std::move(onError));
}

void OpenAIConnector::attemptStream(
    const ChatRequest& req,
    std::string body_str,
    std::shared_ptr<StreamRetryState> state,
    std::function<void(const StreamDelta&)> onDelta,
    std::function<void(const TokenUsage&)> onDone,
    std::function<void(const GatewayError&)> onError) {

    // Pick the next distinct key under the retry_mutex; the attempt budget
    // check keeps a large key pool from being hammered on a single request
    // (mirrors complete()'s max_attempts guard).
    // REV20260707-I14: shared exhaustion surface — mirrors complete()'s
    // "no healthy keys" vs "upstream status error" bifurcation. Called from
    // both the entry exhaustion branch and the retry-declined branch below.
    auto surface_exhaustion = [state, config_name = config_.name, onError]() {
        if (state->last_status > 0) {
            onError({state->last_status, "upstream_error",
                     "Upstream " + config_name + " streaming failed after "
                     + std::to_string(state->tried_keys.size()) + " key(s): "
                     + state->last_error, "", ""});
        } else {
            onError({toHttpStatus(ErrorCode::NoHealthyKeys),
                     toAegisCode(ErrorCode::NoHealthyKeys),
                     toErrorType(ErrorCode::NoHealthyKeys),
                     toDefaultMessage(ErrorCode::NoHealthyKeys), ""});
        }
    };

    // Match complete()'s exhaustion signature at openai.cpp:227-238: a key
    // is "exhausted" when it's empty, already tried (balancer round-tripped),
    // or the attempt budget is used up. This mirrors the non-streaming
    // retry loop and avoids an infinite loop on a small key pool.
    std::string key;
    {
        std::lock_guard<std::mutex> lk(state->retry_mutex);
        key = balancer_.nextKey();
        if (key.empty() || state->tried_keys.count(key)
            || state->tried_keys.size() >= state->max_attempts) {
            surface_exhaustion();
            return;
        }
        state->tried_keys.insert(key);
    }

    UpstreamRequest ureq;
    ureq.method = "POST";
    ureq.path = chatEndpoint();
    ureq.body = body_str;
    ureq.headers["Authorization"] = authHeader(key);
    ureq.headers["Content-Type"] = "application/json";
    ureq.timeout_seconds = config_.timeout_ms / 1000.0;
    Tracing::instance().injectIfEnabled(ureq.headers);

    auto parse_chunk = [this](std::string_view line) { return parseStreamChunk(line); };
    auto parse_usage = [this](const nlohmann::json& j) { return parseUsage(j); };
    auto* bal = &balancer_;
    auto* self = this;

    upstream_->sendStreaming(std::move(ureq),
        // onChunk: mark first_chunk_seen (release order) on first fire, then
        // forward the parsed delta upstream. Idempotent write, no CAS needed.
        [state, parse_chunk, onDelta](std::string_view line) {
            state->first_chunk_seen.store(true, std::memory_order_release);
            auto delta = parse_chunk(line);
            if (!delta.content.empty() || !delta.tool_calls_delta.is_null()
                || !delta.finish_reason.empty()) {
                onDelta(delta);
            }
        },
        // onDone: on 200 success, parse trailing usage and forward. On
        // non-200 with no chunks emitted yet, check canRetryStream and
        // either retry with next key or surface the error.
        [self, req, body_str, key, bal, state, parse_usage, onDelta, onDone,
         onError, surface_exhaustion]
        (int status, std::string respBody) mutable {
            if (status == 200) {
                bal->reportSuccess(key);
                TokenUsage usage;
                std::istringstream stream(respBody);
                std::string line;
                while (std::getline(stream, line)) {
                    if (line.empty() || line == "\r") continue;
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.find("\"usage\"") != std::string::npos) {
                        try {
                            auto data_pos = line.find("data: ");
                            if (data_pos != std::string::npos) {
                                auto j = nlohmann::json::parse(line.substr(data_pos + 6));
                                if (j.contains("usage")) {
                                    usage = parse_usage(j);
                                }
                            }
                        } catch (const std::exception& e) {
                            spdlog::debug("SSE usage parse skipped: {}", e.what());
                        }
                    }
                }
                onDone(usage);
                return;
            }

            bal->reportFailure(key);
            const bool seen = state->first_chunk_seen.load(std::memory_order_acquire);
            size_t tried_size = 0;
            {
                std::lock_guard<std::mutex> lk(state->retry_mutex);
                state->last_status = status;
                state->last_error = "Upstream error " + std::to_string(status)
                    + ": " + respBody;
                tried_size = state->tried_keys.size();
            }
            if (canRetryStream(seen, status, tried_size, state->max_attempts)) {
                self->attemptStream(req, body_str, state,
                                    std::move(onDelta), std::move(onDone),
                                    std::move(onError));
                return;
            }
            // If seen == false and canRetryStream is false due to budget
            // exhaustion, prefer the aggregate exhaustion message (matches
            // complete()'s error shape). Otherwise (mid-stream already
            // committed to client), surface the terminal status.
            if (!seen) {
                surface_exhaustion();
            } else {
                onError({status, "upstream_error", respBody, "", ""});
            }
        },
        // onError: transport-level failure. Same pre-stream retry logic;
        // if the first chunk was emitted before the transport failed, we
        // must surface the error rather than retry (SSE is committed).
        [self, req, body_str, key, bal, state, onDelta, onDone, onError,
         surface_exhaustion]
        (std::string err) mutable {
            bal->reportFailure(key);
            const bool seen = state->first_chunk_seen.load(std::memory_order_acquire);
            size_t tried_size = 0;
            {
                std::lock_guard<std::mutex> lk(state->retry_mutex);
                state->last_status = -1;
                state->last_error = "transport error: " + err;
                tried_size = state->tried_keys.size();
            }
            if (canRetryStream(seen, -1, tried_size, state->max_attempts)) {
                self->attemptStream(req, body_str, state,
                                    std::move(onDelta), std::move(onDone),
                                    std::move(onError));
                return;
            }
            if (!seen) {
                surface_exhaustion();
            } else {
                onError({502, "upstream_error", std::move(err), "", ""});
            }
        });
}

} // namespace aegisgate
