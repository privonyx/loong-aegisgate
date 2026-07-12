#include <gtest/gtest.h>
#include <sstream>
#include <vector>
#include <aegisgate/error_codes.h>
#include "gateway/connector/openai.h"
#include "gateway/connector/upstream_client.h"

using namespace aegisgate;

class MockUpstream : public UpstreamClient {
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

class OpenAIConnectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        ProviderConfig config;
        config.name = "openai";
        config.type = "openai";
        config.base_url = "https://api.openai.com/v1";
        config.api_keys = {{"sk-test-key", 1}};
        config.models = {
            {"gpt-4o", "openai", 0.005, 0.015, 128000,
             {"high-quality"}, {{Capability::Streaming, Capability::Tools}}}
        };
        config.timeout_ms = 30000;
        connector_ = std::make_unique<OpenAIConnector>(config);
    }

    std::unique_ptr<OpenAIConnector> connector_;
};

TEST_F(OpenAIConnectorTest, Provider) {
    EXPECT_EQ(connector_->provider(), "openai");
}

TEST_F(OpenAIConnectorTest, SupportsModel) {
    EXPECT_TRUE(connector_->supportsModel("gpt-4o"));
    EXPECT_FALSE(connector_->supportsModel("claude-sonnet-4-20250514"));
}

TEST_F(OpenAIConnectorTest, BuildRequestBody_Basic) {
    ChatRequest req;
    req.model = "gpt-4o";
    req.messages = {{"user", "hello"}};

    auto body = connector_->buildRequestBody(req);

    EXPECT_EQ(body["model"], "gpt-4o");
    ASSERT_EQ(body["messages"].size(), 1u);
    EXPECT_EQ(body["messages"][0]["role"], "user");
    EXPECT_EQ(body["messages"][0]["content"], "hello");
    EXPECT_FALSE(body.contains("temperature"));
    EXPECT_FALSE(body.contains("max_tokens"));
    EXPECT_FALSE(body.contains("stream"));
}

TEST_F(OpenAIConnectorTest, BuildRequestBody_WithOptions) {
    ChatRequest req;
    req.model = "gpt-4o";
    req.messages = {{"system", "you are helpful"}, {"user", "hi"}};
    req.temperature = 0.7;
    req.max_tokens = 1000;
    req.stream = true;

    auto body = connector_->buildRequestBody(req);

    EXPECT_EQ(body["model"], "gpt-4o");
    EXPECT_EQ(body["messages"].size(), 2u);
    EXPECT_DOUBLE_EQ(body["temperature"].get<double>(), 0.7);
    EXPECT_EQ(body["max_tokens"], 1000);
    EXPECT_TRUE(body["stream"].get<bool>());
}

// P2-#4: vision/multimodal content (content as an array of parts) must survive
// to the upstream body. Previously the parser only read string content, so the
// image_url part was silently discarded at the gateway entry.
TEST_F(OpenAIConnectorTest, BuildRequestBody_PreservesMultimodalImage) {
    ChatRequest req;
    req.model = "gpt-4o";
    Message m;
    m.role = "user";
    m.content_parts = nlohmann::json::array({
        {{"type", "text"}, {"text", "what is in this image?"}},
        {{"type", "image_url"},
         {"image_url", {{"url", "data:image/png;base64,AAAA"}}}}});
    m.content = "what is in this image?";  // text extracted for guardrail scan
    req.messages.push_back(m);

    auto body = connector_->buildRequestBody(req);

    ASSERT_EQ(body["messages"].size(), 1u);
    const auto& content = body["messages"][0]["content"];
    ASSERT_TRUE(content.is_array());
    bool has_image = false;
    std::string image_url;
    for (const auto& part : content) {
        if (part.value("type", "") == "image_url") {
            has_image = true;
            image_url = part["image_url"].value("url", "");
        }
    }
    EXPECT_TRUE(has_image);
    EXPECT_EQ(image_url, "data:image/png;base64,AAAA");
}

