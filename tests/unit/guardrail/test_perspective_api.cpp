#include <gtest/gtest.h>
#include "guardrail/inbound/perspective_api.h"

using namespace aegisgate;

class PerspectiveApiTest : public ::testing::Test {
protected:
    PerspectiveConfig config_;

    void SetUp() override {
        config_.api_key = "test-key";
        config_.base_url = "https://commentanalyzer.googleapis.com";
        config_.threshold = 0.7;
        config_.attributes = {"TOXICITY", "SEVERE_TOXICITY", "THREAT"};
        config_.timeout_ms = 5000;
    }
};

TEST_F(PerspectiveApiTest, IsConfiguredWithKey) {
    PerspectiveApi api(config_);
    EXPECT_TRUE(api.isConfigured());
}

TEST_F(PerspectiveApiTest, IsNotConfiguredWithoutKey) {
    config_.api_key = "";
    PerspectiveApi api(config_);
    EXPECT_FALSE(api.isConfigured());
}

TEST_F(PerspectiveApiTest, ProviderName) {
    PerspectiveApi api(config_);
    EXPECT_EQ(api.providerName(), "google_perspective");
}

TEST_F(PerspectiveApiTest, BuildRequestBody) {
    PerspectiveApi api(config_);
    auto body = api.buildRequestBody("test text");
    EXPECT_EQ(body["comment"]["text"], "test text");
    EXPECT_TRUE(body.contains("requestedAttributes"));
    EXPECT_TRUE(body["requestedAttributes"].contains("TOXICITY"));
    EXPECT_TRUE(body["requestedAttributes"].contains("SEVERE_TOXICITY"));
    EXPECT_TRUE(body["requestedAttributes"].contains("THREAT"));
    EXPECT_TRUE(body.contains("languages"));
}

TEST_F(PerspectiveApiTest, ParseCleanResponse) {
    PerspectiveApi api(config_);
    std::string response = R"({
        "attributeScores": {
            "TOXICITY": {
                "summaryScore": {"value": 0.1, "type": "PROBABILITY"}
            },
            "SEVERE_TOXICITY": {
                "summaryScore": {"value": 0.05, "type": "PROBABILITY"}
            },
            "THREAT": {
                "summaryScore": {"value": 0.02, "type": "PROBABILITY"}
            }
        }
    })";
    auto result = api.parseResponse(response);
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.flagged);
    EXPECT_EQ(result.categories.size(), 3);
}

TEST_F(PerspectiveApiTest, ParseToxicResponse) {
    PerspectiveApi api(config_);
    std::string response = R"({
        "attributeScores": {
            "TOXICITY": {
                "summaryScore": {"value": 0.95, "type": "PROBABILITY"}
            },
            "SEVERE_TOXICITY": {
                "summaryScore": {"value": 0.85, "type": "PROBABILITY"}
            },
            "THREAT": {
                "summaryScore": {"value": 0.1, "type": "PROBABILITY"}
            }
        }
    })";
    auto result = api.parseResponse(response);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.flagged);

    int flagged_count = 0;
    for (const auto& cat : result.categories) {
        if (cat.flagged) flagged_count++;
        if (cat.name == "TOXICITY") {
            EXPECT_TRUE(cat.flagged);
            EXPECT_GT(cat.score, 0.9);
        }
        if (cat.name == "THREAT") {
            EXPECT_FALSE(cat.flagged);
        }
    }
    EXPECT_EQ(flagged_count, 2);
}

TEST_F(PerspectiveApiTest, ParseInvalidJson) {
    PerspectiveApi api(config_);
    auto result = api.parseResponse("not json");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
}

TEST_F(PerspectiveApiTest, ParseMissingAttributeScores) {
    PerspectiveApi api(config_);
    auto result = api.parseResponse(R"({"languages": ["en"]})");
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.find("missing attributeScores") != std::string::npos);
}

TEST_F(PerspectiveApiTest, ThresholdRespected) {
    config_.threshold = 0.5;
    PerspectiveApi api(config_);
    std::string response = R"({
        "attributeScores": {
            "TOXICITY": {
                "summaryScore": {"value": 0.55, "type": "PROBABILITY"}
            }
        }
    })";
    auto result = api.parseResponse(response);
    EXPECT_TRUE(result.flagged);

    config_.threshold = 0.9;
    PerspectiveApi api2(config_);
    result = api2.parseResponse(response);
    EXPECT_FALSE(result.flagged);
}

TEST_F(PerspectiveApiTest, CheckWithMockHttpSuccess) {
    HttpPostFn mock_fn = [](const std::string& /*url*/, const std::string& /*body*/,
                            const std::vector<std::pair<std::string, std::string>>& /*headers*/,
                            int /*timeout*/) -> std::pair<int, std::string> {
        return {200, R"({
            "attributeScores": {
                "TOXICITY": {
                    "summaryScore": {"value": 0.92, "type": "PROBABILITY"}
                }
            }
        })"};
    };

    PerspectiveApi api(config_, mock_fn);
    auto result = api.check("toxic content");
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.flagged);
    EXPECT_EQ(result.provider, "google_perspective");
}

TEST_F(PerspectiveApiTest, CheckWithMockHttpError) {
    HttpPostFn mock_fn = [](const std::string&, const std::string&,
                            const std::vector<std::pair<std::string, std::string>>&,
                            int) -> std::pair<int, std::string> {
        return {403, "Forbidden"};
    };

    PerspectiveApi api(config_, mock_fn);
    auto result = api.check("test");
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.find("403") != std::string::npos);
}

TEST_F(PerspectiveApiTest, CheckWithoutApiKey) {
    config_.api_key = "";
    PerspectiveApi api(config_);
    auto result = api.check("test");
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.find("not configured") != std::string::npos);
}

TEST_F(PerspectiveApiTest, UrlContainsApiKey) {
    std::string captured_url;
    HttpPostFn mock_fn = [&captured_url](
        const std::string& url, const std::string&,
        const std::vector<std::pair<std::string, std::string>>&,
        int) -> std::pair<int, std::string> {
        captured_url = url;
        return {200, R"({"attributeScores": {}})"};
    };

    PerspectiveApi api(config_, mock_fn);
    api.check("test");
    EXPECT_TRUE(captured_url.find("key=test-key") != std::string::npos);
    EXPECT_TRUE(captured_url.find("comments:analyze") != std::string::npos);
}
