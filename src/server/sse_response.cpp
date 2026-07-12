#include "sse_response.h"
#include <spdlog/spdlog.h>

namespace aegisgate {

SseResponseWriter::SseResponseWriter(
    std::function<void(const drogon::HttpResponsePtr&)> callback)
    : callback_(std::move(callback)), is_streaming_(false) {}

SseResponseWriter::SseResponseWriter(drogon::ResponseStreamPtr stream)
    : stream_(std::move(stream)), is_streaming_(true) {}

std::string SseResponseWriter::formatSseChunk(
    std::string_view content,
    const std::string& model,
    const std::string& chunk_id) {

    nlohmann::json j;
    j["id"] = chunk_id;
    j["object"] = "chat.completion.chunk";
    j["model"] = model;
    j["choices"] = nlohmann::json::array();
    j["choices"].push_back({
        {"index", 0},
        {"delta", {{"content", std::string(content)}}},
        {"finish_reason", nullptr}
    });

    return "data: " + j.dump() + "\n\n";
}

std::string SseResponseWriter::formatSseDelta(
    const StreamDelta& delta,
    const std::string& model,
    const std::string& chunk_id) {

    nlohmann::json j;
    j["id"] = chunk_id;
    j["object"] = "chat.completion.chunk";
    j["model"] = model;

    nlohmann::json delta_json;
    if (!delta.content.empty()) {
        delta_json["content"] = delta.content;
    }
    if (!delta.tool_calls_delta.is_null() && delta.tool_calls_delta.is_array()) {
        delta_json["tool_calls"] = delta.tool_calls_delta;
    }

    nlohmann::json choice;
    choice["index"] = 0;
    choice["delta"] = delta_json;
    choice["finish_reason"] = delta.finish_reason.empty()
        ? nlohmann::json(nullptr) : nlohmann::json(delta.finish_reason);

    j["choices"] = nlohmann::json::array({choice});

    return "data: " + j.dump() + "\n\n";
}

std::string SseResponseWriter::formatSseDone() {
    return "data: [DONE]\n\n";
}

std::string SseResponseWriter::formatSseError(const GatewayError& error) {
    nlohmann::json j;
    j["error"]["code"] = error.error_code;
    j["error"]["type"] = error.error_type;
    j["error"]["message"] = error.message;
    return "data: " + j.dump() + "\n\n";
}

void SseResponseWriter::writeChunk(std::string_view content,
                                    const std::string& model) {
    auto chunk_id = "chatcmpl-stream-" + std::to_string(chunk_index_++);
    auto formatted = formatSseChunk(content, model, chunk_id);

    if (is_streaming_ && stream_) {
        stream_->send(formatted);
    } else {
        buffer_ += formatted;
    }
}

void SseResponseWriter::writeDelta(const StreamDelta& delta,
                                    const std::string& model) {
    auto chunk_id = "chatcmpl-stream-" + std::to_string(chunk_index_++);
    auto formatted = formatSseDelta(delta, model, chunk_id);

    if (is_streaming_ && stream_) {
        stream_->send(formatted);
    } else {
        buffer_ += formatted;
    }
}

void SseResponseWriter::writeDone(const TokenUsage& usage,
                                   const std::string& /*model*/,
                                   int tokens_saved) {
    if (usage.total_tokens > 0 || tokens_saved > 0) {
        nlohmann::json meta;
        meta["aegisgate"]["tokens_saved"] = tokens_saved;
        meta["aegisgate"]["usage"]["prompt_tokens"] = usage.prompt_tokens;
        meta["aegisgate"]["usage"]["completion_tokens"] = usage.completion_tokens;
        meta["aegisgate"]["usage"]["total_tokens"] = usage.total_tokens;
        auto event = "data: " + meta.dump() + "\n\n";
        if (is_streaming_ && stream_) stream_->send(event);
        else buffer_ += event;
    }

    auto done = formatSseDone();

    if (is_streaming_ && stream_) {
        stream_->send(done);
        stream_->close();
        stream_.reset();
    } else if (callback_) {
        buffer_ += done;
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k200OK);
        resp->setContentTypeString("text/event-stream");
        resp->addHeader("Cache-Control", "no-cache");
        resp->addHeader("Connection", "keep-alive");
        resp->setBody(buffer_);
        callback_(resp);
    }
}

void SseResponseWriter::writeError(const GatewayError& error) {
    if (is_streaming_ && stream_) {
        // Headers (HTTP 200) are already flushed for an open SSE stream, so the
        // status can no longer change. P1-D: emit a structured error event and
        // then a terminal [DONE] so clients that loop until [DONE] finalize
        // cleanly instead of hanging / treating the stream as truncated.
        stream_->send(formatSseError(error));
        stream_->send(formatSseDone());
        stream_->close();
        stream_.reset();
    } else if (callback_) {
        // Pre-stream failure (no chunk sent yet): return a real HTTP error with
        // the proper status code and a JSON body.
        nlohmann::json j;
        j["error"]["code"] = error.error_code;
        j["error"]["type"] = error.error_type;
        j["error"]["message"] = error.message;
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(static_cast<drogon::HttpStatusCode>(error.http_status));
        resp->setContentTypeString("application/json");
        resp->setBody(j.dump());
        callback_(resp);
    }
}

} // namespace aegisgate
