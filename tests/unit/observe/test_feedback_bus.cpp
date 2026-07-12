#include <gtest/gtest.h>
#include "aegisgate/feedback_event.h"
#include "observe/feedback_bus.h"
#include "observe/metrics_feedback_subscriber.h"
#include "observe/metrics.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace aegisgate;
using namespace std::chrono_literals;

namespace {

FeedbackBusConfig enabledCfg(size_t max_queue = 10000) {
    FeedbackBusConfig c;
    c.enabled = true;
    c.max_queue_size = max_queue;
    c.drop_policy = "oldest";
    return c;
}

FeedbackEvent makeEvent(FeedbackEventType t = FeedbackEventType::GuardFeedback) {
    FeedbackEvent e;
    e.type = t;
    e.topic = FeedbackEvent::topicOf(t);
    e.request_id = "r-1";
    e.tenant_id = "t-a";
    e.source = "unit-test";
    e.timestamp = std::chrono::system_clock::now();
    e.payload = {{"note", "hello"}};
    return e;
}

} // namespace

// =============================================================================
// Event schema
// =============================================================================

TEST(FeedbackEventSchema, TopicOfStableMapping) {
    EXPECT_EQ(FeedbackEvent::topicOf(FeedbackEventType::GuardFeedback), "guard.feedback");
    EXPECT_EQ(FeedbackEvent::topicOf(FeedbackEventType::GuardAnomalyFlagged), "guard.anomaly");
    EXPECT_EQ(FeedbackEvent::topicOf(FeedbackEventType::RouterOutcome), "router.outcome");
    EXPECT_EQ(FeedbackEvent::topicOf(FeedbackEventType::RouterDecision), "router.decision");
    EXPECT_EQ(FeedbackEvent::topicOf(FeedbackEventType::QualityFeedback), "quality.feedback");
    EXPECT_EQ(FeedbackEvent::topicOf(FeedbackEventType::QualityDrift), "quality.drift");
    EXPECT_EQ(FeedbackEvent::topicOf(FeedbackEventType::CostObservation), "cost.observation");
    EXPECT_EQ(FeedbackEvent::topicOf(FeedbackEventType::BudgetAlert), "cost.budget_alert");
    EXPECT_EQ(FeedbackEvent::topicOf(FeedbackEventType::OpsIncident), "ops.incident");
    EXPECT_EQ(FeedbackEvent::topicOf(FeedbackEventType::OpsRollbackTriggered), "ops.rollback");
    EXPECT_EQ(FeedbackEvent::topicOf(FeedbackEventType::Custom), "custom");
}

TEST(FeedbackEventSchema, TypeOfStableMapping) {
    EXPECT_EQ(FeedbackEvent::typeOf("guard.feedback"), FeedbackEventType::GuardFeedback);
    EXPECT_EQ(FeedbackEvent::typeOf("cost.observation"), FeedbackEventType::CostObservation);
    EXPECT_EQ(FeedbackEvent::typeOf("ops.rollback"), FeedbackEventType::OpsRollbackTriggered);
}

TEST(FeedbackEventSchema, BidirectionalConsistency) {
    const FeedbackEventType all[] = {
        FeedbackEventType::GuardFeedback, FeedbackEventType::GuardAnomalyFlagged,
        FeedbackEventType::RouterOutcome, FeedbackEventType::RouterDecision,
        FeedbackEventType::QualityFeedback, FeedbackEventType::QualityDrift,
        FeedbackEventType::CostObservation, FeedbackEventType::BudgetAlert,
        FeedbackEventType::OpsIncident, FeedbackEventType::OpsRollbackTriggered,
        FeedbackEventType::Custom
    };
    for (auto t : all) {
        EXPECT_EQ(FeedbackEvent::typeOf(FeedbackEvent::topicOf(t)), t);
    }
}

