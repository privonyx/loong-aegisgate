#include <gtest/gtest.h>
#include "aegisgate/feedback_event.h"
#include "observe/feedback_bus.h"
#include "observe/metrics.h"
#include <chrono>
#include <string>

using namespace aegisgate;
using namespace std::chrono_literals;

namespace {

// Shared helper to reset the FeedbackBus singleton state between tests.
class FeedbackBusFixture : public ::testing::Test {
protected:
    void SetUp() override {
        auto& bus = FeedbackBus::instance();
        bus.shutdown();  // idempotent — guarantees clean state
        FeedbackBusConfig disabled;
        disabled.enabled = false;
        bus.reconfigure(std::move(disabled));
    }
    void TearDown() override {
        auto& bus = FeedbackBus::instance();
        bus.shutdown();
    }
};

FeedbackEvent makeEvent(FeedbackEventType t = FeedbackEventType::GuardFeedback) {
    FeedbackEvent e;
    e.type = t;
    e.topic = FeedbackEvent::topicOf(t);
    e.request_id = "r-test";
    e.timestamp = std::chrono::system_clock::now();
    e.payload = {{"note", "test"}};
    return e;
}

} // namespace

// =============================================================================
// (1) Disabled by default — zero runtime cost
// =============================================================================

TEST_F(FeedbackBusFixture, DisabledByDefaultMeansNoSubscribers) {
    // Runtime wiring not exercised: bus stays at zero subscribers.
    auto stats = FeedbackBus::instance().stats();
    EXPECT_EQ(stats.subscriber_count, 0u);
}

// =============================================================================
// (2) MetricsRegistry exposes feedback_events_total — lazy + idempotent
// =============================================================================

TEST(MetricsRegistryFeedback, FeedbackEventsCounterExists) {
    auto& reg = MetricsRegistry::instance();
    auto& c1 = reg.feedbackEventsTotal();
    auto& c2 = reg.feedbackEventsTotal();
    EXPECT_EQ(&c1, &c2);
    EXPECT_EQ(c1.name(), "feedback_events_total");
}

TEST(MetricsRegistryFeedback, ExposeAllIncludesFeedbackCounter) {
    auto& reg = MetricsRegistry::instance();
    reg.feedbackEventsTotal();  // ensure materialised
    auto all = reg.exposeAll();
    EXPECT_NE(all.find("feedback_events_total"), std::string::npos);
}

// =============================================================================
// (3) End-to-end: start bus, attach metrics subscriber, publish, verify counter
//     (simulates what GatewayRuntime::initialize will do)
// =============================================================================

#include "observe/metrics_feedback_subscriber.h"

namespace {
LabelSet makeTypeLabel(const std::string& topic) {
    LabelSet ls;
    ls.labels.emplace_back("type", topic);
    return ls;
}
}

TEST_F(FeedbackBusFixture, EnabledBusWithMetricsSubscriberIncrementsCounter) {
    auto& bus = FeedbackBus::instance();
    auto& counter = MetricsRegistry::instance().feedbackEventsTotal();
    const double baseline = counter.get(makeTypeLabel("guard.feedback"));

    FeedbackBusConfig cfg;
    cfg.enabled = true;
    cfg.max_queue_size = 1000;
    cfg.drop_policy = "oldest";
    bus.reconfigure(std::move(cfg));
    bus.start();

    MetricsFeedbackSubscriber sub(counter);
    sub.attach(bus);
    EXPECT_GE(bus.stats().subscriber_count, 1u);

    EXPECT_TRUE(bus.publish(makeEvent(FeedbackEventType::GuardFeedback)));
    EXPECT_TRUE(bus.publish(makeEvent(FeedbackEventType::GuardFeedback)));
    EXPECT_TRUE(bus.flush(1s));

    const double after = counter.get(makeTypeLabel("guard.feedback"));
    EXPECT_EQ(after - baseline, 2.0);
}

TEST_F(FeedbackBusFixture, MetricsSubscriberRoutesUnknownTopicAsSelf) {
    auto& bus = FeedbackBus::instance();
    auto& counter = MetricsRegistry::instance().feedbackEventsTotal();

    FeedbackBusConfig cfg;
    cfg.enabled = true;
    cfg.max_queue_size = 100;
    bus.reconfigure(std::move(cfg));
    bus.start();
    MetricsFeedbackSubscriber sub(counter);
    sub.attach(bus);

    FeedbackEvent e;
    e.type = FeedbackEventType::Custom;
    e.topic = "phase11.custom.xyz";
    e.timestamp = std::chrono::system_clock::now();
    const double baseline = counter.get(makeTypeLabel("phase11.custom.xyz"));
    EXPECT_TRUE(bus.publish(std::move(e)));
    EXPECT_TRUE(bus.flush(1s));
    EXPECT_EQ(counter.get(makeTypeLabel("phase11.custom.xyz")) - baseline, 1.0);
}

