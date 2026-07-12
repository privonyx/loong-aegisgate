#include <gtest/gtest.h>
#include "gateway/connector/upstream_client.h"

using namespace aegisgate;

class MockUpstreamClient : public UpstreamClient {
public:
    void send(UpstreamRequest req, ResponseCallback onDone,
              ErrorCallback /*onError*/) override {
        last_req = std::move(req);
        onDone(200, R"({"choices":[{"message":{"content":"hello"}}]})");
    }
    void sendStreaming(UpstreamRequest req, ChunkCallback onChunk,
                       ResponseCallback onDone,
                       ErrorCallback /*onError*/) override {
        last_req = std::move(req);
        onChunk("data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}");
        onDone(200, "");
    }
    UpstreamRequest last_req;
};

TEST(UpstreamClientTest, MockSendCallsOnDone) {
    MockUpstreamClient client;
    int status = 0;
    std::string body;
    UpstreamRequest req;
    req.method = "POST";
    req.path = "/chat/completions";
    req.body = "{}";
    client.send(std::move(req),
        [&](int s, std::string b) { status = s; body = std::move(b); },
        [](std::string) {});
    EXPECT_EQ(status, 200);
    EXPECT_FALSE(body.empty());
}

TEST(UpstreamClientTest, MockStreamingCallsOnChunk) {
    MockUpstreamClient client;
    std::string received;
    UpstreamRequest req;
    req.method = "POST";
    req.path = "/chat/completions";
    client.sendStreaming(std::move(req),
        [&](std::string_view chunk) { received += chunk; },
        [](int, std::string) {},
        [](std::string) {});
    EXPECT_FALSE(received.empty());
}

TEST(UpstreamClientTest, RequestFieldsPropagated) {
    MockUpstreamClient client;
    UpstreamRequest req;
    req.method = "POST";
    req.path = "/v1/chat";
    req.body = "{\"model\":\"test\"}";
    req.headers["Authorization"] = "Bearer key123";
    req.timeout_seconds = 15.0;
    client.send(std::move(req), [](int, std::string) {}, [](std::string) {});
    EXPECT_EQ(client.last_req.path, "/v1/chat");
    EXPECT_EQ(client.last_req.headers.at("Authorization"), "Bearer key123");
    EXPECT_DOUBLE_EQ(client.last_req.timeout_seconds, 15.0);
}
