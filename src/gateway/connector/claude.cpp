#include "claude.h"
#include "default_upstream.h"
#include "observe/tracing.h"
#include <aegisgate/error_codes.h>
#include <future>
#include <spdlog/spdlog.h>
#include <sstream>
#include <unordered_set>

namespace aegisgate {

ClaudeConnector::ClaudeConnector(const ProviderConfig& config,
                                 std::unique_ptr<UpstreamClient> upstream)
    : config_(config), balancer_(config.api_keys),
      upstream_(upstream ? std::move(upstream)
                         : makeDefaultUpstreamClient(config.base_url)) {}

std::string ClaudeConnector::provider() const {
    return config_.name;
}

bool ClaudeConnector::supportsModel(const std::string& model) const {
    for (const auto& m : config_.models) {
        if (m.id == model) return true;
    }
    return false;
}

ModelCapabilities ClaudeConnector::capabilities(const std::string& /*model*/) const {
    return {{Capability::Streaming, Capability::Tools, Capability::Vision,
             Capability::SystemMessage, Capability::Temperature,
             Capability::TopP, Capability::MaxTokens}};
}

nlohmann::json ClaudeConnector::buildRequestBody(const ChatRequest& req) {
    nlohmann::json body;
    body["model"] = req.model;

    std::string system_msg;
    auto& msgs = body["messages"];
    msgs = nlohmann::json::array();
    for (const auto& m : req.messages) {
        if (m.role == "system") {
            system_msg = m.content;
        } else if (m.role == "assistant" && !m.tool_calls.empty()) {
            nlohmann::json mj;
            mj["role"] = "assistant";
            nlohmann::json content_arr = nlohmann::json::array();
            if (!m.content.empty()) {
                content_arr.push_back({{"type", "text"}, {"text", m.content}});
            }
            for (const auto& tc : m.tool_calls) {
                nlohmann::json tu;
                tu["type"] = "tool_use";
                tu["id"] = tc.id;
                tu["name"] = tc.function.value("name", "");
                auto args_str = tc.function.value("arguments", "");
                tu["input"] = args_str.empty()
                    ? nlohmann::json::object()
                    : nlohmann::json::parse(args_str, nullptr, false);
                if (tu["input"].is_discarded()) tu["input"] = nlohmann::json::object();
                content_arr.push_back(tu);
            }
            mj["content"] = content_arr;
            msgs.push_back(mj);
        } else if (m.role == "tool") {
            nlohmann::json mj;
            mj["role"] = "user";
            mj["content"] = nlohmann::json::array({
                {{"type", "tool_result"},
                 {"tool_use_id", m.tool_call_id},
                 {"content", m.content}}
            });
            msgs.push_back(mj);
        } else {
            nlohmann::json mj;
            mj["role"] = m.role;
            mj["content"] = m.content;
            msgs.push_back(mj);
        }
    }
    if (!system_msg.empty()) {
        body["system"] = system_msg;
    }

    body["max_tokens"] = req.max_tokens.value_or(4096);

    if (req.temperature.has_value()) {
        body["temperature"] = req.temperature.value();
    }
    if (req.stream) {
        body["stream"] = true;
    }

    if (!req.tools.is_null() && req.tools.is_array()) {
        nlohmann::json claude_tools = nlohmann::json::array();
        for (const auto& tool : req.tools) {
            if (tool.contains("function")) {
                nlohmann::json ct;
                ct["name"] = tool["function"].value("name", "");
                ct["description"] = tool["function"].value("description", "");
                if (tool["function"].contains("parameters"))
                    ct["input_schema"] = tool["function"]["parameters"];
                else
                    ct["input_schema"] = {{"type", "object"}, {"properties", nlohmann::json::object()}};
                claude_tools.push_back(ct);
            }
        }
        if (!claude_tools.empty()) body["tools"] = claude_tools;
    }

    if (!req.tool_choice.is_null()) {
        if (req.tool_choice.is_string()) {
            auto tc = req.tool_choice.get<std::string>();
            if (tc == "auto") body["tool_choice"] = {{"type", "auto"}};
            else if (tc == "none") body["tool_choice"] = {{"type", "none"}};
            else if (tc == "required") body["tool_choice"] = {{"type", "any"}};
        } else if (req.tool_choice.is_object()) {
            if (req.tool_choice.contains("function")) {
                body["tool_choice"] = {
                    {"type", "tool"},
                    {"name", req.tool_choice["function"].value("name", "")}
                };
            }
        }
    }

    return body;
}

ChatResponse ClaudeConnector::parseResponse(const nlohmann::json& body) {
    ChatResponse resp;
    resp.id = body.value("id", "");
    resp.model = body.value("model", "");

    if (body.contains("content") && body["content"].is_array()) {
        for (const auto& block : body["content"]) {
            auto block_type = block.value("type", "");
            if (block_type == "text") {
                resp.content += block.value("text", "");
            } else if (block_type == "tool_use") {
                ToolCall tc;
                tc.id = block.value("id", "");
                tc.type = "function";
                nlohmann::json fn;
                fn["name"] = block.value("name", "");
                fn["arguments"] = block.contains("input")
                    ? block["input"].dump() : "{}";
                tc.function = fn;
                resp.tool_calls.push_back(std::move(tc));
            }
        }
    }

    auto stop_reason = body.value("stop_reason", "");
    if (stop_reason == "tool_use") {
        resp.finish_reason = "tool_calls";
    } else {
        resp.finish_reason = stop_reason;
    }

    if (body.contains("usage")) {
        resp.usage = parseUsage(body);
    }

    return resp;
}

StreamDelta ClaudeConnector::parseStreamChunk(std::string_view line) {
    StreamDelta result;
    const std::string_view prefix = "data: ";
    if (line.substr(0, prefix.size()) != prefix) {
        return result;
    }

    auto data = line.substr(prefix.size());
    try {
        auto j = nlohmann::json::parse(data);
        auto event_type = j.value("type", "");

        if (event_type == "content_block_delta") {
            if (j.contains("delta")) {
                auto delta_type = j["delta"].value("type", "");
                if (delta_type == "text_delta") {
                    result.content = j["delta"].value("text", "");
                } else if (delta_type == "input_json_delta") {
                    auto idx = j.value("index", 0);
                    nlohmann::json tc_delta = nlohmann::json::array();
                    tc_delta.push_back({
                        {"index", idx},
                        {"function", {{"arguments", j["delta"].value("partial_json", "")}}}
                    });
                    result.tool_calls_delta = tc_delta;
                }
            }
        } else if (event_type == "content_block_start") {
            if (j.contains("content_block")) {
                auto block_type = j["content_block"].value("type", "");
                if (block_type == "tool_use") {
                    auto idx = j.value("index", 0);
                    nlohmann::json tc_delta = nlohmann::json::array();
                    tc_delta.push_back({
                        {"index", idx},
                        {"id", j["content_block"].value("id", "")},
                        {"type", "function"},
                        {"function", {
                            {"name", j["content_block"].value("name", "")},
                            {"arguments", ""}
                        }}
                    });
                    result.tool_calls_delta = tc_delta;
                }
            }
        } else if (event_type == "message_delta") {
            if (j.contains("delta")) {
                auto stop = j["delta"].value("stop_reason", "");
                if (stop == "tool_use") result.finish_reason = "tool_calls";
                else if (!stop.empty()) result.finish_reason = stop;
            }
        }
    } catch (const nlohmann::json::exception& e) {
        spdlog::warn("Failed to parse Claude SSE: {}", e.what());
    }
    return result;
}

TokenUsage ClaudeConnector::parseUsage(const nlohmann::json& body) {
    TokenUsage usage;
    if (body.contains("usage")) {
        const auto& u = body["usage"];
        usage.prompt_tokens = u.value("input_tokens", 0);
        usage.completion_tokens = u.value("output_tokens", 0);
        usage.total_tokens = usage.prompt_tokens + usage.completion_tokens;
    }
    return usage;
}

ChatResponse ClaudeConnector::complete(const ChatRequest& req) {
    auto body = buildRequestBody(req);
    const std::string body_str = body.dump();

    // P0-3: retry across distinct healthy keys within a single request (see the
    // OpenAIConnector::complete comment for the rationale on the tried-set bound).
    std::unordered_set<std::string> tried;
    std::string last_error = "no healthy API keys";
    int last_status = 0;  // P1-A: see OpenAIConnector::complete for rationale
    // P1-E: bound same-request key retries (initial try + max_retries retries).
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
        ureq.path = "/v1/messages";
        ureq.body = body_str;
        ureq.headers["x-api-key"] = key;
        ureq.headers["anthropic-version"] = "2023-06-01";
        ureq.headers["Content-Type"] = "application/json";
        ureq.timeout_seconds = config_.timeout_ms / 1000.0;
        Tracing::instance().injectIfEnabled(ureq.headers);

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
            last_error = "Upstream error " + std::to_string(status) + ": " + respBody;
            if (!isRetryableStatus(status)) {
                throw UpstreamStatusError(status, last_error);
            }
        }
    }
}

