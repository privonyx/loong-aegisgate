#include <chrono>
#include <gtest/gtest.h>
#include "gateway/fallback.h"
#include "gateway/connector/base.h"
#include <memory>

using namespace aegisgate;

class SuccessConnector : public ModelConnector {
public:
    explicit SuccessConnector(std::string model) : model_(std::move(model)) {}
    ChatResponse complete(const ChatRequest& req) override {
        ChatResponse r;
        r.model = req.model;
        r.content = "response from " + req.model;
        return r;
    }
    void streamComplete(const ChatRequest&, std::function<void(const StreamDelta&)>,
                        std::function<void(const TokenUsage&)>,
                        std::function<void(const GatewayError&)>) override {}
    std::string provider() const override { return "test"; }
    bool supportsModel(const std::string& m) const override { return m == model_; }
    ModelCapabilities capabilities(const std::string&) const override { return {}; }
protected:
    nlohmann::json buildRequestBody(const ChatRequest&) override { return {}; }
    ChatResponse parseResponse(const nlohmann::json&) override { return {}; }
    StreamDelta parseStreamChunk(std::string_view) override { return {}; }
    TokenUsage parseUsage(const nlohmann::json&) override { return {}; }
    std::string model_;
};

class FailConnector : public ModelConnector {
public:
    explicit FailConnector(std::string model) : model_(std::move(model)) {}
    ChatResponse complete(const ChatRequest&) override {
        throw std::runtime_error("model unavailable");
    }
    void streamComplete(const ChatRequest&, std::function<void(const StreamDelta&)>,
                        std::function<void(const TokenUsage&)>,
                        std::function<void(const GatewayError&)>) override {}
    std::string provider() const override { return "test"; }
    bool supportsModel(const std::string& m) const override { return m == model_; }
    ModelCapabilities capabilities(const std::string&) const override { return {}; }
protected:
    nlohmann::json buildRequestBody(const ChatRequest&) override { return {}; }
    ChatResponse parseResponse(const nlohmann::json&) override { return {}; }
    StreamDelta parseStreamChunk(std::string_view) override { return {}; }
    TokenUsage parseUsage(const nlohmann::json&) override { return {}; }
    std::string model_;
};

// P1-A: a connector that always fails with a typed upstream HTTP status.
class StatusConnector : public ModelConnector {
public:
    StatusConnector(std::string model, int status)
        : model_(std::move(model)), status_(status) {}
    ChatResponse complete(const ChatRequest&) override {
        throw UpstreamStatusError(status_, "upstream " + std::to_string(status_));
    }
    void streamComplete(const ChatRequest&, std::function<void(const StreamDelta&)>,
                        std::function<void(const TokenUsage&)>,
                        std::function<void(const GatewayError&)>) override {}
    std::string provider() const override { return "test"; }
    bool supportsModel(const std::string& m) const override { return m == model_; }
    ModelCapabilities capabilities(const std::string&) const override { return {}; }
protected:
    nlohmann::json buildRequestBody(const ChatRequest&) override { return {}; }
    ChatResponse parseResponse(const nlohmann::json&) override { return {}; }
    StreamDelta parseStreamChunk(std::string_view) override { return {}; }
    TokenUsage parseUsage(const nlohmann::json&) override { return {}; }
    std::string model_;
    int status_;
};

// P1-D: a streaming connector that always fails through the onError callback
// (no chunk delivered), so the fallback chain can advance / finalize.
class StreamFailConnector : public ModelConnector {
public:
    explicit StreamFailConnector(std::string model) : model_(std::move(model)) {}
    ChatResponse complete(const ChatRequest&) override {
        throw std::runtime_error("model unavailable");
    }
    void streamComplete(const ChatRequest&,
                        std::function<void(const StreamDelta&)>,
                        std::function<void(const TokenUsage&)>,
                        std::function<void(const GatewayError&)> onError) override {
        onError(GatewayError{502, "AEGIS-5001", "upstream_error",
                             "stream upstream failed", ""});
    }
    std::string provider() const override { return "test"; }
    bool supportsModel(const std::string& m) const override { return m == model_; }
    ModelCapabilities capabilities(const std::string&) const override { return {}; }
protected:
    nlohmann::json buildRequestBody(const ChatRequest&) override { return {}; }
    ChatResponse parseResponse(const nlohmann::json&) override { return {}; }
    StreamDelta parseStreamChunk(std::string_view) override { return {}; }
    TokenUsage parseUsage(const nlohmann::json&) override { return {}; }
    std::string model_;
};

TEST(FallbackTest, PrimarySucceeds) {
    ConnectorRegistry registry;
    registry.registerConnector(std::make_unique<SuccessConnector>("gpt-4o"));
    registry.registerConnector(std::make_unique<SuccessConnector>("gpt-4o-mini"));

    FallbackManager fb(registry);
    fb.setChain("gpt-4o", {"gpt-4o-mini"});

    ChatRequest req;
    req.model = "gpt-4o";
    auto resp = fb.executeWithFallback(req, "gpt-4o");
    EXPECT_EQ(resp.model, "gpt-4o");
}

TEST(FallbackTest, PrimaryFailsFallbackSucceeds) {
    ConnectorRegistry registry;
    registry.registerConnector(std::make_unique<FailConnector>("gpt-4o"));
    registry.registerConnector(std::make_unique<SuccessConnector>("gpt-4o-mini"));

    FallbackManager fb(registry);
    fb.setChain("gpt-4o", {"gpt-4o-mini"});

    ChatRequest req;
    req.model = "gpt-4o";
    auto resp = fb.executeWithFallback(req, "gpt-4o");
    EXPECT_EQ(resp.model, "gpt-4o-mini");
    EXPECT_EQ(resp.content, "response from gpt-4o-mini");
}

