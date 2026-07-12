#include "guardrail/inbound/openai_moderation.h"
#include <spdlog/spdlog.h>
#include <chrono>

#ifdef AEGISGATE_ENABLE_CURL
#include <curl/curl.h>
#endif

namespace aegisgate {

OpenAIModeration::OpenAIModeration(const OpenAIModerationConfig& config)
    : config_(config) {}

OpenAIModeration::OpenAIModeration(const OpenAIModerationConfig& config, HttpPostFn http_fn)
    : config_(config), http_fn_(std::move(http_fn)) {}

bool OpenAIModeration::isConfigured() const {
    return !config_.api_key.empty();
}

nlohmann::json OpenAIModeration::buildRequestBody(const std::string& text) const {
    nlohmann::json body;
    body["input"] = text;
    body["model"] = config_.model;
    return body;
}

SafetyResult OpenAIModeration::parseResponse(const std::string& body) const {
    SafetyResult result;
    result.provider = providerName();
    try {
        auto json = nlohmann::json::parse(body);
        if (!json.contains("results") || !json["results"].is_array() ||
            json["results"].empty()) {
            result.error = "Invalid response: missing results array";
            result.success = false;
            return result;
        }
        const auto& r = json["results"][0];
        result.flagged = r.value("flagged", false);

        if (r.contains("categories") && r["categories"].is_object()) {
            const auto& cats = r["categories"];
            const auto& scores = r.contains("category_scores") ?
                r["category_scores"] : nlohmann::json::object();

            for (auto it = cats.begin(); it != cats.end(); ++it) {
                SafetyCategory cat;
                cat.name = it.key();
                cat.flagged = it.value().get<bool>();
                if (scores.contains(it.key())) {
                    cat.score = scores[it.key()].get<double>();
                }
                result.categories.push_back(std::move(cat));
            }
        }
    } catch (const nlohmann::json::exception& e) {
        result.error = std::string("JSON parse error: ") + e.what();
        result.success = false;
    }
    return result;
}

SafetyResult OpenAIModeration::doHttpPost(
    const std::string& url, const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& headers) {
    SafetyResult result;
    result.provider = providerName();

    if (http_fn_) {
        auto start = std::chrono::steady_clock::now();
        auto [status, resp_body] = http_fn_(url, body, headers, config_.timeout_ms);
        auto end = std::chrono::steady_clock::now();
        result.latency = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        if (status == 200) {
            return parseResponse(resp_body);
        }
        result.error = "HTTP " + std::to_string(status) + ": " + resp_body;
        result.success = false;
        return result;
    }

#ifdef AEGISGATE_ENABLE_CURL
    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "Failed to init curl";
        result.success = false;
        return result;
    }

    std::string response_body;
    auto write_cb = [](char* ptr, size_t, size_t nmemb, void* ud) -> size_t {
        static_cast<std::string*>(ud)->append(ptr, nmemb);
        return nmemb;
    };

    struct curl_slist* h_list = nullptr;
    for (const auto& [k, v] : headers) {
        h_list = curl_slist_append(h_list, (k + ": " + v).c_str());
    }
    h_list = curl_slist_append(h_list, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h_list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(config_.timeout_ms));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    auto start = std::chrono::steady_clock::now();
    CURLcode res = curl_easy_perform(curl);
    auto end = std::chrono::steady_clock::now();
    result.latency = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    if (res != CURLE_OK) {
        result.error = std::string("curl error: ") + curl_easy_strerror(res);
        result.success = false;
        curl_slist_free_all(h_list);
        curl_easy_cleanup(curl);
        return result;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(h_list);
    curl_easy_cleanup(curl);

    if (http_code == 200) {
        auto parsed = parseResponse(response_body);
        parsed.latency = result.latency;
        return parsed;
    }
    result.error = "HTTP " + std::to_string(http_code) + ": " + response_body;
    result.success = false;
    return result;
#else
    result.error = "No HTTP client available (CURL not enabled and no custom HttpPostFn)";
    result.success = false;
    return result;
#endif
}

SafetyResult OpenAIModeration::check(const std::string& text) {
    if (!isConfigured()) {
        SafetyResult r;
        r.provider = providerName();
        r.error = "API key not configured";
        r.success = false;
        return r;
    }

    auto body = buildRequestBody(text);
    std::string url = config_.base_url + "/v1/moderations";
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", "Bearer " + config_.api_key}
    };

    auto result = doHttpPost(url, body.dump(), headers);
    if (result.flagged) {
        spdlog::warn("OpenAI Moderation flagged content: {} categories flagged",
                     [&]() {
                         int count = 0;
                         for (const auto& c : result.categories)
                             if (c.flagged) count++;
                         return count;
                     }());
    }
    return result;
}

} // namespace aegisgate
