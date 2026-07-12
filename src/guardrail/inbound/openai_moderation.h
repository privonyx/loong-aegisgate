#pragma once
#include "external_safety_api.h"
#include <string>
#include <mutex>

namespace aegisgate {

struct OpenAIModerationConfig {
    std::string api_key;
    std::string base_url = "https://api.openai.com";
    std::string model = "omni-moderation-latest";
    int timeout_ms = 5000;
};

class OpenAIModeration : public ExternalSafetyApi {
public:
    explicit OpenAIModeration(const OpenAIModerationConfig& config);
    OpenAIModeration(const OpenAIModerationConfig& config, HttpPostFn http_fn);

    SafetyResult check(const std::string& text) override;
    std::string providerName() const override { return "openai_moderation"; }
    bool isConfigured() const override;

    nlohmann::json buildRequestBody(const std::string& text) const;
    SafetyResult parseResponse(const std::string& body) const;

private:
    SafetyResult doHttpPost(const std::string& url, const std::string& body,
                            const std::vector<std::pair<std::string, std::string>>& headers);

    OpenAIModerationConfig config_;
    HttpPostFn http_fn_;
};

} // namespace aegisgate
