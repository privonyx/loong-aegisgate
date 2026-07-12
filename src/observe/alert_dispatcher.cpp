#include "observe/alert_dispatcher.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace aegisgate {

void AlertDispatcher::addChannel(const std::string& name, Channel channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    channels_.emplace_back(name, std::move(channel));
}

void AlertDispatcher::removeChannel(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    channels_.erase(
        std::remove_if(channels_.begin(), channels_.end(),
                       [&](const auto& p) { return p.first == name; }),
        channels_.end());
}

void AlertDispatcher::dispatch(const Alert& alert) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (channels_.empty()) {
        dropped_no_channel_total_.fetch_add(1, std::memory_order_relaxed);
        spdlog::warn("AlertDispatcher: alert '{}' dropped, no channels configured",
                     alert.rule_id);
        return;
    }
    for (const auto& [name, channel] : channels_) {
        try {
            channel(alert);
            delivered_total_.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& e) {
            failed_total_.fetch_add(1, std::memory_order_relaxed);
            spdlog::error("Alert channel '{}' failed: {}", name, e.what());
        } catch (...) {
            failed_total_.fetch_add(1, std::memory_order_relaxed);
            spdlog::error("Alert channel '{}' failed with unknown error", name);
        }
    }
}

size_t AlertDispatcher::channelCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return channels_.size();
}

} // namespace aegisgate