TEST(FeedbackEventSchema, ToJsonFromJsonRoundTrip) {
    auto e = makeEvent(FeedbackEventType::RouterOutcome);
    e.payload = {{"model", "gpt-4o"}, {"latency_ms", 123}};
    auto j = e.toJson();
    auto back = FeedbackEvent::fromJson(j);
    EXPECT_EQ(back.type, e.type);
    EXPECT_EQ(back.topic, e.topic);
    EXPECT_EQ(back.request_id, e.request_id);
    EXPECT_EQ(back.tenant_id, e.tenant_id);
    EXPECT_EQ(back.source, e.source);
    EXPECT_EQ(back.payload, e.payload);
}

TEST(FeedbackEventSchema, FromJsonUnknownTopicMapsToCustom) {
    nlohmann::json j = {
        {"topic", "nonexistent.xyz"},
        {"request_id", "r-2"},
        {"payload", {{"x", 1}}}
    };
    auto e = FeedbackEvent::fromJson(j);
    EXPECT_EQ(e.type, FeedbackEventType::Custom);
    EXPECT_EQ(e.topic, "nonexistent.xyz");
}

TEST(FeedbackEventSchema, FromJsonWithoutPayloadDoesNotCrash) {
    nlohmann::json j = {{"topic", "guard.feedback"}};
    auto e = FeedbackEvent::fromJson(j);
    EXPECT_EQ(e.type, FeedbackEventType::GuardFeedback);
    EXPECT_TRUE(e.payload.is_null() || e.payload.is_object());
}

// =============================================================================
// Subscribe / unsubscribe
// =============================================================================

TEST(FeedbackBusSubscribe, ReturnsMonotonicIds) {
    FeedbackBus bus(enabledCfg());
    bus.start();
    auto id1 = bus.subscribe([](const FeedbackEvent&){});
    auto id2 = bus.subscribe([](const FeedbackEvent&){});
    auto id3 = bus.subscribe([](const FeedbackEvent&){});
    EXPECT_LT(id1, id2);
    EXPECT_LT(id2, id3);
    bus.shutdown();
}

TEST(FeedbackBusSubscribe, UnsubscribeRemovesInConstantTime) {
    FeedbackBus bus(enabledCfg());
    bus.start();
    auto id = bus.subscribe([](const FeedbackEvent&){});
    EXPECT_EQ(bus.stats().subscriber_count, 1u);
    bus.unsubscribe(id);
    EXPECT_EQ(bus.stats().subscriber_count, 0u);
    bus.shutdown();
}

TEST(FeedbackBusSubscribe, UnsubscribedCallbackNoLongerInvoked) {
    FeedbackBus bus(enabledCfg());
    bus.start();
    std::atomic<int> hits{0};
    auto id = bus.subscribe([&](const FeedbackEvent&) { hits.fetch_add(1); });
    bus.unsubscribe(id);
    bus.publish(makeEvent());
    bus.flush(500ms);
    EXPECT_EQ(hits.load(), 0);
    bus.shutdown();
}

TEST(FeedbackBusSubscribe, SameCallbackRegisteredTwiceIsIndependent) {
    FeedbackBus bus(enabledCfg());
    bus.start();
    std::atomic<int> hits{0};
    auto cb = [&](const FeedbackEvent&) { hits.fetch_add(1); };
    auto id1 = bus.subscribe(cb);
    auto id2 = bus.subscribe(cb);
    EXPECT_NE(id1, id2);
    bus.publish(makeEvent());
    bus.flush(500ms);
    EXPECT_EQ(hits.load(), 2);
    bus.shutdown();
}

// =============================================================================
// Delivery
// =============================================================================

TEST(FeedbackBusDelivery, EmptyBusFlushReturnsTrueImmediately) {
    FeedbackBus bus(enabledCfg());
    bus.start();
    EXPECT_TRUE(bus.flush(100ms));
    bus.shutdown();
}

TEST(FeedbackBusDelivery, PublishThenFlushDeliversToAllSubscribers) {
    FeedbackBus bus(enabledCfg());
    bus.start();
    std::atomic<int> a{0}, b{0};
    bus.subscribe([&](const FeedbackEvent&) { a.fetch_add(1); });
    bus.subscribe([&](const FeedbackEvent&) { b.fetch_add(1); });
    for (int i = 0; i < 5; ++i) bus.publish(makeEvent());
    EXPECT_TRUE(bus.flush(1s));
    EXPECT_EQ(a.load(), 5);
    EXPECT_EQ(b.load(), 5);
    bus.shutdown();
}