// P2-#4: a guardrail that rewrites the extracted text (e.g. PII masking) must
// not be bypassed — the re-emitted multimodal array reflects the modified text
// while still carrying the image.
TEST_F(OpenAIConnectorTest, BuildRequestBody_MultimodalReflectsModifiedText) {
    ChatRequest req;
    req.model = "gpt-4o";
    Message m;
    m.role = "user";
    m.content_parts = nlohmann::json::array({
        {{"type", "text"}, {"text", "call me at 13812345678"}},
        {{"type", "image_url"},
         {"image_url", {{"url", "data:image/png;base64,BBBB"}}}}});
    m.content = "call me at [PHONE]";  // masked by PIIFilter downstream
    req.messages.push_back(m);

    auto body = connector_->buildRequestBody(req);
    const auto& content = body["messages"][0]["content"];
    ASSERT_TRUE(content.is_array());

    std::string text_seen;
    bool has_image = false;
    for (const auto& part : content) {
        if (part.value("type", "") == "text") text_seen = part.value("text", "");
        if (part.value("type", "") == "image_url") has_image = true;
    }
    EXPECT_EQ(text_seen, "call me at [PHONE]");
    EXPECT_EQ(text_seen.find("13812345678"), std::string::npos);
    EXPECT_TRUE(has_image);
}

// P2-#4: round-trip — array content parsed via from_json populates content_parts
// and extracts text into content for guardrail scanning.
TEST_F(OpenAIConnectorTest, FromJsonParsesMultimodalContent) {
    nlohmann::json mj = {
        {"role", "user"},
        {"content", nlohmann::json::array(
            {{{"type", "text"}, {"text", "describe"}},
             {{"type", "image_url"},
              {"image_url", {{"url", "http://img"}}}}})}};
    Message m = mj.get<Message>();
    EXPECT_EQ(m.role, "user");
    EXPECT_EQ(m.content, "describe");
    ASSERT_TRUE(m.content_parts.is_array());
    EXPECT_EQ(m.content_parts.size(), 2u);
}

TEST_F(OpenAIConnectorTest, ParseResponse_Normal) {
    nlohmann::json body = {
        {"id", "chatcmpl-123"},
        {"model", "gpt-4o"},
        {"choices", {{
            {"message", {{"content", "Hello! How can I help?"}}},
            {"finish_reason", "stop"}
        }}},
        {"usage", {
            {"prompt_tokens", 10},
            {"completion_tokens", 8},
            {"total_tokens", 18}
        }}
    };

    auto resp = connector_->parseResponse(body);
    EXPECT_EQ(resp.id, "chatcmpl-123");
    EXPECT_EQ(resp.model, "gpt-4o");
    EXPECT_EQ(resp.content, "Hello! How can I help?");
    EXPECT_EQ(resp.finish_reason, "stop");
    EXPECT_EQ(resp.usage.prompt_tokens, 10);
    EXPECT_EQ(resp.usage.completion_tokens, 8);
    EXPECT_EQ(resp.usage.total_tokens, 18);
}

TEST_F(OpenAIConnectorTest, ParseResponse_EmptyChoices) {
    nlohmann::json body = {
        {"id", "chatcmpl-123"},
        {"model", "gpt-4o"},
        {"choices", nlohmann::json::array()}
    };

    auto resp = connector_->parseResponse(body);
    EXPECT_EQ(resp.content, "");
}

TEST_F(OpenAIConnectorTest, ParseStreamChunk_ContentDelta) {
    std::string line = R"(data: {"choices":[{"delta":{"content":"Hello"}}]})";
    auto delta = connector_->parseStreamChunk(line);
    EXPECT_EQ(delta.content, "Hello");
}

TEST_F(OpenAIConnectorTest, ParseStreamChunk_RoleDelta) {
    std::string line = R"(data: {"choices":[{"delta":{"role":"assistant"}}]})";
    auto delta = connector_->parseStreamChunk(line);
    EXPECT_EQ(delta.content, "");
}

TEST_F(OpenAIConnectorTest, ParseStreamChunk_Done) {
    std::string line = "data: [DONE]";
    auto delta = connector_->parseStreamChunk(line);
    EXPECT_EQ(delta.content, "");
}

TEST_F(OpenAIConnectorTest, ParseStreamChunk_EmptyLine) {
    auto delta = connector_->parseStreamChunk("");
    EXPECT_EQ(delta.content, "");
}

TEST_F(OpenAIConnectorTest, ParseStreamChunk_ToolCallsDelta) {
    std::string line = R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_abc","type":"function","function":{"name":"get_weather","arguments":""}}]},"finish_reason":null}]})";
    auto delta = connector_->parseStreamChunk(line);
    EXPECT_TRUE(delta.content.empty());
    ASSERT_FALSE(delta.tool_calls_delta.is_null());
    EXPECT_EQ(delta.tool_calls_delta.size(), 1u);
    EXPECT_EQ(delta.tool_calls_delta[0]["id"], "call_abc");
}

