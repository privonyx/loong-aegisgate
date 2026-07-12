#include "observe/metrics_feedback_subscriber.h"
#include <utility>

namespace aegisgate {

MetricsFeedbackSubscriber::MetricsFeedbackSubscriber(Counter& counter)
    : counter_(counter) {}

MetricsFeedbackSubscriber::~MetricsFeedbackSubscriber() {
    if (bus_ != nullptr) {
        bus_->unsubscribe(id_);
    }
}

size_t MetricsFeedbackSubscriber::attach(FeedbackBus& bus) {
    // If already attached to a (possibly different) bus, detach first to
    // avoid leaking a dangling lambda referencing `this`.
    if (bus_ != nullptr) {
        bus_->unsubscribe(id_);
        bus_ = nullptr;
        id_ = 0;
    }
    id_ = bus.subscribe([this](const FeedbackEvent& event) {
        LabelSet labels;
        labels.labels.emplace_back(
            "type",
            event.topic.empty() ? std::string("custom") : event.topic);
        counter_.inc(labels);
    });
    bus_ = &bus;
    return id_;
}

void MetricsFeedbackSubscriber::detach(FeedbackBus& bus) {
    if (bus_ == &bus && id_ != 0) {
        bus.unsubscribe(id_);
        bus_ = nullptr;
        id_ = 0;
    }
}

} // namespace aegisgate