TEST(FeedbackBusDelivery, TopicFilterExactMatch) {
    FeedbackBus bus(enabledCfg());
    bus.start();
    std::atomic<int> guard_hits{0}, router_hits{0};
    bus.subscribe([&](const FeedbackEvent&) { guard_hits.fetch_add(1); }, "guard.feedback");
    bus.subscribe([&](const FeedbackEvent&) { router_hits.fetch_add(1); }, "router.outcome");
    bus.publish(makeEvent(FeedbackEventType::GuardFeedback));
    bus.publish(makeEvent(FeedbackEventType::RouterOutcome));
    bus.publish(makeEvent(FeedbackEventType::GuardFeedback));
    EXPECT_TRUE(bus.flush(1s));
    EXPECT_EQ(guard_hits.load(), 2);
    EXPECT_EQ(router_hits.load(), 1);
    bus.shutdown();
}

TEST(FeedbackBusDelivery, TopicFilterPrefixMatch) {
    FeedbackBus bus(enabledCfg());
    bus.start();
    std::atomic<int> guard_all{0};
    bus.subscribe([&](const FeedbackEvent&) { guard_all.fetch_add(1); }, "guard.");
    bus.publish(makeEvent(FeedbackEventType::GuardFeedback));
    bus.publish(makeEvent(FeedbackEventType::GuardAnomalyFlagged));
    bus.publish(makeEvent(FeedbackEventType::RouterOutcome));
    EXPECT_TRUE(bus.flush(1s));
    EXPECT_EQ(guard_all.load(), 2);
    bus.shutdown();
}

TEST(FeedbackBusDelivery, NonMatchingFilterDoesNotInvoke) {
    FeedbackBus bus(enabledCfg());
    bus.start();
    std::atomic<int> hits{0};
    bus.subscribe([&](const FeedbackEvent&) { hits.fetch_add(1); }, "does.not.exist");
    for (int i = 0; i < 3; ++i) bus.publish(makeEvent());
    EXPECT_TRUE(bus.flush(500ms));
    EXPECT_EQ(hits.load(), 0);
    bus.shutdown();
}

// =============================================================================
// Bounded queue / drop policy
// =============================================================================

TEST(FeedbackBusQueue, ExceedingMaxSizeIncrementsDropCounter) {
    FeedbackBus bus(enabledCfg(/*max_queue=*/4));
    // Do NOT start — so events stay in the queue and drops can be observed deterministically.
    for (int i = 0; i < 20; ++i) bus.publish(makeEvent());
    auto s = bus.stats();
    EXPECT_EQ(s.dropped_queue_full, 16u);
    EXPECT_EQ(s.published, 20u);
}

TEST(FeedbackBusQueue, DropOldestKeepsNewestInQueue) {
    FeedbackBus bus(enabledCfg(/*max_queue=*/2));

    // Register the subscriber BEFORE publishing — otherwise the dispatcher
    // may drain the queue between start() and subscribe() under aggressive
    // -O2 scheduling, leaving seen.size() == 0 (events delivered to an
    // empty subscriber set).
    std::vector<std::string> seen;
    std::mutex m;
    bus.subscribe([&](const FeedbackEvent& ev) {
        std::lock_guard<std::mutex> lk(m);
        seen.push_back(ev.request_id);
    });

    // Publish 3 events with distinct request_ids while the dispatcher is
    // still NOT running so we can observe the bounded-queue drop policy
    // deterministically (oldest wins eviction).
    auto e1 = makeEvent(); e1.request_id = "r-1";
    auto e2 = makeEvent(); e2.request_id = "r-2";
    auto e3 = makeEvent(); e3.request_id = "r-3";
    bus.publish(std::move(e1));
    bus.publish(std::move(e2));
    bus.publish(std::move(e3));

    // Now start the dispatcher; it drains the 2 survivors into `seen`.
    bus.start();
    bus.flush(1s);

    std::lock_guard<std::mutex> lk(m);
    // r-1 was dropped; order of delivery r-2 then r-3.
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], "r-2");
    EXPECT_EQ(seen[1], "r-3");
    bus.shutdown();
}

