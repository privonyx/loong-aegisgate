#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>

namespace aegisgate {

struct SafetyCategory {
    std::string name;
    double score = 0.0;
    bool flagged = false;
};

struct SafetyResult {
    bool flagged = false;
    std::vector<SafetyCategory> categories;
    std::string provider;
    std::chrono::milliseconds latency{0};
    std::string error;
    bool success = true;
};

using HttpPostFn = std::function<std::pair<int, std::string>(
    const std::string& url,
    const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& headers,
    int timeout_ms)>;

class ExternalSafetyApi {
public:
    virtual ~ExternalSafetyApi() = default;

    virtual SafetyResult check(const std::string& text) = 0;
    virtual std::string providerName() const = 0;
    virtual bool isConfigured() const = 0;
};

} // namespace aegisgate