TEST_F(OpenAIConnectorTest, ParseResponse_ToolCalls) {
    nlohmann::json body = {
        {"id", "chatcmpl-tool"},
        {"model", "gpt-4o"},
        {"choices", {{
            {"message", {
                {"role", "assistant"},
                {"content", nullptr},
                {"tool_calls", {{
                    {"id", "call_123"},
                    {"type", "function"},
                    {"function", {{"name", "get_weather"}, {"arguments", "{\"city\":\"London\"}"}}}
                }}}
            }},
            {"finish_reason", "tool_calls"}
        }}},
        {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 5}, {"total_tokens", 15}}}
    };
    auto resp = connector_->parseResponse(body);
    EXPECT_EQ(resp.content, "");
    EXPECT_EQ(resp.finish_reason, "tool_calls");
    ASSERT_EQ(resp.tool_calls.size(), 1u);
    EXPECT_EQ(resp.tool_calls[0].id, "call_123");
    EXPECT_EQ(resp.tool_calls[0].function["name"], "get_weather");
}

TEST_F(OpenAIConnectorTest, BuildRequestBody_WithTools) {
    ChatRequest req;
    req.model = "gpt-4o";
    req.messages = {{"user", "what's the weather?"}};
    req.tools = nlohmann::json::array({{
        {"type", "function"},
        {"function", {
            {"name", "get_weather"},
            {"description", "Get weather"},
            {"parameters", {{"type", "object"}, {"properties", {{"city", {{"type", "string"}}}}}}}
        }}
    }});
    req.tool_choice = "auto";

    auto body = connector_->buildRequestBody(req);
    ASSERT_TRUE(body.contains("tools"));
    EXPECT_EQ(body["tools"].size(), 1u);
    EXPECT_EQ(body["tool_choice"], "auto");
}

TEST_F(OpenAIConnectorTest, ParseUsage) {
    nlohmann::json body = {
        {"usage", {
            {"prompt_tokens", 50},
            {"completion_tokens", 100},
            {"total_tokens", 150}
        }}
    };
    auto usage = connector_->parseUsage(body);
    EXPECT_EQ(usage.prompt_tokens, 50);
    EXPECT_EQ(usage.completion_tokens, 100);
    EXPECT_EQ(usage.total_tokens, 150);
}

TEST_F(OpenAIConnectorTest, ParseUsage_Missing) {
    nlohmann::json body = {};
    auto usage = connector_->parseUsage(body);
    EXPECT_EQ(usage.total_tokens, 0);
}

TEST_F(OpenAIConnectorTest, Capabilities) {
    auto caps = connector_->capabilities("gpt-4o");
    EXPECT_TRUE(caps.has(Capability::Streaming));
    EXPECT_TRUE(caps.has(Capability::Tools));
    EXPECT_TRUE(caps.has(Capability::Vision));
}

TEST(OpenAIConnectorMockTest, CompleteViaMockUpstream) {
    ProviderConfig config;
    config.name = "openai";
    config.base_url = "https://api.openai.com";
    config.api_keys = {{"sk-test", 1}};
    config.models = {{"gpt-4o", "openai", 0.0, 0.0, 4096, {}, {}}};
    config.timeout_ms = 5000;

    auto mock = std::make_unique<MockUpstream>();
    mock->response_body = R"({
        "id":"chatcmpl-1","model":"gpt-4o",
        "choices":[{"message":{"content":"hello world"},"finish_reason":"stop"}],
        "usage":{"prompt_tokens":5,"completion_tokens":3,"total_tokens":8}
    })";
    auto* mock_ptr = mock.get();

    OpenAIConnector connector(config, std::move(mock));

    ChatRequest req;
    req.model = "gpt-4o";
    req.messages = {{"user", "hi"}};

    auto resp = connector.complete(req);

    EXPECT_EQ(resp.content, "hello world");
    EXPECT_EQ(resp.usage.total_tokens, 8);
    EXPECT_EQ(mock_ptr->last_req.path, "/chat/completions");
    EXPECT_EQ(mock_ptr->last_req.headers.at("Authorization"), "Bearer sk-test");
    EXPECT_EQ(mock_ptr->last_req.headers.at("Content-Type"), "application/json");
}