// =============================================================================
// (4) Lifecycle — flush + shutdown cooperate with metrics subscriber
// =============================================================================

TEST_F(FeedbackBusFixture, ShutdownDrainsPendingEventsToSubscribers) {
    auto& bus = FeedbackBus::instance();
    auto& counter = MetricsRegistry::instance().feedbackEventsTotal();

    FeedbackBusConfig cfg;
    cfg.enabled = true;
    cfg.max_queue_size = 1000;
    bus.reconfigure(std::move(cfg));
    bus.start();
    MetricsFeedbackSubscriber sub(counter);
    sub.attach(bus);

    const double baseline = counter.get(makeTypeLabel("router.outcome"));
    for (int i = 0; i < 25; ++i) {
        bus.publish(makeEvent(FeedbackEventType::RouterOutcome));
    }

    // shutdown() synchronously drains.
    bus.shutdown();
    const double after = counter.get(makeTypeLabel("router.outcome"));
    EXPECT_EQ(after - baseline, 25.0);
}

TEST_F(FeedbackBusFixture, PublishAfterShutdownReturnsFalse) {
    auto& bus = FeedbackBus::instance();
    FeedbackBusConfig cfg;
    cfg.enabled = true;
    bus.reconfigure(std::move(cfg));
    bus.start();
    bus.shutdown();
    EXPECT_FALSE(bus.publish(makeEvent()));
}

// =============================================================================
// (5) Reconfigure semantics — simulates SIGHUP-style reload
// =============================================================================

TEST_F(FeedbackBusFixture, ReconfigureDisableStopsAcceptingPublishes) {
    auto& bus = FeedbackBus::instance();
    FeedbackBusConfig enabled;
    enabled.enabled = true;
    bus.reconfigure(enabled);
    bus.start();
    EXPECT_TRUE(bus.publish(makeEvent()));

    FeedbackBusConfig disabled;
    disabled.enabled = false;
    bus.reconfigure(std::move(disabled));
    EXPECT_FALSE(bus.publish(makeEvent()));
}

TEST_F(FeedbackBusFixture, ReconfigureResizesQueueAndAcceptsLargerBatches) {
    auto& bus = FeedbackBus::instance();
    // FeedbackBus stats counters are process-wide and accumulate across
    // tests sharing the singleton; capture baselines.
    const uint64_t baseline_dropped = bus.stats().dropped_queue_full;
    const uint64_t baseline_published = bus.stats().published;

    // start() is required because shutdown() in PublishAfterShutdownReturnsFalse
    // (or any prior test) sets stopping_=true; start() resets it.
    FeedbackBusConfig small;
    small.enabled = true;
    small.max_queue_size = 2;
    bus.reconfigure(small);
    bus.start();

    // 3 publishes into a 2-capacity queue: at least one should be either
    // dropped OR delivered before being counted, depending on dispatcher
    // race. We verify the bus is operational and the resize works.
    EXPECT_TRUE(bus.publish(makeEvent()));
    EXPECT_TRUE(bus.publish(makeEvent()));
    EXPECT_TRUE(bus.publish(makeEvent()));
    EXPECT_TRUE(bus.flush(1s));
    EXPECT_GE(bus.stats().published - baseline_published, 3u);
    (void)baseline_dropped;  // dropped depends on dispatcher race; not asserted

    // Now resize up and confirm a much larger burst is fully accepted with
    // zero new drops (since the dispatcher keeps draining).
    FeedbackBusConfig bigger;
    bigger.enabled = true;
    bigger.max_queue_size = 1000;
    bus.reconfigure(std::move(bigger));
    const uint64_t before = bus.stats().dropped_queue_full;
    for (int i = 0; i < 50; ++i) bus.publish(makeEvent());
    EXPECT_TRUE(bus.flush(1s));
    EXPECT_EQ(bus.stats().dropped_queue_full, before);
}

// =============================================================================
// (6) Singleton across consumers (what GatewayRuntime relies on)
// =============================================================================

TEST(FeedbackBusSingletonShared, InstanceSharedBetweenCallers) {
    auto& a = FeedbackBus::instance();
    auto& b = FeedbackBus::instance();
    EXPECT_EQ(&a, &b);
}
