#pragma once
#include "aegisgate/types.h"
#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <string_view>

namespace aegisgate {

class SseResponseWriter {
public:
    explicit SseResponseWriter(
        std::function<void(const drogon::HttpResponsePtr&)> callback);

    explicit SseResponseWriter(drogon::ResponseStreamPtr stream);

    void writeChunk(std::string_view content, const std::string& model);
    void writeDelta(const StreamDelta& delta, const std::string& model);
    void writeDone(const TokenUsage& usage, const std::string& model,
                   int tokens_saved = 0);
    void writeError(const GatewayError& error);

    static std::string formatSseChunk(
        std::string_view content,
        const std::string& model,
        const std::string& chunk_id);

    static std::string formatSseDelta(
        const StreamDelta& delta,
        const std::string& model,
        const std::string& chunk_id);

    static std::string formatSseDone();

    static std::string formatSseError(const GatewayError& error);

private:
    std::function<void(const drogon::HttpResponsePtr&)> callback_;
    drogon::ResponseStreamPtr stream_;
    bool is_streaming_ = false;
    std::string buffer_;
    int chunk_index_ = 0;
};

} // namespace aegisgate
