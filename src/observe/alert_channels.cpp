#include "observe/alert_channels.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#ifdef AEGISGATE_ENABLE_CURL
#include <curl/curl.h>
#endif

namespace aegisgate {

namespace {

std::string severityString(AlertSeverity sev) {
    switch (sev) {
    case AlertSeverity::Info: return "info";
    case AlertSeverity::Warning: return "warning";
    case AlertSeverity::Critical: return "critical";
    }
    return "unknown";
}

// P2-#5: previously a no-op, so every configured webhook/DingTalk/Feishu/Slack
// channel silently dropped alerts in production whenever no transport was
// injected (the assembler never injects one). Now performs a real fire-and-
// forget HTTP POST when libcurl is compiled in; without CURL it degrades to a
// warning so the operator knows alerts are not actually leaving the process.
void defaultTransport(const std::string& url,
                      const std::string& body,
                      const std::string& content_type) {
#ifdef AEGISGATE_ENABLE_CURL
    if (url.empty()) {
        spdlog::warn("Alert channel: empty webhook URL, dropping alert");
        return;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        spdlog::error("Alert channel: failed to init curl");
        return;
    }
    std::string sink;
    auto write_cb = [](char* ptr, size_t, size_t nmemb, void* ud) -> size_t {
        static_cast<std::string*>(ud)->append(ptr, nmemb);
        return nmemb;
    };
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Content-Type: " + content_type).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        spdlog::warn("Alert channel: webhook POST failed: {}",
                     curl_easy_strerror(res));
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code < 200 || http_code >= 300) {
            spdlog::warn("Alert channel: webhook returned HTTP {}", http_code);
        }
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
#else
    (void)url;
    (void)body;
    (void)content_type;
    spdlog::warn("Alert channel: CURL not compiled in — alert NOT delivered "
                 "(rebuild with -DENABLE_CURL=ON or inject an HttpTransport)");
#endif
}

} // namespace

AlertDispatcher::Channel makeWebhookChannel(
    const std::string& url,
    const std::string& secret,
    HttpTransport transport) {
    if (!transport) transport = defaultTransport;
    return [url, secret, transport](const Alert& alert) {
        nlohmann::json j;
        j["rule"] = alert.rule_id;
        j["description"] = alert.description;
        j["severity"] = severityString(alert.severity);
        j["current_value"] = alert.current_value;
        j["threshold"] = alert.threshold;
        j["timestamp"] = alert.timestamp;
        if (!secret.empty()) {
            j["signed"] = true;
        }
        transport(url, j.dump(), "application/json");
    };
}

AlertDispatcher::Channel makeDingTalkChannel(
    const std::string& webhook_url,
    HttpTransport transport) {
    if (!transport) transport = defaultTransport;
    return [webhook_url, transport](const Alert& alert) {
        nlohmann::json j;
        j["msgtype"] = "markdown";
        j["markdown"]["title"] = "AegisGate Alert: " + alert.rule_id;
        j["markdown"]["text"] = "### " + alert.rule_id + "\n"
            "- **Severity:** " + severityString(alert.severity) + "\n"
            "- **Description:** " + alert.description + "\n"
            "- **Value:** " + std::to_string(alert.current_value) + "\n"
            "- **Threshold:** " + std::to_string(alert.threshold) + "\n"
            "- **Time:** " + alert.timestamp;
        transport(webhook_url, j.dump(), "application/json");
    };
}

AlertDispatcher::Channel makeFeishuChannel(
    const std::string& webhook_url,
    HttpTransport transport) {
    if (!transport) transport = defaultTransport;
    return [webhook_url, transport](const Alert& alert) {
        nlohmann::json card;
        card["msg_type"] = "interactive";
        card["card"]["header"]["title"]["tag"] = "plain_text";
        card["card"]["header"]["title"]["content"] = "AegisGate Alert: " + alert.rule_id;
        card["card"]["header"]["template"] =
            (alert.severity == AlertSeverity::Critical) ? "red" : "orange";

        nlohmann::json element;
        element["tag"] = "markdown";
        element["content"] = "**Severity:** " + severityString(alert.severity) + "\n"
            "**Description:** " + alert.description + "\n"
            "**Value:** " + std::to_string(alert.current_value) + "\n"
            "**Threshold:** " + std::to_string(alert.threshold) + "\n"
            "**Time:** " + alert.timestamp;
        card["card"]["elements"] = nlohmann::json::array({element});

        transport(webhook_url, card.dump(), "application/json");
    };
}

AlertDispatcher::Channel makeSlackChannel(
    const std::string& webhook_url,
    HttpTransport transport) {
    if (!transport) transport = defaultTransport;
    return [webhook_url, transport](const Alert& alert) {
        nlohmann::json j;
        nlohmann::json section;
        section["type"] = "section";
        section["text"]["type"] = "mrkdwn";
        section["text"]["text"] = "*" + alert.rule_id + "* (" +
            severityString(alert.severity) + ")\n" +
            alert.description + "\n"
            "Value: " + std::to_string(alert.current_value) +
            " | Threshold: " + std::to_string(alert.threshold) +
            "\n_" + alert.timestamp + "_";
        j["blocks"] = nlohmann::json::array({section});
        transport(webhook_url, j.dump(), "application/json");
    };
}

} // namespace aegisgate
