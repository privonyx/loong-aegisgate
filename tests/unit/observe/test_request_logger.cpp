#include <gtest/gtest.h>
#include "observe/request_logger.h"

using namespace aegisgate;

class RequestLoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        logger_.clear();
    }
    RequestLogger logger_;
};

TEST_F(RequestLoggerTest, FormatsEntryAsJSON) {
    RequestLogEntry entry;
    entry.request_id = "req-001";
    entry.tenant_id = "t1";
    entry.model = "gpt-4";
    entry.prompt_tokens = 100;
    entry.completion_tokens = 50;
    entry.total_tokens = 150;
    entry.duration_ms = 345.6;
    entry.status = "ok";

    auto j = logger_.formatEntry(entry);
    EXPECT_EQ(j["request_id"], "req-001");
    EXPECT_EQ(j["model"], "gpt-4");
    EXPECT_EQ(j["tokens"]["prompt"], 100);
    EXPECT_EQ(j["tokens"]["completion"], 50);
    EXPECT_EQ(j["tokens"]["total"], 150);
    EXPECT_DOUBLE_EQ(j["duration_ms"], 345.6);
    EXPECT_EQ(j["status"], "ok");
    EXPECT_TRUE(j.contains("timestamp"));
}

TEST_F(RequestLoggerTest, LogsRequest) {
    RequestLogEntry entry;
    entry.request_id = "req-002";
    entry.model = "claude-3";
    entry.status = "ok";

    logger_.logRequest(entry);
    ASSERT_EQ(logger_.logs().size(), 1u);
    EXPECT_EQ(logger_.logs()[0]["request_id"], "req-002");
}

TEST_F(RequestLoggerTest, SinkReceivesLogs) {
    std::vector<nlohmann::json> received;
    logger_.setSink([&received](const nlohmann::json& j) {
        received.push_back(j);
    });

    RequestLogEntry entry;
    entry.request_id = "req-003";
    entry.model = "gpt-3.5";
    entry.status = "ok";
    logger_.logRequest(entry);

    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0]["request_id"], "req-003");
}

TEST_F(RequestLoggerTest, MasksAPIKey) {
    EXPECT_EQ(RequestLogger::maskApiKey("sk-abcdefghijklmnop"), "sk-a...mnop");
    EXPECT_EQ(RequestLogger::maskApiKey("short"), "****");
    EXPECT_EQ(RequestLogger::maskApiKey("12345678"), "1234...5678");
}

TEST_F(RequestLoggerTest, ClearRemovesLogs) {
    RequestLogEntry entry;
    entry.request_id = "req-004";
    entry.status = "ok";
    logger_.logRequest(entry);
    EXPECT_EQ(logger_.logs().size(), 1u);
    logger_.clear();
    EXPECT_EQ(logger_.logs().size(), 0u);
}

TEST_F(RequestLoggerTest, PipelineLogsContext) {
    RequestContext ctx;
    ctx.request_id = "req-005";
    ctx.tenant_id = "tenant-a";
    ctx.chat_request.model = "gpt-4";
    ctx.token_usage = {100, 50, 150};
    ctx.start_time = std::chrono::steady_clock::now();

    EXPECT_EQ(logger_.process(ctx), StageResult::Continue);
    ASSERT_EQ(logger_.logs().size(), 1u);
    EXPECT_EQ(logger_.logs()[0]["request_id"], "req-005");
    EXPECT_EQ(logger_.logs()[0]["tokens"]["total"], 150);
}

TEST_F(RequestLoggerTest, ErrorEntryIncludesDetail) {
    RequestLogEntry entry;
    entry.request_id = "req-006";
    entry.status = "error";
    entry.error_detail = "Connection timeout";

    auto j = logger_.formatEntry(entry);
    EXPECT_EQ(j["status"], "error");
    EXPECT_EQ(j["error"], "Connection timeout");
}

TEST_F(RequestLoggerTest, SuccessEntryOmitsError) {
    RequestLogEntry entry;
    entry.request_id = "req-007";
    entry.status = "ok";

    auto j = logger_.formatEntry(entry);
    EXPECT_FALSE(j.contains("error"));
}