TEST(FallbackTest, AllFail) {
    ConnectorRegistry registry;
    registry.registerConnector(std::make_unique<FailConnector>("gpt-4o"));
    registry.registerConnector(std::make_unique<FailConnector>("gpt-4o-mini"));

    FallbackManager fb(registry);
    fb.setChain("gpt-4o", {"gpt-4o-mini"});

    ChatRequest req;
    req.model = "gpt-4o";
    EXPECT_THROW(fb.executeWithFallback(req, "gpt-4o"), std::runtime_error);
}

TEST(FallbackTest, NoFallbackChain) {
    ConnectorRegistry registry;
    registry.registerConnector(std::make_unique<SuccessConnector>("gpt-4o"));

    FallbackManager fb(registry);

    ChatRequest req;
    req.model = "gpt-4o";
    auto resp = fb.executeWithFallback(req, "gpt-4o");
    EXPECT_EQ(resp.model, "gpt-4o");
}

TEST(FallbackTest, GetChain) {
    ConnectorRegistry registry;
    FallbackManager fb(registry);
    fb.setChain("gpt-4o", {"gpt-4o-mini", "local"});

    auto chain = fb.getChain("gpt-4o");
    ASSERT_EQ(chain.size(), 2u);
    EXPECT_EQ(chain[0], "gpt-4o-mini");
    EXPECT_EQ(chain[1], "local");

    EXPECT_TRUE(fb.getChain("unknown").empty());
}

// P1-A: when every candidate fails with a typed upstream status, the real
// status must be preserved (not collapsed into a generic 502 runtime_error).
TEST(FallbackTest, PreservesUpstreamStatusWhenAllFail) {
    ConnectorRegistry registry;
    registry.registerConnector(std::make_unique<StatusConnector>("gpt-4o", 503));
    registry.registerConnector(std::make_unique<StatusConnector>("gpt-4o-mini", 503));

    FallbackManager fb(registry);
    fb.setChain("gpt-4o", {"gpt-4o-mini"});

    ChatRequest req;
    req.model = "gpt-4o";
    try {
        fb.executeWithFallback(req, "gpt-4o");
        FAIL() << "expected throw";
    } catch (const UpstreamStatusError& e) {
        EXPECT_EQ(e.upstreamStatus(), 503);
    } catch (const std::exception& e) {
        FAIL() << "expected UpstreamStatusError, got: " << e.what();
    }
}

// P1-A: when the only candidate's circuit is open (nothing attempted), surface
// a typed CircuitBreakerOpenError so the gateway can return AEGIS-4002 (503).
TEST(FallbackTest, ThrowsCircuitOpenWhenAllCandidatesCircuitOpen) {
    ConnectorRegistry registry;
    registry.registerConnector(std::make_unique<FailConnector>("gpt-4o"));

    FallbackManager fb(registry, CircuitConfig{1, std::chrono::seconds{3600}});

    ChatRequest req;
    req.model = "gpt-4o";

    // First call attempts and fails → opens the circuit (threshold 1).
    EXPECT_THROW(fb.executeWithFallback(req, "gpt-4o"), std::runtime_error);
    // Second call: circuit open, nothing attempted → typed circuit-open error.
    EXPECT_THROW(fb.executeWithFallback(req, "gpt-4o"), CircuitBreakerOpenError);
}

// P1-D: a streaming run where every candidate's circuit is open (nothing
// attempted) must finalize with a typed 503 circuit-breaker error so the SSE
// terminal event is meaningful instead of an empty/generic default.
TEST(FallbackTest, StreamSurfacesCircuitOpenWhenAllCandidatesCircuitOpen) {
    ConnectorRegistry registry;
    registry.registerConnector(std::make_unique<StreamFailConnector>("gpt-4o"));

    FallbackManager fb(registry, CircuitConfig{1, std::chrono::seconds{3600}});

    ChatRequest req;
    req.model = "gpt-4o";

    // First stream attempt fails via onError → opens the circuit (threshold 1).
    bool first_err = false;
    fb.streamWithFallback(
        req, "gpt-4o", [](const StreamDelta&) {}, [](const TokenUsage&) {},
        [&](const GatewayError&) { first_err = true; });
    EXPECT_TRUE(first_err);

    // Second attempt: circuit open, nothing attempted → typed 503 circuit-open.
    GatewayError second{};
    bool second_err = false;
    fb.streamWithFallback(
        req, "gpt-4o", [](const StreamDelta&) {}, [](const TokenUsage&) {},
        [&](const GatewayError& e) { second = e; second_err = true; });
    ASSERT_TRUE(second_err);
    EXPECT_EQ(second.http_status, 503);
    EXPECT_EQ(second.error_code, toAegisCode(ErrorCode::CircuitBreakerOpen));
}

TEST(FallbackTest, CircuitBreakerSkipsOpenPrimaryAfterRepeatedFailures) {
    ConnectorRegistry registry;
    registry.registerConnector(std::make_unique<FailConnector>("gpt-4o"));
    registry.registerConnector(std::make_unique<SuccessConnector>("gpt-4o-mini"));

    FallbackManager fb(registry, CircuitConfig{3, std::chrono::seconds{3600}});
    fb.setChain("gpt-4o", {"gpt-4o-mini"});

    ChatRequest req;
    req.model = "gpt-4o";

    for (int i = 0; i < 3; ++i) {
        auto resp = fb.executeWithFallback(req, "gpt-4o");
        EXPECT_EQ(resp.model, "gpt-4o-mini");
    }

    auto resp = fb.executeWithFallback(req, "gpt-4o");
    EXPECT_EQ(resp.model, "gpt-4o-mini");
    EXPECT_EQ(resp.content, "response from gpt-4o-mini");
}
