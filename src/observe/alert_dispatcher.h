#pragma once
#include "alerting.h"
#include <vector>
#include <string>
#include <mutex>
#include <functional>
#include <atomic>

namespace aegisgate {

class AlertDispatcher {
public:
    using Channel = std::function<void(const Alert&)>;

    void addChannel(const std::string& name, Channel channel);
    void removeChannel(const std::string& name);
    void dispatch(const Alert& alert) const;
    size_t channelCount() const;

    // Delivery reliability counters — a channel that keeps throwing, or an
    // alert fired with no channels at all, is otherwise an invisible failure.
    size_t deliveredTotal() const {
        return delivered_total_.load(std::memory_order_relaxed);
    }
    size_t failedTotal() const {
        return failed_total_.load(std::memory_order_relaxed);
    }
    size_t droppedNoChannelTotal() const {
        return dropped_no_channel_total_.load(std::memory_order_relaxed);
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, Channel>> channels_;
    mutable std::atomic<size_t> delivered_total_{0};
    mutable std::atomic<size_t> failed_total_{0};
    mutable std::atomic<size_t> dropped_no_channel_total_{0};
};

} // namespace aegisgate
