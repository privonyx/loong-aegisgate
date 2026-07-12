#include <gtest/gtest.h>
#include "gateway/connector/claude.h"
#include "gateway/connector/upstream_client.h"

using namespace aegisgate;

class MockUpstreamClaude : public UpstreamClient {
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

class ClaudeConnectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        ProviderConfig cfg;
        cfg.name = "claude";
        cfg.base_url = "https://api.anthropic.com";
        cfg.api_keys = {{"test-key", 1}};
        cfg.models = {{
            "claude-3-opus-20240229",
            "claude",
            0.0,
            0.0,
            4096,
            std::vector<std::string>{},
            ModelCapabilities{},
        }};
        cfg.timeout_ms = 5000;
        connector_ = std::make_unique<ClaudeConnector>(cfg);
    }

    std::unique_ptr<ClaudeConnector> connector_;
};

TEST_F(ClaudeConnectorTest, BuildRequestBody_SeparatesSystemMessage) {
    ChatRequest req;
    req.model = "claude-3-opus-20240229";
    req.messages = {
        {"system", "You are helpful."},
        {"user", "Hello"},
    };

    auto body = connector_->buildRequestBody(req);

    ASSERT_EQ(body["messages"].size(), 1u);
    EXPECT_EQ(body["messages"][0]["role"], "user");
    EXPECT_EQ(body["messages"][0]["content"], "Hello");
    ASSERT_TRUE(body.contains("system"));
    EXPECT_EQ(body["system"], "You are helpful.");
}

TEST_F(ClaudeConnectorTest, BuildRequestBody_SetsMaxTokensDefault) {
    ChatRequest req;
    req.model = "claude-3-opus-20240229";
    req.messages = {{"user", "hi"}};
    req.max_tokens.reset();

    auto body = connector_->buildRequestBody(req);

    EXPECT_EQ(body["max_tokens"], 4096);
}

TEST_F(ClaudeConnectorTest, ParseResponse_ExtractsTextContent) {
    nlohmann::json body = {
        {"id", "msg_01"},
        {"model", "claude-3-opus-20240229"},
        {"content",
         nlohmann::json::array(
             {{{"type", "text"}, {"text", "First"}}, {{"type", "text"}, {"text", "Second"}}})},
        {"stop_reason", "end_turn"},
    };

    auto resp = connector_->parseResponse(body);

    EXPECT_EQ(resp.content, "FirstSecond");
    EXPECT_EQ(resp.finish_reason, "end_turn");
}

TEST_F(ClaudeConnectorTest, ParseResponse_ExtractsUsage) {
    nlohmann::json body = {
        {"id", "msg_02"},
        {"model", "claude-3-opus-20240229"},
        {"content", nlohmann::json::array({{{"type", "text"}, {"text", "ok"}}})},
        {"usage", {{"input_tokens", 12}, {"output_tokens", 34}}},
    };

    auto resp = connector_->parseResponse(body);

    EXPECT_EQ(resp.usage.prompt_tokens, 12);
    EXPECT_EQ(resp.usage.completion_tokens, 34);
    EXPECT_EQ(resp.usage.total_tokens, 46);
}

TEST_F(ClaudeConnectorTest, ParseStreamChunk_ContentBlockDelta) {
    std::string line =
        R"(data: {"type":"content_block_delta","delta":{"type":"text_delta","text":"Hello"}})";
    auto delta = connector_->parseStreamChunk(line);
    EXPECT_EQ(delta.content, "Hello");
}

TEST_F(ClaudeConnectorTest, ParseStreamChunk_IgnoresOtherEvents) {
    std::string line = R"(data: {"type":"message_start","message":{"id":"x"}})";
    auto delta = connector_->parseStreamChunk(line);
    EXPECT_EQ(delta.content, "");
}

TEST_F(ClaudeConnectorTest, ParseStreamChunk_ToolUseBlock) {
    std::string line =
        R"(data: {"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"toolu_123","name":"get_weather"}})";
    auto delta = connector_->parseStreamChunk(line);
    EXPECT_TRUE(delta.content.empty());
    ASSERT_FALSE(delta.tool_calls_delta.is_null());
    EXPECT_EQ(delta.tool_calls_delta[0]["id"], "toolu_123");
    EXPECT_EQ(delta.tool_calls_delta[0]["function"]["name"], "get_weather");
}

