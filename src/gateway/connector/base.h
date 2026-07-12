#pragma once
#include "aegisgate/types.h"
#include "core/context.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace aegisgate {

enum class Capability {
    Streaming,
    Tools,
    ResponseFormat,
    Logprobs,
    Vision,
    SystemMessage,
    Temperature,
    TopP,
    MaxTokens
};

struct ModelCapabilities {
    std::unordered_set<Capability> supported;

    bool has(Capability cap) const {
        return supported.count(cap) > 0;
    }
};

// P0-3: HTTP statuses worth retrying on a *different* API key of the same
// provider within one request. 401/403 → key may be invalid/forbidden;
// 408/409/429 → transient or per-key rate limit; 5xx → upstream transient.
// Other 4xx are request-level faults another key won't fix.
inline bool isRetryableStatus(int status) {
    return status == 401 || status == 403 || status == 408 ||
           status == 409 || status == 429 || status >= 500;
}

// REV20260707-I14 (D2 Option A): shared retry state for streaming connectors
// (OpenAI / Claude / future providers). Once the first stream chunk has been
// emitted to the client via onChunk, the stream is committed to the client
// and cannot retry with a different key (preserves SSE semantics). Before
// the first chunk, transport-level errors and retryable HTTP status codes
// can transparently retry with the next key, mirroring the non-streaming
// complete() while-loop retry family. Passed via std::shared_ptr so the
// three sendStreaming callbacks (onChunk / onDone / onError) share lifetime
// and state across the recursive attemptStream call chain.
struct StreamRetryState {
    // Set once by onChunk on its first fire (release order). Read by
    // onDone / onError (acquire order) to decide retry eligibility. No CAS
    // needed: idempotent write, sendStreaming contract guarantees onDone
    // and onError do not both fire.
    std::atomic<bool> first_chunk_seen{false};

    // Guarded by retry_mutex. Fields describe the retry history for the
    // current request across all key attempts so far.
    std::unordered_set<std::string> tried_keys;
    std::string last_error;
    int last_status = 0;      // 0 = first attempt (no prior failure);
                               // -1 = transport-level error; HTTP status otherwise.

    // Immutable after construction.
    size_t max_attempts = 1;   // max_retries + 1, matching complete().

    mutable std::mutex retry_mutex;
};

// REV20260707-I14 (D2 Option A): predicate deciding whether a streaming
// request may be retried with the next API key. Layer 1 test target with a
// 6-case truth table. Extracted per the "predicate-extraction family"
// pattern (N=4 confirmation: shouldWireRedisRateLimiter,
// shouldWireGuardExplanation, isAdvancedRoutingEnabled, canRetryStream).
inline bool canRetryStream(bool first_chunk_seen, int last_status,
                            size_t tried_size, size_t max_attempts) {
    if (first_chunk_seen) return false;           // Stream committed to client.
    if (tried_size >= max_attempts) return false; // Attempt budget exhausted.
    if (last_status == 0) return true;            // First attempt.
    if (last_status == -1) return true;           // Transport-level error.
    return isRetryableStatus(last_status);        // HTTP status retryability.
}

struct ModelInfo {
    std::string id;
    std::string provider;
    double cost_per_1k_input = 0.0;
    double cost_per_1k_output = 0.0;
    int max_context_tokens = 4096;
    std::vector<std::string> tags;
    ModelCapabilities capabilities;
};

struct ProviderConfig {
    std::string name;
    std::string type;
    std::string base_url;
    std::vector<std::pair<std::string, int>> api_keys; // key, weight
    std::vector<ModelInfo> models;
    int timeout_ms = 30000;
    int max_retries = 2;  // P1-E: caps same-request key retries in complete() (initial try + max_retries)
};

class ModelConnector {
public:
    virtual ~ModelConnector() = default;

    virtual ChatResponse complete(const ChatRequest& req) = 0;

    // Design note: CurlUpstreamClient now handles true streaming. Incremental
    // usage accumulation could be added here if providers expose partial usage.
    virtual void streamComplete(
        const ChatRequest& req,
        std::function<void(const StreamDelta&)> onDelta,
        std::function<void(const TokenUsage&)> onDone,
        std::function<void(const GatewayError&)> onError) = 0;

    virtual std::string provider() const = 0;
    virtual bool supportsModel(const std::string& model) const = 0;
    virtual ModelCapabilities capabilities(const std::string& model) const = 0;

    virtual bool supportsEndpoint(const std::string& /*endpoint*/) const {
        return false;
    }
    virtual ProxyResponse proxyRequest(const ProxyRequest& /*req*/) {
        return {404, R"({"error":{"message":"Endpoint not supported by this provider"}})", "application/json"};
    }

protected:
    virtual nlohmann::json buildRequestBody(const ChatRequest& req) = 0;
    virtual ChatResponse parseResponse(const nlohmann::json& body) = 0;
    virtual StreamDelta parseStreamChunk(std::string_view chunk) = 0;
    virtual TokenUsage parseUsage(const nlohmann::json& body) = 0;
};

} // namespace aegisgate
