#pragma once
#include "external_safety_api.h"
#include <string>
#include <vector>

namespace aegisgate {

struct PerspectiveConfig {
    std::string api_key;
    std::string base_url = "https://commentanalyzer.googleapis.com";
    double threshold = 0.7;
    std::vector<std::string> attributes = {
        "TOXICITY", "SEVERE_TOXICITY", "IDENTITY_ATTACK",
        "INSULT", "PROFANITY", "THREAT"
    };
    int timeout_ms = 5000;
};

class PerspectiveApi : public ExternalSafetyApi {
public:
    explicit PerspectiveApi(const PerspectiveConfig& config);
    PerspectiveApi(const PerspectiveConfig& config, HttpPostFn http_fn);

    SafetyResult check(const std::string& text) override;
    std::string providerName() const override { return "google_perspective"; }
    bool isConfigured() const override;

    nlohmann::json buildRequestBody(const std::string& text) const;
    SafetyResult parseResponse(const std::string& body) const;

private:
    SafetyResult doHttpPost(const std::string& url, const std::string& body,
                            const std::vector<std::pair<std::string, std::string>>& headers);

    PerspectiveConfig config_;
    HttpPostFn http_fn_;
};

} // namespace aegisgate
