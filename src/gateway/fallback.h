#pragma once
#include "circuit_breaker.h"
#include "connector/base.h"
#include "connector/registry.h"
#include <aegisgate/error_codes.h>
#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include <functional>

namespace aegisgate {

struct FallbackChain {
    std::vector<std::string> models;
};

struct StreamFallbackState {
    ChatRequest base_req;
    std::vector<std::string> candidates;
    std::function<void(const StreamDelta&)> onDelta;
    std::function<void(const TokenUsage&)> onDone;
    std::function<void(const GatewayError&)> onError;
    std::atomic<bool> chunks_sent{false};
    // P1-D: distinguish "every candidate was skipped because its circuit was
    // open" (nothing attempted) from "candidates attempted and failed", so the
    // terminal SSE error mirrors the non-streaming circuit-open semantics.
    std::atomic<bool> any_attempt{false};
    std::atomic<bool> any_circuit_open{false};
    GatewayError last_err{
        502,
        toAegisCode(ErrorCode::UpstreamError),
        toErrorType(ErrorCode::UpstreamError),
        toDefaultMessage(ErrorCode::UpstreamError),
        "All models in fallback chain failed"};
};

class FallbackManager {
public:
    explicit FallbackManager(ConnectorRegistry& registry);
    FallbackManager(ConnectorRegistry& registry, CircuitConfig circuit_config);

    void setChain(const std::string& primary, const std::vector<std::string>& fallbacks);

    ChatResponse executeWithFallback(
        const ChatRequest& req,
        const std::string& primary_model);

    void streamWithFallback(
        const ChatRequest& req,
        const std::string& primary_model,
        std::function<void(const StreamDelta&)> onDelta,
        std::function<void(const TokenUsage&)> onDone,
        std::function<void(const GatewayError&)> onError);

    std::vector<std::string> getChain(const std::string& model) const;
    const CircuitBreaker& circuitBreaker() const { return circuit_breaker_; }
    CircuitBreaker& mutableCircuitBreaker() { return circuit_breaker_; }

private:
    void tryStreamModel(std::shared_ptr<StreamFallbackState> state, size_t idx);

    ConnectorRegistry& registry_;
    std::unordered_map<std::string, FallbackChain> chains_;
    CircuitBreaker circuit_breaker_;
};

} // namespace aegisgate
