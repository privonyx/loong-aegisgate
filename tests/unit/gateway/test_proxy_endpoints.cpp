#include <gtest/gtest.h>
#include "gateway/connector/openai.h"
#include "gateway/connector/claude.h"
#include "gateway/connector/upstream_client.h"

using namespace aegisgate;

class ProxyMockUpstream : public UpstreamClient {
public:
    int response_status = 200;
    std::string response_body;
    UpstreamRequest last_req;

    void send(UpstreamRequest req, ResponseCallback onDone,
              ErrorCallback /*onError*/) override {
        last_req = std::move(req);
        onDone(response_status, response_body);
    }
    void sendStreaming(UpstreamRequest req, ChunkCallback /*onChunk*/,
                       ResponseCallback onDone,
                       ErrorCallback /*onError*/) override {
        last_req = std::move(req);
        onDone(response_status, response_body);
    }
};

static ProviderConfig makeOpenAIConfig() {
    ProviderConfig config;
    config.name = "openai";
    config.type = "openai";
    config.base_url = "https://api.openai.com";
    config.api_keys = {{"sk-test-key", 1}};
    config.models = {
        {"gpt-4o", "openai", 0.005, 0.015, 128000, {}, {}},
        {"text-embedding-3-small", "openai", 0.0001, 0.0, 8191, {}, {}},
    };
    config.timeout_ms = 30000;
    return config;
}

static ProviderConfig makeClaudeConfig() {
    ProviderConfig config;
    config.name = "anthropic";
    config.type = "claude";
    config.base_url = "https://api.anthropic.com";
    config.api_keys = {{"sk-ant-test", 1}};
    config.models = {
        {"claude-sonnet-4-20250514", "anthropic", 0.003, 0.015, 200000, {}, {}},
    };
    config.timeout_ms = 30000;
    return config;
}

// === OpenAI: supportsEndpoint ===

TEST(OpenAIProxyTest, SupportsEmbeddingsEndpoint) {
    OpenAIConnector connector(makeOpenAIConfig());
    EXPECT_TRUE(connector.supportsEndpoint("/v1/embeddings"));
}

TEST(OpenAIProxyTest, SupportsImageGenerationsEndpoint) {
    OpenAIConnector connector(makeOpenAIConfig());
    EXPECT_TRUE(connector.supportsEndpoint("/v1/images/generations"));
}

TEST(OpenAIProxyTest, SupportsAudioTranscriptions) {
    OpenAIConnector connector(makeOpenAIConfig());
    EXPECT_TRUE(connector.supportsEndpoint("/v1/audio/transcriptions"));
}

TEST(OpenAIProxyTest, SupportsAudioTranslations) {
    OpenAIConnector connector(makeOpenAIConfig());
    EXPECT_TRUE(connector.supportsEndpoint("/v1/audio/translations"));
}

TEST(OpenAIProxyTest, SupportsAudioSpeech) {
    OpenAIConnector connector(makeOpenAIConfig());
    EXPECT_TRUE(connector.supportsEndpoint("/v1/audio/speech"));
}

TEST(OpenAIProxyTest, DoesNotSupportUnknownEndpoint) {
    OpenAIConnector connector(makeOpenAIConfig());
    EXPECT_FALSE(connector.supportsEndpoint("/v1/fine_tuning/jobs"));
    EXPECT_FALSE(connector.supportsEndpoint("/v1/chat/completions"));
    EXPECT_FALSE(connector.supportsEndpoint("/v1/unknown"));
}

// === Claude: supportsEndpoint ===

TEST(ClaudeProxyTest, DoesNotSupportAnyProxyEndpoint) {
    ClaudeConnector connector(makeClaudeConfig());
    EXPECT_FALSE(connector.supportsEndpoint("/v1/embeddings"));
    EXPECT_FALSE(connector.supportsEndpoint("/v1/images/generations"));
    EXPECT_FALSE(connector.supportsEndpoint("/v1/audio/speech"));
}

// === OpenAI: proxyRequest with mock upstream ===

TEST(OpenAIProxyTest, ProxyEmbeddingsRequest) {
    auto mock = std::make_unique<ProxyMockUpstream>();
    mock->response_body = R"({
        "object": "list",
        "data": [{"object": "embedding", "embedding": [0.1, 0.2, 0.3], "index": 0}],
        "model": "text-embedding-3-small",
        "usage": {"prompt_tokens": 5, "total_tokens": 5}
    })";
    auto* mock_ptr = mock.get();

    OpenAIConnector connector(makeOpenAIConfig(), std::move(mock));

    ProxyRequest req;
    req.endpoint = "/v1/embeddings";
    req.model = "text-embedding-3-small";
    req.raw_body = R"({"model":"text-embedding-3-small","input":"Hello world"})";
    req.content_type = "application/json";

    auto resp = connector.proxyRequest(req);

    EXPECT_EQ(resp.http_status, 200);
    EXPECT_EQ(resp.content_type, "application/json");
    EXPECT_FALSE(resp.body.empty());

    auto body = nlohmann::json::parse(resp.body);
    EXPECT_EQ(body["object"], "list");
    EXPECT_EQ(body["data"].size(), 1u);

    EXPECT_EQ(mock_ptr->last_req.path, "/v1/embeddings");
    EXPECT_EQ(mock_ptr->last_req.headers.at("Authorization"), "Bearer sk-test-key");
    EXPECT_EQ(mock_ptr->last_req.headers.at("Content-Type"), "application/json");
}