TEST(OpenAIConnectorMockTest, CompleteUpstreamError) {
    ProviderConfig config;
    config.name = "openai";
    config.base_url = "https://api.openai.com";
    config.api_keys = {{"sk-test", 1}};
    config.models = {{"gpt-4o", "openai", 0.0, 0.0, 4096, {}, {}}};

    auto mock = std::make_unique<MockUpstream>();
    mock->response_status = 429;
    mock->response_body = "rate limited";

    OpenAIConnector connector(config, std::move(mock));

    ChatRequest req;
    req.model = "gpt-4o";
    req.messages = {{"user", "hi"}};

    EXPECT_THROW(connector.complete(req), std::runtime_error);
}

// P0-3: a mock that replays a scripted sequence of (status, body) responses,
// one per send() call, so we can exercise same-provider key-level retry.
class SequencedMockUpstream : public UpstreamClient {
public:
    struct Resp { int status; std::string body; bool transport_error = false; };
    std::vector<Resp> script;
    size_t call_count = 0;

    void send(UpstreamRequest /*req*/, ResponseCallback onDone,
              ErrorCallback onError) override {
        const Resp& r = script[std::min(call_count, script.size() - 1)];
        ++call_count;
        if (r.transport_error) { onError(r.body); return; }
        onDone(r.status, r.body);
    }
    void sendStreaming(UpstreamRequest /*req*/, ChunkCallback /*onChunk*/,
                       ResponseCallback onDone, ErrorCallback /*onError*/) override {
        const Resp& r = script[std::min(call_count, script.size() - 1)];
        ++call_count;
        onDone(r.status, r.body);
    }
};

static ProviderConfig twoKeyConfig() {
    ProviderConfig config;
    config.name = "openai";
    config.base_url = "https://api.openai.com";
    config.api_keys = {{"sk-key-a", 1}, {"sk-key-b", 1}};
    config.models = {{"gpt-4o", "openai", 0.0, 0.0, 4096, {}, {}}};
    config.timeout_ms = 5000;
    return config;
}

static const char* kOkBody = R"({
    "id":"chatcmpl-1","model":"gpt-4o",
    "choices":[{"message":{"content":"ok"},"finish_reason":"stop"}],
    "usage":{"prompt_tokens":1,"completion_tokens":1,"total_tokens":2}
})";

// P0-3: a retryable upstream failure on the first key must transparently fail
// over to the next healthy key within the SAME request (previously a single
// nextKey() attempt surfaced "No healthy API keys" → 502).
TEST(OpenAIConnectorKeyRetryTest, RetriesNextKeyOnRetryableFailure) {
    auto mock = std::make_unique<SequencedMockUpstream>();
    mock->script = {{503, "overloaded"}, {200, kOkBody}};
    auto* mock_ptr = mock.get();

    OpenAIConnector connector(twoKeyConfig(), std::move(mock));
    ChatRequest req;
    req.model = "gpt-4o";
    req.messages = {{"user", "hi"}};

    auto resp = connector.complete(req);
    EXPECT_EQ(resp.content, "ok");
    EXPECT_EQ(mock_ptr->call_count, 2u);  // first key 503, second key 200
}

// P0-3 / P1-A: NoHealthyKeys is now reserved for the pure capacity case — no
// configured key, or every key failing at the transport layer (the upstream
// never produced an HTTP status). Transport errors on every key → AEGIS-4007.
TEST(OpenAIConnectorKeyRetryTest, ThrowsNoHealthyKeysWhenAllKeysTransportFail) {
    auto mock = std::make_unique<SequencedMockUpstream>();
    mock->script = {{0, "connection refused", /*transport_error=*/true}};
    OpenAIConnector connector(twoKeyConfig(), std::move(mock));
    ChatRequest req;
    req.model = "gpt-4o";
    req.messages = {{"user", "hi"}};

    EXPECT_THROW(connector.complete(req), NoHealthyKeysError);
}

// P1-A: when every key exhausts against an upstream HTTP status (e.g. 503), the
// real upstream status must be surfaced via UpstreamStatusError so the gateway
// can pass 503 through instead of collapsing to a generic 502 / 4007.
TEST(OpenAIConnectorKeyRetryTest, SurfacesUpstreamStatusWhenAllKeysReturnStatus) {
    auto mock = std::make_unique<SequencedMockUpstream>();
    mock->script = {{503, "overloaded"}};  // every call 503
    OpenAIConnector connector(twoKeyConfig(), std::move(mock));
    ChatRequest req;
    req.model = "gpt-4o";
    req.messages = {{"user", "hi"}};

    try {
        connector.complete(req);
        FAIL() << "expected throw";
    } catch (const UpstreamStatusError& e) {
        EXPECT_EQ(e.upstreamStatus(), 503);
    } catch (const std::exception& e) {
        FAIL() << "expected UpstreamStatusError, got: " << e.what();
    }
}