void ClaudeConnector::streamComplete(
    const ChatRequest& req,
    std::function<void(const StreamDelta&)> onDelta,
    std::function<void(const TokenUsage&)> onDone,
    std::function<void(const GatewayError&)> onError) {

    // REV20260707-I14 (D2 Option A): mirrors OpenAIConnector::streamComplete;
    // see openai.cpp for the recursion/lifetime/atomicity design.
    auto body = buildRequestBody(req);
    body["stream"] = true;
    auto body_str = body.dump();

    auto state = std::make_shared<StreamRetryState>();
    state->max_attempts = config_.max_retries > 0
        ? static_cast<size_t>(config_.max_retries) + 1 : 1;

    attemptStream(req, std::move(body_str), state,
                  std::move(onDelta), std::move(onDone), std::move(onError));
}

void ClaudeConnector::attemptStream(
    const ChatRequest& req,
    std::string body_str,
    std::shared_ptr<StreamRetryState> state,
    std::function<void(const StreamDelta&)> onDelta,
    std::function<void(const TokenUsage&)> onDone,
    std::function<void(const GatewayError&)> onError) {

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
    ureq.path = "/v1/messages";
    ureq.body = body_str;
    ureq.headers["x-api-key"] = key;
    ureq.headers["anthropic-version"] = "2023-06-01";
    ureq.headers["Content-Type"] = "application/json";
    ureq.timeout_seconds = config_.timeout_ms / 1000.0;
    Tracing::instance().injectIfEnabled(ureq.headers);

    auto parse_chunk = [this](std::string_view line) { return parseStreamChunk(line); };
    auto* bal = &balancer_;
    auto* self = this;

    upstream_->sendStreaming(std::move(ureq),
        [state, parse_chunk, onDelta](std::string_view line) {
            state->first_chunk_seen.store(true, std::memory_order_release);
            auto delta = parse_chunk(line);
            if (!delta.content.empty() || !delta.tool_calls_delta.is_null()
                || !delta.finish_reason.empty()) {
                onDelta(delta);
            }
        },
        [self, req, body_str, key, bal, state, onDelta, onDone, onError,
         surface_exhaustion]
        (int status, std::string respBody) mutable {
            if (status == 200) {
                bal->reportSuccess(key);
                TokenUsage usage;
                std::istringstream stream(respBody);
                std::string line;
                while (std::getline(stream, line)) {
                    if (line.empty() || line == "\r") continue;
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.find("\"message_delta\"") != std::string::npos ||
                        line.find("\"message_stop\"") != std::string::npos ||
                        line.find("\"usage\"") != std::string::npos) {
                        try {
                            auto data_pos = line.find("data: ");
                            if (data_pos != std::string::npos) {
                                auto j = nlohmann::json::parse(line.substr(data_pos + 6));
                                if (j.contains("usage")) {
                                    const auto& u = j["usage"];
                                    usage.prompt_tokens =
                                        u.value("input_tokens", usage.prompt_tokens);
                                    usage.completion_tokens =
                                        u.value("output_tokens", usage.completion_tokens);
                                    usage.total_tokens =
                                        usage.prompt_tokens + usage.completion_tokens;
                                }
                            }
                        } catch (...) {
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
            if (!seen) {
                surface_exhaustion();
            } else {
                onError({status, "upstream_error", respBody, "", ""});
            }
        },
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