TEST(OpenAIProxyTest, ProxyImageGenerationRequest) {
    auto mock = std::make_unique<ProxyMockUpstream>();
    mock->response_body = R"({
        "created": 1234567890,
        "data": [{"url": "https://example.com/image.png"}]
    })";
    auto* mock_ptr = mock.get();

    OpenAIConnector connector(makeOpenAIConfig(), std::move(mock));

    ProxyRequest req;
    req.endpoint = "/v1/images/generations";
    req.raw_body = R"({"model":"dall-e-3","prompt":"a cat","n":1,"size":"1024x1024"})";
    req.content_type = "application/json";

    auto resp = connector.proxyRequest(req);

    EXPECT_EQ(resp.http_status, 200);
    EXPECT_EQ(resp.content_type, "application/json");
    EXPECT_EQ(mock_ptr->last_req.path, "/v1/images/generations");
}

TEST(OpenAIProxyTest, ProxyAudioSpeechReturnsAudioContentType) {
    auto mock = std::make_unique<ProxyMockUpstream>();
    mock->response_body = "fake-audio-binary-data";

    OpenAIConnector connector(makeOpenAIConfig(), std::move(mock));

    ProxyRequest req;
    req.endpoint = "/v1/audio/speech";
    req.raw_body = R"({"model":"tts-1","input":"Hello","voice":"alloy"})";
    req.content_type = "application/json";

    auto resp = connector.proxyRequest(req);

    EXPECT_EQ(resp.http_status, 200);
    EXPECT_EQ(resp.content_type, "audio/mpeg");
    EXPECT_EQ(resp.body, "fake-audio-binary-data");
}

TEST(OpenAIProxyTest, ProxyAudioTranscriptionMultipart) {
    auto mock = std::make_unique<ProxyMockUpstream>();
    mock->response_body = R"({"text":"This is a test."})";
    auto* mock_ptr = mock.get();

    OpenAIConnector connector(makeOpenAIConfig(), std::move(mock));

    ProxyRequest req;
    req.endpoint = "/v1/audio/transcriptions";
    req.raw_body = "fake-multipart-data";
    req.content_type = "multipart/form-data; boundary=----WebKitFormBoundary";

    auto resp = connector.proxyRequest(req);

    EXPECT_EQ(resp.http_status, 200);
    auto body = nlohmann::json::parse(resp.body);
    EXPECT_EQ(body["text"], "This is a test.");
    EXPECT_EQ(mock_ptr->last_req.headers.at("Content-Type"),
              "multipart/form-data; boundary=----WebKitFormBoundary");
}

TEST(OpenAIProxyTest, ProxyRequestUpstreamError) {
    auto mock = std::make_unique<ProxyMockUpstream>();
    mock->response_status = 429;
    mock->response_body = R"({"error":{"message":"Rate limit exceeded"}})";

    OpenAIConnector connector(makeOpenAIConfig(), std::move(mock));

    ProxyRequest req;
    req.endpoint = "/v1/embeddings";
    req.raw_body = R"({"model":"text-embedding-3-small","input":"test"})";

    auto resp = connector.proxyRequest(req);

    EXPECT_EQ(resp.http_status, 429);
    EXPECT_FALSE(resp.body.empty());
}

TEST(OpenAIProxyTest, ProxyRequestNoApiKeys) {
    ProviderConfig config = makeOpenAIConfig();
    config.api_keys.clear();

    auto mock = std::make_unique<ProxyMockUpstream>();
    OpenAIConnector connector(config, std::move(mock));

    ProxyRequest req;
    req.endpoint = "/v1/embeddings";
    req.raw_body = R"({"model":"text-embedding-3-small","input":"test"})";

    EXPECT_THROW(connector.proxyRequest(req), std::runtime_error);
}

// === Base ModelConnector defaults ===

TEST(ClaudeProxyTest, ProxyRequestReturns404) {
    ClaudeConnector connector(makeClaudeConfig());

    ProxyRequest req;
    req.endpoint = "/v1/embeddings";
    req.raw_body = R"({"model":"text-embedding-3-small","input":"test"})";

    auto resp = connector.proxyRequest(req);

    EXPECT_EQ(resp.http_status, 404);
    EXPECT_NE(resp.body.find("not supported"), std::string::npos);
}

// === ProxyRequest / ProxyResponse types ===

TEST(ProxyTypesTest, DefaultValues) {
    ProxyRequest req;
    EXPECT_TRUE(req.endpoint.empty());
    EXPECT_TRUE(req.model.empty());
    EXPECT_TRUE(req.raw_body.empty());
    EXPECT_EQ(req.content_type, "application/json");

    ProxyResponse resp;
    EXPECT_EQ(resp.http_status, 200);
    EXPECT_TRUE(resp.body.empty());
    EXPECT_EQ(resp.content_type, "application/json");
}