// P1-A: a non-retryable client error (400) is surfaced as UpstreamStatusError
// carrying the original status so the gateway passes 400 through verbatim.
TEST(OpenAIConnectorKeyRetryTest, SurfacesClientErrorStatus) {
    auto mock = std::make_unique<SequencedMockUpstream>();
    mock->script = {{400, "bad request"}};
    OpenAIConnector connector(twoKeyConfig(), std::move(mock));
    ChatRequest req;
    req.model = "gpt-4o";
    req.messages = {{"user", "hi"}};

    try {
        connector.complete(req);
        FAIL() << "expected throw";
    } catch (const UpstreamStatusError& e) {
        EXPECT_EQ(e.upstreamStatus(), 400);
    } catch (const std::exception& e) {
        FAIL() << "expected UpstreamStatusError, got: " << e.what();
    }
}

// P0-3: a non-retryable client error (e.g. 400 bad request) must NOT burn
// other keys — it is the request's fault, surface immediately.
TEST(OpenAIConnectorKeyRetryTest, DoesNotRetryOnClientError) {
    auto mock = std::make_unique<SequencedMockUpstream>();
    mock->script = {{400, "bad request"}};
    auto* mock_ptr = mock.get();
    OpenAIConnector connector(twoKeyConfig(), std::move(mock));
    ChatRequest req;
    req.model = "gpt-4o";
    req.messages = {{"user", "hi"}};

    try {
        connector.complete(req);
        FAIL() << "expected throw";
    } catch (const NoHealthyKeysError&) {
        FAIL() << "400 must not be treated as key exhaustion";
    } catch (const std::runtime_error&) {
        // expected
    }
    EXPECT_EQ(mock_ptr->call_count, 1u);  // only one attempt, no key burn
}

// P1-E: max_retries bounds same-request key attempts. With max_retries=1 only
// two attempts (initial + one retry) are made even when many healthy keys are
// configured, so an abusive/degraded upstream can't burn the whole pool.
TEST(OpenAIConnectorKeyRetryTest, StopsAfterMaxRetries) {
    ProviderConfig config;
    config.name = "openai";
    config.base_url = "https://api.openai.com";
    config.api_keys = {{"k1", 1}, {"k2", 1}, {"k3", 1}, {"k4", 1}, {"k5", 1}};
    config.models = {{"gpt-4o", "openai", 0.0, 0.0, 4096, {}, {}}};
    config.timeout_ms = 5000;
    config.max_retries = 1;

    auto mock = std::make_unique<SequencedMockUpstream>();
    mock->script = {{503, "overloaded"}};  // every key returns retryable 503
    auto* mock_ptr = mock.get();
    OpenAIConnector connector(config, std::move(mock));
    ChatRequest req;
    req.model = "gpt-4o";
    req.messages = {{"user", "hi"}};

    EXPECT_THROW(connector.complete(req), UpstreamStatusError);
    EXPECT_EQ(mock_ptr->call_count, 2u);  // initial + 1 retry, not all 5 keys
}

TEST(OpenAIConnectorMockTest, StreamCompleteViaMock) {
    ProviderConfig config;
    config.name = "openai";
    config.base_url = "https://api.openai.com";
    config.api_keys = {{"sk-test", 1}};
    config.models = {{"gpt-4o", "openai", 0.0, 0.0, 4096, {}, {}}};
    config.timeout_ms = 5000;

    auto mock = std::make_unique<MockUpstream>();
    mock->response_body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n"
        "data: {\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":2,\"total_tokens\":7},\"choices\":[]}\n"
        "data: [DONE]\n";
    auto* mock_ptr = mock.get();

    OpenAIConnector connector(config, std::move(mock));

    ChatRequest req;
    req.model = "gpt-4o";
    req.messages = {{"user", "hi"}};

    TokenUsage final_usage;
    bool done_called = false;

    connector.streamComplete(req,
        [](const StreamDelta&) {},
        [&](const TokenUsage& u) { final_usage = u; done_called = true; },
        [](const GatewayError&) { FAIL(); });

    EXPECT_TRUE(done_called);
    EXPECT_EQ(final_usage.total_tokens, 7);
    EXPECT_TRUE(mock_ptr->last_req.headers.count("Authorization"));
}