TEST(FeedbackBusQueue, OtherSubscribersStillReceiveAfterDrops) {
    FeedbackBus bus(enabledCfg(/*max_queue=*/2));
    bus.start();
    std::atomic<int> received{0};
    bus.subscribe([&](const FeedbackEvent&) { received.fetch_add(1); });
    for (int i = 0; i < 5; ++i) bus.publish(makeEvent());
    bus.flush(1s);
    // Every event that made it through must have been delivered at least to this subscriber.
    EXPECT_GT(received.load(), 0);
    auto s = bus.stats();
    EXPECT_EQ(s.delivered, static_cast<uint64_t>(received.load()));
    bus.shutdown();
}

// =============================================================================
// Exception isolation
// =============================================================================

TEST(FeedbackBusException, OneSubscriberThrowsDoesNotBreakOthers) {
    FeedbackBus bus(enabledCfg());
    bus.start();
    std::atomic<int> ok_hits{0};
    bus.subscribe([&](const FeedbackEvent&) { throw std::runtime_error("boom"); });
    bus.subscribe([&](const FeedbackEvent&) { ok_hits.fetch_add(1); });
    for (int i = 0; i < 3; ++i) bus.publish(makeEvent());
    bus.flush(1s);
    EXPECT_EQ(ok_hits.load(), 3);
    bus.shutdown();
}

TEST(FeedbackBusException, DeliveryErrorsCounterIncrements) {
    FeedbackBus bus(enabledCfg());
    bus.start();
    bus.subscribe([&](const FeedbackEvent&) { throw std::runtime_error("boom"); });
    bus.publish(makeEvent());
    bus.flush(500ms);
    EXPECT_GE(bus.stats().delivery_errors, 1u);
    bus.shutdown();
}

TEST(FeedbackBusException, RepeatedExceptionsDoNotCrash) {
    FeedbackBus bus(enabledCfg());
    bus.start();
    bus.subscribe([&](const FeedbackEvent&) { throw std::runtime_error("boom"); });
    for (int i = 0; i < 50; ++i) bus.publish(makeEvent());
    bus.flush(2s);
    EXPECT_GE(bus.stats().delivery_errors, 50u);
    bus.shutdown();
}

// =============================================================================
// Lifecycle
// =============================================================================

TEST(FeedbackBusLifecycle, PublishRejectedWhenNotStarted) {
    FeedbackBus bus;  // default-constructed, disabled
    EXPECT_FALSE(bus.publish(makeEvent()));
}

TEST(FeedbackBusLifecycle, StartIsIdempotent) {
    FeedbackBus bus(enabledCfg());
    bus.start();
    bus.start();
    bus.start();
    EXPECT_TRUE(bus.publish(makeEvent()));
    bus.shutdown();
}

TEST(FeedbackBusLifecycle, ShutdownBlocksUntilDrained) {
    FeedbackBus bus(enabledCfg());
    bus.start();
    std::atomic<int> hits{0};
    bus.subscribe([&](const FeedbackEvent&) {
        std::this_thread::sleep_for(1ms);
        hits.fetch_add(1);
    });
    for (int i = 0; i < 50; ++i) bus.publish(makeEvent());
    bus.shutdown();  // must drain before returning
    EXPECT_EQ(hits.load(), 50);
}

TEST(FeedbackBusLifecycle, PublishRejectedAfterShutdown) {
    FeedbackBus bus(enabledCfg());
    bus.start();
    bus.shutdown();
    EXPECT_FALSE(bus.publish(makeEvent()));
}

// =============================================================================
// Config / singleton
// =============================================================================

