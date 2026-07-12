#pragma once
#include "base.h"
#include "upstream_client.h"
#include "gateway/balancer.h"
#include <memory>
#include <mutex>
#include <functional>

namespace aegisgate {

class OpenAIConnector : public ModelConnector {
public:
    explicit OpenAIConnector(const ProviderConfig& config,
                             std::unique_ptr<UpstreamClient> upstream = nullptr);

    ChatResponse complete(const ChatRequest& req) override;
    void streamComplete(
        const ChatRequest& req,
        std::function<void(const StreamDelta&)> onDelta,
        std::function<void(const TokenUsage&)> onDone,
        std::function<void(const GatewayError&)> onError) override;

    // REV20260707-I14 (D2 Option A): recursive attempt driver used by
    // streamComplete for pre-stream multi-API-key retry. Public for test
    // visibility of the retry loop; typical callers should use
    // streamComplete which sets up the shared StreamRetryState.
    void attemptStream(
        const ChatRequest& req,
        std::string body_str,
        std::shared_ptr<StreamRetryState> state,
        std::function<void(const StreamDelta&)> onDelta,
        std::function<void(const TokenUsage&)> onDone,
        std::function<void(const GatewayError&)> onError);

    std::string provider() const override;
    bool supportsModel(const std::string& model) const override;
    ModelCapabilities capabilities(const std::string& model) const override;

    bool supportsEndpoint(const std::string& endpoint) const override;
    ProxyResponse proxyRequest(const ProxyRequest& req) override;

    nlohmann::json buildRequestBody(const ChatRequest& req) override;
    ChatResponse parseResponse(const nlohmann::json& body) override;
    StreamDelta parseStreamChunk(std::string_view line) override;
    TokenUsage parseUsage(const nlohmann::json& body) override;

protected:
    virtual std::string chatEndpoint() const;
    virtual std::string authHeader(const std::string& key) const;

    ProviderConfig config_;
    Balancer balancer_;
    std::unique_ptr<UpstreamClient> upstream_;
};

} // namespace aegisgate
