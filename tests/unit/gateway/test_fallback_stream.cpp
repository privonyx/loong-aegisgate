#include <gtest/gtest.h>
#include "gateway/fallback.h"
#include "gateway/connector/base.h"
#include <memory>
#include <string>

using namespace aegisgate;

class StreamSuccessConnector : public ModelConnector {
public:
    explicit StreamSuccessConnector(std::string model) : model_(std::move(model)) {}
    ChatResponse complete(const ChatRequest& req) override {
        ChatResponse r;
        r.model = req.model;
        r.content = "full response";
        return r;
    }
    void streamComplete(const ChatRequest& req,
                        std::function<void(const StreamDelta&)> onDelta,
                        std::function<void(const TokenUsage&)> onDone,
                        std::function<void(const GatewayError&)> onError) override {
        (void)req;
        (void)onError;
        onDelta(StreamDelta{"hello ", {}, ""});
        onDelta(StreamDelta{"world", {}, ""});
        onDone({10, 5, 15});
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

class StreamFailConnector : public ModelConnector {
public:
    explicit StreamFailConnector(std::string model) : model_(std::move(model)) {}
    ChatResponse complete(const ChatRequest&) override {
        throw std::runtime_error("unavailable");
    }
    void streamComplete(const ChatRequest&,
                        std::function<void(const StreamDelta&)>,
                        std::function<void(const TokenUsage&)>,
                        std::function<void(const GatewayError&)> onError) override {
        onError({502, "upstream_error", "model unavailable", "", ""});
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

TEST(FallbackStreamTest, PrimaryStreamSuccess) {
    ConnectorRegistry registry;
    registry.registerConnector(std::make_unique<StreamSuccessConnector>("gpt-4o"));

    FallbackManager fb(registry);
    std::string accumulated;
    TokenUsage final_usage;
    bool done = false;
    bool errored = false;

    fb.streamWithFallback(
        ChatRequest{}, "gpt-4o",
        [&](const StreamDelta& d) { accumulated += d.content; },
        [&](const TokenUsage& u) { final_usage = u; done = true; },
        [&](const GatewayError&) { errored = true; });

    EXPECT_EQ(accumulated, "hello world");
    EXPECT_TRUE(done);
    EXPECT_FALSE(errored);
    EXPECT_EQ(final_usage.total_tokens, 15);
}

TEST(FallbackStreamTest, FallbackOnStreamFailure) {
    ConnectorRegistry registry;
    registry.registerConnector(std::make_unique<StreamFailConnector>("gpt-4o"));
    registry.registerConnector(std::make_unique<StreamSuccessConnector>("gpt-4o-mini"));

    FallbackManager fb(registry);
    fb.setChain("gpt-4o", {"gpt-4o-mini"});

    std::string accumulated;
    bool done = false;
    bool errored = false;

    fb.streamWithFallback(
        ChatRequest{}, "gpt-4o",
        [&](const StreamDelta& d) { accumulated += d.content; },
        [&](const TokenUsage&) { done = true; },
        [&](const GatewayError&) { errored = true; });

    EXPECT_EQ(accumulated, "hello world");
    EXPECT_TRUE(done);
    EXPECT_FALSE(errored);
}

class StreamPartialThenFailConnector : public ModelConnector {
public:
    explicit StreamPartialThenFailConnector(std::string model) : model_(std::move(model)) {}
    ChatResponse complete(const ChatRequest&) override { return {}; }
    void streamComplete(const ChatRequest&,
                        std::function<void(const StreamDelta&)> onDelta,
                        std::function<void(const TokenUsage&)>,
                        std::function<void(const GatewayError&)> onError) override {
        onDelta(StreamDelta{"partial", {}, ""});
        onError({500, "mid_stream_error", "error after chunks sent", "", ""});
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

TEST(FallbackStreamTest, NoFallbackAfterChunksSent) {
    ConnectorRegistry registry;
    registry.registerConnector(std::make_unique<StreamPartialThenFailConnector>("gpt-4o"));
    registry.registerConnector(std::make_unique<StreamSuccessConnector>("gpt-4o-mini"));

    FallbackManager fb(registry);
    fb.setChain("gpt-4o", {"gpt-4o-mini"});

    std::string accumulated;
    bool errored = false;
    bool done = false;

    fb.streamWithFallback(
        ChatRequest{}, "gpt-4o",
        [&](const StreamDelta& d) { accumulated += d.content; },
        [&](const TokenUsage&) { done = true; },
        [&](const GatewayError& e) {
            (void)e;
            errored = true;
        });

    EXPECT_EQ(accumulated, "partial");
    EXPECT_TRUE(errored);
    EXPECT_FALSE(done);
}

TEST(FallbackStreamTest, AllStreamFail) {
    ConnectorRegistry registry;
    registry.registerConnector(std::make_unique<StreamFailConnector>("gpt-4o"));
    registry.registerConnector(std::make_unique<StreamFailConnector>("gpt-4o-mini"));

    FallbackManager fb(registry);
    fb.setChain("gpt-4o", {"gpt-4o-mini"});

    bool errored = false;
    GatewayError last_err;

    fb.streamWithFallback(
        ChatRequest{}, "gpt-4o",
        [](const StreamDelta&) {},
        [](const TokenUsage&) { FAIL() << "Should not call onDone"; },
        [&](const GatewayError& e) { errored = true; last_err = e; });

    EXPECT_TRUE(errored);
    EXPECT_EQ(last_err.http_status, 502);
}
