#pragma once
#include "base.h"
#include "upstream_client.h"
#include "gateway/balancer.h"
#include <memory>
#include <functional>

namespace aegisgate {

class ClaudeConnector : public ModelConnector {
public:
    explicit ClaudeConnector(const ProviderConfig& config,
                             std::unique_ptr<UpstreamClient> upstream = nullptr);

    ChatResponse complete(const ChatRequest& req) override;
    void streamComplete(
        const ChatRequest& req,
        std::function<void(const StreamDelta&)> onDelta,
        std::function<void(const TokenUsage&)> onDone,
        std::function<void(const GatewayError&)> onError) override;

    // REV20260707-I14 (D2 Option A): recursive attempt driver mirroring
    // OpenAIConnector::attemptStream. See openai.h for design rationale.
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

    nlohmann::json buildRequestBody(const ChatRequest& req) override;
    ChatResponse parseResponse(const nlohmann::json& body) override;
    StreamDelta parseStreamChunk(std::string_view line) override;
    TokenUsage parseUsage(const nlohmann::json& body) override;

protected:
    ProviderConfig config_;
    Balancer balancer_;
    std::unique_ptr<UpstreamClient> upstream_;
};

} // namespace aegisgate
