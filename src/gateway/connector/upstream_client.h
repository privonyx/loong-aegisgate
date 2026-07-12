#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace aegisgate {

struct UpstreamRequest {
    std::string method;
    std::string path;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    double timeout_seconds = 30.0;
};

class UpstreamClient {
public:
    virtual ~UpstreamClient() = default;

    using ResponseCallback = std::function<void(int status, std::string body)>;
    using ChunkCallback = std::function<void(std::string_view chunk)>;
    using ErrorCallback = std::function<void(std::string error)>;

    virtual void send(UpstreamRequest req, ResponseCallback onDone,
                      ErrorCallback onError) = 0;

    /// CurlUpstreamClient provides true per-chunk streaming since phase 0.2.
    /// Incremental usage accumulation from provider-specific final chunks
    /// (OpenAI final chunk usage, Anthropic message_delta.usage) can be
    /// added when provider connectors expose partial usage data.
    virtual void sendStreaming(UpstreamRequest req, ChunkCallback onChunk,
                               ResponseCallback onDone,
                               ErrorCallback onError) = 0;
};

} // namespace aegisgate
