#include <gtest/gtest.h>
#include "guardrail/inbound/openai_moderation.h"

using namespace aegisgate;

class OpenAIModerationTest : public ::testing::Test {
protected:
    OpenAIModerationConfig config_;

    void SetUp() override {
        config_.api_key = "test-key";
        config_.base_url = "https://api.openai.com";
        config_.model = "omni-moderation-latest";
        config_.timeout_ms = 5000;
    }
};

TEST_F(OpenAIModerationTest, IsConfiguredWithKey) {
    OpenAIModeration mod(config_);
    EXPECT_TRUE(mod.isConfigured());
}

TEST_F(OpenAIModerationTest, IsNotConfiguredWithoutKey) {
    config_.api_key = "";
    OpenAIModeration mod(config_);
    EXPECT_FALSE(mod.isConfigured());
}

TEST_F(OpenAIModerationTest, ProviderName) {
    OpenAIModeration mod(config_);
    EXPECT_EQ(mod.providerName(), "openai_moderation");
}

TEST_F(OpenAIModerationTest, BuildRequestBody) {
    OpenAIModeration mod(config_);
    auto body = mod.buildRequestBody("test text");
    EXPECT_EQ(body["input"], "test text");
    EXPECT_EQ(body["model"], "omni-moderation-latest");
}

TEST_F(OpenAIModerationTest, ParseCleanResponse) {
    OpenAIModeration mod(config_);
    std::string response = R"({
        "id": "modr-123",
        "model": "omni-moderation-latest",
        "results": [{
            "flagged": false,
            "categories": {
                "sexual": false,
                "hate": false,
                "harassment": false,
                "self-harm": false,
                "violence": false
            },
            "category_scores": {
                "sexual": 0.01,
                "hate": 0.02,
                "harassment": 0.01,
                "self-harm": 0.001,
                "violence": 0.005
            }
        }]
    })";
    auto result = mod.parseResponse(response);
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.flagged);
    EXPECT_EQ(result.categories.size(), 5);
}

TEST_F(OpenAIModerationTest, ParseFlaggedResponse) {
    OpenAIModeration mod(config_);
    std::string response = R"({
        "id": "modr-456",
        "model": "omni-moderation-latest",
        "results": [{
            "flagged": true,
            "categories": {
                "violence": true,
                "hate": false,
                "sexual": false
            },
            "category_scores": {
                "violence": 0.95,
                "hate": 0.10,
                "sexual": 0.02
            }
        }]
    })";
    auto result = mod.parseResponse(response);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.flagged);

    bool found_violence = false;
    for (const auto& cat : result.categories) {
        if (cat.name == "violence") {
            EXPECT_TRUE(cat.flagged);
            EXPECT_GT(cat.score, 0.9);
            found_violence = true;
        }
    }
    EXPECT_TRUE(found_violence);
}

TEST_F(OpenAIModerationTest, ParseInvalidJson) {
    OpenAIModeration mod(config_);
    auto result = mod.parseResponse("not json");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
}

TEST_F(OpenAIModerationTest, ParseMissingResults) {
    OpenAIModeration mod(config_);
    auto result = mod.parseResponse(R"({"id": "test"})");
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.find("missing results") != std::string::npos);
}

TEST_F(OpenAIModerationTest, CheckWithMockHttpSuccess) {
    HttpPostFn mock_fn = [](const std::string& /*url*/, const std::string& /*body*/,
                            const std::vector<std::pair<std::string, std::string>>& /*headers*/,
                            int /*timeout*/) -> std::pair<int, std::string> {
        return {200, R"({
            "results": [{
                "flagged": true,
                "categories": {"violence": true},
                "category_scores": {"violence": 0.99}
            }]
        })"};
    };

    OpenAIModeration mod(config_, mock_fn);
    auto result = mod.check("violent content");
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.flagged);
    EXPECT_EQ(result.provider, "openai_moderation");
}

TEST_F(OpenAIModerationTest, CheckWithMockHttpError) {
    HttpPostFn mock_fn = [](const std::string&, const std::string&,
                            const std::vector<std::pair<std::string, std::string>>&,
                            int) -> std::pair<int, std::string> {
        return {500, "Internal Server Error"};
    };

    OpenAIModeration mod(config_, mock_fn);
    auto result = mod.check("test");
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.find("500") != std::string::npos);
}

TEST_F(OpenAIModerationTest, CheckWithoutApiKey) {
    config_.api_key = "";
    OpenAIModeration mod(config_);
    auto result = mod.check("test");
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.find("not configured") != std::string::npos);
}

TEST_F(OpenAIModerationTest, ParseEmptyCategories) {
    OpenAIModeration mod(config_);
    std::string response = R"({
        "results": [{
            "flagged": false,
            "categories": {},
            "category_scores": {}
        }]
    })";
    auto result = mod.parseResponse(response);
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.flagged);
    EXPECT_EQ(result.categories.size(), 0);
}

TEST_F(OpenAIModerationTest, CheckAuthHeaderSent) {
    std::string captured_auth;
    HttpPostFn mock_fn = [&captured_auth](
        const std::string&, const std::string&,
        const std::vector<std::pair<std::string, std::string>>& headers,
        int) -> std::pair<int, std::string> {
        for (const auto& [k, v] : headers) {
            if (k == "Authorization") captured_auth = v;
        }
        return {200, R"({"results": [{"flagged": false, "categories": {}, "category_scores": {}}]})"};
    };

    OpenAIModeration mod(config_, mock_fn);
    mod.check("test");
    EXPECT_EQ(captured_auth, "Bearer test-key");
}