TEST(FeedbackBusConfigOps, ReconfigureAdjustsMaxQueueSize) {
    FeedbackBus bus(enabledCfg(/*max_queue=*/2));
    bus.start();
    FeedbackBusConfig c = enabledCfg(/*max_queue=*/100);
    bus.reconfigure(c);
    for (int i = 0; i < 50; ++i) bus.publish(makeEvent());
    bus.flush(1s);
    EXPECT_EQ(bus.stats().dropped_queue_full, 0u);
    bus.shutdown();
}

TEST(FeedbackBusConfigOps, ReconfigureDisableRejectsPublish) {
    FeedbackBus bus(enabledCfg());
    bus.start();
    FeedbackBusConfig disabled;
    disabled.enabled = false;
    bus.reconfigure(disabled);
    EXPECT_FALSE(bus.publish(makeEvent()));
    bus.shutdown();
}

TEST(FeedbackBusSingleton, InstanceReturnsSameObject) {
    auto& a = FeedbackBus::instance();
    auto& b = FeedbackBus::instance();
    EXPECT_EQ(&a, &b);
}

// =============================================================================
// Concurrency
// =============================================================================

TEST(FeedbackBusConcurrency, TenThreadsPublishingAreAllDelivered) {
    FeedbackBus bus(enabledCfg(/*max_queue=*/50000));
    bus.start();
    std::atomic<int> received{0};
    bus.subscribe([&](const FeedbackEvent&) { received.fetch_add(1); });

    const int kThreads = 10;
    const int kPerThread = 1000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                auto e = makeEvent();
                e.request_id = "t" + std::to_string(t) + "-" + std::to_string(i);
                bus.publish(std::move(e));
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_TRUE(bus.flush(5s));
    EXPECT_EQ(received.load(), kThreads * kPerThread);
    bus.shutdown();
}

TEST(FeedbackBusConcurrency, SubscribeUnsubscribeDuringPublish) {
    FeedbackBus bus(enabledCfg(/*max_queue=*/50000));
    bus.start();
    std::atomic<bool> stop{false};

    std::thread publisher([&]() {
        while (!stop.load()) {
            bus.publish(makeEvent());
            std::this_thread::sleep_for(100us);
        }
    });

    for (int i = 0; i < 200; ++i) {
        auto id = bus.subscribe([](const FeedbackEvent&){});
        std::this_thread::sleep_for(200us);
        bus.unsubscribe(id);
    }
    stop.store(true);
    publisher.join();
    bus.flush(2s);
    SUCCEED();  // primarily a TSAN-friendliness check
    bus.shutdown();
}

// =============================================================================
// MetricsFeedbackSubscriber
// =============================================================================

TEST(MetricsFeedbackSubscriberTest, IncrementsCounterByTopic) {
    Counter events("feedback_events_total_test", "test");
    FeedbackBus bus(enabledCfg());
    bus.start();
    MetricsFeedbackSubscriber sub(events);
    sub.attach(bus);

    bus.publish(makeEvent(FeedbackEventType::GuardFeedback));
    bus.publish(makeEvent(FeedbackEventType::GuardFeedback));
    bus.publish(makeEvent(FeedbackEventType::RouterOutcome));
    bus.flush(1s);

    LabelSet guard_labels;
    guard_labels.labels.emplace_back("type", "guard.feedback");
    LabelSet router_labels;
    router_labels.labels.emplace_back("type", "router.outcome");
    EXPECT_EQ(events.get(guard_labels), 2.0);
    EXPECT_EQ(events.get(router_labels), 1.0);
    bus.shutdown();
}

TEST(MetricsFeedbackSubscriberTest, UnknownTopicDoesNotCrash) {
    Counter events("feedback_events_total_test2", "test");
    FeedbackBus bus(enabledCfg());
    bus.start();
    MetricsFeedbackSubscriber sub(events);
    sub.attach(bus);

    FeedbackEvent e;
    e.type = FeedbackEventType::Custom;
    e.topic = "xxx.unknown";
    e.timestamp = std::chrono::system_clock::now();
    EXPECT_NO_THROW(bus.publish(e));
    bus.flush(500ms);
    bus.shutdown();
}
