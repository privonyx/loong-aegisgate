#include <gtest/gtest.h>
#include "observe/alert_channels.h"
#include <nlohmann/json.hpp>

using namespace aegisgate;

namespace {

Alert makeTestAlert() {
    Alert a;
    a.rule_id = "high_error_rate";
    a.description = "Error rate > 5%";
    a.severity = AlertSeverity::Critical;
    a.current_value = 7.5;
    a.threshold = 5.0;
    a.timestamp = "2026-03-25T10:00:00Z";
    return a;
}

} // namespace

TEST(AlertChannelsTest, WebhookFormatCorrect) {
    std::string captured_body;
    auto transport = [&](const std::string& /*url*/,
                         const std::string& body,
                         const std::string& content_type) {
        captured_body = body;
        EXPECT_EQ(content_type, "application/json");
    };

    auto channel = makeWebhookChannel("https://example.com/hook", "secret123", transport);
    channel(makeTestAlert());

    auto json = nlohmann::json::parse(captured_body);
    EXPECT_EQ(json["rule"], "high_error_rate");
    EXPECT_EQ(json["severity"], "critical");
    EXPECT_DOUBLE_EQ(json["current_value"].get<double>(), 7.5);
    EXPECT_DOUBLE_EQ(json["threshold"].get<double>(), 5.0);
    EXPECT_TRUE(json["signed"].get<bool>());
}

TEST(AlertChannelsTest, WebhookWithoutSecret) {
    std::string captured_body;
    auto transport = [&](const std::string&, const std::string& body,
                         const std::string&) { captured_body = body; };

    auto channel = makeWebhookChannel("https://example.com/hook", "", transport);
    channel(makeTestAlert());

    auto json = nlohmann::json::parse(captured_body);
    EXPECT_FALSE(json.contains("signed"));
}

TEST(AlertChannelsTest, DingTalkMarkdownFormat) {
    std::string captured_body;
    auto transport = [&](const std::string&, const std::string& body,
                         const std::string&) { captured_body = body; };

    auto channel = makeDingTalkChannel("https://oapi.dingtalk.com/robot/send", transport);
    channel(makeTestAlert());

    auto json = nlohmann::json::parse(captured_body);
    EXPECT_EQ(json["msgtype"], "markdown");
    EXPECT_TRUE(json["markdown"].contains("title"));
    EXPECT_TRUE(json["markdown"].contains("text"));
    EXPECT_NE(json["markdown"]["text"].get<std::string>().find("high_error_rate"),
              std::string::npos);
}

TEST(AlertChannelsTest, FeishuCardFormat) {
    std::string captured_body;
    auto transport = [&](const std::string&, const std::string& body,
                         const std::string&) { captured_body = body; };

    auto channel = makeFeishuChannel("https://open.feishu.cn/open-apis/bot/v2/hook/xxx", transport);
    channel(makeTestAlert());

    auto json = nlohmann::json::parse(captured_body);
    EXPECT_EQ(json["msg_type"], "interactive");
    EXPECT_EQ(json["card"]["header"]["template"], "red");
    EXPECT_FALSE(json["card"]["elements"].empty());
}

TEST(AlertChannelsTest, SlackBlockFormat) {
    std::string captured_body;
    auto transport = [&](const std::string&, const std::string& body,
                         const std::string&) { captured_body = body; };

    auto channel = makeSlackChannel("https://hooks.slack.com/services/xxx", transport);
    channel(makeTestAlert());

    auto json = nlohmann::json::parse(captured_body);
    EXPECT_TRUE(json.contains("blocks"));
    EXPECT_FALSE(json["blocks"].empty());
    EXPECT_EQ(json["blocks"][0]["type"], "section");
}
