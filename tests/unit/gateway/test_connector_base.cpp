#include <gtest/gtest.h>
#include "gateway/connector/base.h"

using namespace aegisgate;

class MockConnector : public ModelConnector {
public:
    ChatResponse complete(const ChatRequest& req) override {
        ChatResponse resp;
        resp.id = "mock-id";
        resp.model = req.model;
        resp.content = "mock response";
        resp.finish_reason = "stop";
        return resp;
    }

    void streamComplete(
        const ChatRequest& req,
        std::function<void(const StreamDelta&)> onDelta,
        std::function<void(const TokenUsage&)> onDone,
        std::function<void(const GatewayError&)> onError) override {
        (void)req;
        (void)onError;
        onDelta(StreamDelta{"hello ", {}, ""});
        onDelta(StreamDelta{"world", {}, ""});
        TokenUsage usage{5, 2, 7};
        onDone(usage);
    }

    std::string provider() const override { return "mock"; }
    bool supportsModel(const std::string& model) const override {
        return model == "mock-model";
    }
    ModelCapabilities capabilities(const std::string& /*model*/) const override {
        return {{Capability::Streaming, Capability::Temperature}};
    }

protected:
    nlohmann::json buildRequestBody(const ChatRequest& req) override {
        return {{"model", req.model}};
    }
    ChatResponse parseResponse(const nlohmann::json& body) override {
        ChatResponse resp;
        resp.model = body.value("model", "");
        return resp;
    }
    StreamDelta parseStreamChunk(std::string_view /*chunk*/) override {
        return {};
    }
    TokenUsage parseUsage(const nlohmann::json& /*body*/) override {
        return {};
    }
};

TEST(ModelConnectorTest, MockComplete) {
    MockConnector conn;
    ChatRequest req;
    req.model = "mock-model";
    req.messages = {{"user", "hello"}};
    auto resp = conn.complete(req);
    EXPECT_EQ(resp.id, "mock-id");
    EXPECT_EQ(resp.model, "mock-model");
    EXPECT_EQ(resp.content, "mock response");
}

TEST(ModelConnectorTest, MockStreamComplete) {
    MockConnector conn;
    ChatRequest req;
    req.model = "mock-model";
    req.stream = true;

    std::string accumulated;
    TokenUsage final_usage;

    conn.streamComplete(
        req,
        [&](const StreamDelta& delta) { accumulated += delta.content; },
        [&](const TokenUsage& u) { final_usage = u; },
        [](const GatewayError&) { FAIL() << "should not error"; });

    EXPECT_EQ(accumulated, "hello world");
    EXPECT_EQ(final_usage.total_tokens, 7);
}

TEST(ModelConnectorTest, SupportsModel) {
    MockConnector conn;
    EXPECT_TRUE(conn.supportsModel("mock-model"));
    EXPECT_FALSE(conn.supportsModel("other-model"));
}

TEST(ModelConnectorTest, Capabilities) {
    MockConnector conn;
    auto caps = conn.capabilities("mock-model");
    EXPECT_TRUE(caps.has(Capability::Streaming));
    EXPECT_TRUE(caps.has(Capability::Temperature));
    EXPECT_FALSE(caps.has(Capability::Tools));
    EXPECT_FALSE(caps.has(Capability::Vision));
}

TEST(ModelCapabilitiesTest, EmptyCapabilities) {
    ModelCapabilities caps;
    EXPECT_FALSE(caps.has(Capability::Streaming));
    EXPECT_FALSE(caps.has(Capability::Tools));
}

TEST(ModelInfoTest, Structure) {
    ModelInfo info;
    info.id = "gpt-4o";
    info.provider = "openai";
    info.cost_per_1k_input = 0.005;
    info.tags = {"high-quality", "expensive"};
    info.capabilities = {{Capability::Streaming, Capability::Tools, Capability::Vision}};

    EXPECT_EQ(info.id, "gpt-4o");
    EXPECT_TRUE(info.capabilities.has(Capability::Vision));
    EXPECT_EQ(info.tags.size(), 2u);
}
