#pragma once
#include "alert_dispatcher.h"
#include <string>
#include <functional>

namespace aegisgate {

using HttpTransport = std::function<void(const std::string& url,
                                          const std::string& body,
                                          const std::string& content_type)>;

AlertDispatcher::Channel makeWebhookChannel(
    const std::string& url,
    const std::string& secret,
    HttpTransport transport = nullptr);

AlertDispatcher::Channel makeDingTalkChannel(
    const std::string& webhook_url,
    HttpTransport transport = nullptr);

AlertDispatcher::Channel makeFeishuChannel(
    const std::string& webhook_url,
    HttpTransport transport = nullptr);

AlertDispatcher::Channel makeSlackChannel(
    const std::string& webhook_url,
    HttpTransport transport = nullptr);

} // namespace aegisgate