TEST_F(ClaudeConnectorTest, ParseResponse_ToolUse) {
    nlohmann::json body = {
        {"id", "msg_tool"},
        {"model", "claude-3-opus-20240229"},
        {"content", nlohmann::json::array({
            {{"type", "text"}, {"text", "Let me check"}},
            {{"type", "tool_use"}, {"id", "toolu_abc"}, {"name", "get_weather"},
             {"input", {{"city", "London"}}}}
        })},
        {"stop_reason", "tool_use"},
        {"usage", {{"input_tokens", 20}, {"output_tokens", 10}}}
    };
    auto resp = connector_->parseResponse(body);
    EXPECT_EQ(resp.content, "Let me check");
    EXPECT_EQ(resp.finish_reason, "tool_calls");
    ASSERT_EQ(resp.tool_calls.size(), 1u);
    EXPECT_EQ(resp.tool_calls[0].id, "toolu_abc");
    EXPECT_EQ(resp.tool_calls[0].function["name"], "get_weather");
}

TEST_F(ClaudeConnectorTest, BuildRequestBody_ToolConversion) {
    ChatRequest req;
    req.model = "claude-3-opus-20240229";
    req.messages = {{"user", "weather in Tokyo?"}};
    req.tools = nlohmann::json::array({{
        {"type", "function"},
        {"function", {
            {"name", "get_weather"},
            {"description", "Get current weather"},
            {"parameters", {{"type", "object"}, {"properties", {{"city", {{"type", "string"}}}}}}}
        }}
    }});
    req.tool_choice = "auto";

    auto body = connector_->buildRequestBody(req);
    ASSERT_TRUE(body.contains("tools"));
    EXPECT_EQ(body["tools"][0]["name"], "get_weather");
    EXPECT_TRUE(body["tools"][0].contains("input_schema"));
    EXPECT_EQ(body["tool_choice"]["type"], "auto");
}

TEST_F(ClaudeConnectorTest, ParseUsage_MapsClaudeFieldNames) {
    nlohmann::json body = {
        {"usage", {{"input_tokens", 100}, {"output_tokens", 200}}},
    };

    auto usage = connector_->parseUsage(body);

    EXPECT_EQ(usage.prompt_tokens, 100);
    EXPECT_EQ(usage.completion_tokens, 200);
    EXPECT_EQ(usage.total_tokens, 300);
}

TEST(ClaudeConnectorMockTest, CompleteViaMockUpstream) {
    ProviderConfig cfg;
    cfg.name = "claude";
    cfg.base_url = "https://api.anthropic.com";
    cfg.api_keys = {{"test-key", 1}};
    cfg.models = {
        {"claude-3-opus-20240229", "claude", 0.0, 0.0, 4096, {}, {}}};
    cfg.timeout_ms = 5000;

    auto mock = std::make_unique<MockUpstreamClaude>();
    mock->response_body = R"({
        "id":"msg_01","model":"claude-3-opus-20240229",
        "content":[{"type":"text","text":"hi there"}],
        "stop_reason":"end_turn",
        "usage":{"input_tokens":10,"output_tokens":5}
    })";
    auto* mock_ptr = mock.get();

    ClaudeConnector connector(cfg, std::move(mock));

    ChatRequest req;
    req.model = "claude-3-opus-20240229";
    req.messages = {{"user", "hello"}};

    auto resp = connector.complete(req);

    EXPECT_EQ(resp.content, "hi there");
    EXPECT_EQ(resp.usage.prompt_tokens, 10);
    EXPECT_EQ(resp.usage.completion_tokens, 5);
    EXPECT_EQ(mock_ptr->last_req.path, "/v1/messages");
    EXPECT_EQ(mock_ptr->last_req.headers.at("x-api-key"), "test-key");
    EXPECT_EQ(mock_ptr->last_req.headers.at("anthropic-version"), "2023-06-01");
}

TEST(ClaudeConnectorMockTest, CompleteUpstreamError) {
    ProviderConfig cfg;
    cfg.name = "claude";
    cfg.base_url = "https://api.anthropic.com";
    cfg.api_keys = {{"test-key", 1}};
    cfg.models = {
        {"claude-3-opus-20240229", "claude", 0.0, 0.0, 4096, {}, {}}};

    auto mock = std::make_unique<MockUpstreamClaude>();
    mock->response_status = 500;
    mock->response_body = "internal error";

    ClaudeConnector connector(cfg, std::move(mock));

    ChatRequest req;
    req.model = "claude-3-opus-20240229";
    req.messages = {{"user", "hello"}};

    EXPECT_THROW(connector.complete(req), std::runtime_error);
}
