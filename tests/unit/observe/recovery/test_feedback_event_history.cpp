// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 1 task 1.4.
//
// FeedbackEventHistory tests (5).

#include "aegisgate/feedback_event.h"
#include "observe/feedback_bus.h"
#include "observe/recovery/feedback_event_history.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace aegisgate;

namespace {

FeedbackBusConfig enabledConfig(size_t cap = 1000) {
    FeedbackBusConfig c;
    c.enabled = true;
    c.max_queue_size = cap;
    return c;
}

FeedbackEvent makeEvent(FeedbackEventType t,
                          const std::string& tenant = "tenant-a") {
    FeedbackEvent e;
    e.type = t;
    e.topic = FeedbackEvent::topicOf(t);
    e.tenant_id = tenant;
    e.timestamp = std::chrono::system_clock::now();
    return e;
}

} // namespace

TEST(FeedbackEventHistoryTest, SubscribeAddsEntry) {
    FeedbackBus bus(enabledConfig());
    bus.start();

    FeedbackEventHistory hist(bus, /*capacity=*/100);

    bus.publish(makeEvent(FeedbackEventType::OpsIncident));
    ASSERT_TRUE(bus.flush(std::chrono::milliseconds(500)));

    auto entries = hist.snapshotByType(FeedbackEventType::OpsIncident, 10);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].type, FeedbackEventType::OpsIncident);

    bus.shutdown();
}

TEST(FeedbackEventHistoryTest, CapacityFifoEviction) {
    FeedbackBus bus(enabledConfig());
    bus.start();

    FeedbackEventHistory hist(bus, /*capacity=*/3);

    for (int i = 0; i < 5; ++i) {
        bus.publish(makeEvent(FeedbackEventType::OpsIncident,
                                "tenant-" + std::to_string(i)));
    }
    ASSERT_TRUE(bus.flush(std::chrono::milliseconds(500)));

    auto all = hist.snapshotByType(FeedbackEventType::OpsIncident, 100);
    EXPECT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0].tenant_id, "tenant-2");
    EXPECT_EQ(all[2].tenant_id, "tenant-4");

    bus.shutdown();
}

TEST(FeedbackEventHistoryTest, UnsubscribesOnDestroy) {
    FeedbackBus bus(enabledConfig());
    bus.start();

    {
        FeedbackEventHistory hist(bus, /*capacity=*/10);
        bus.publish(makeEvent(FeedbackEventType::OpsIncident));
        ASSERT_TRUE(bus.flush(std::chrono::milliseconds(500)));
        EXPECT_EQ(hist.snapshotByType(FeedbackEventType::OpsIncident, 10).size(),
                  1u);
    }
    // After destroy, publishing more events should be safe (no dangling
    // callback). Verify bus stats subscriber_count returns to 0.
    auto stats = bus.stats();
    EXPECT_EQ(stats.subscriber_count, 0u);

    bus.publish(makeEvent(FeedbackEventType::OpsIncident));
    ASSERT_TRUE(bus.flush(std::chrono::milliseconds(500)));

    bus.shutdown();
}

TEST(FeedbackEventHistoryTest, SnapshotByTypeFiltersCorrectly) {
    FeedbackBus bus(enabledConfig());
    bus.start();

    FeedbackEventHistory hist(bus, /*capacity=*/100);

    bus.publish(makeEvent(FeedbackEventType::OpsIncident));
    bus.publish(makeEvent(FeedbackEventType::OpsIncident));
    bus.publish(makeEvent(FeedbackEventType::OpsRollbackTriggered));
    bus.publish(makeEvent(FeedbackEventType::CostObservation));
    ASSERT_TRUE(bus.flush(std::chrono::milliseconds(500)));

    EXPECT_EQ(hist.snapshotByType(FeedbackEventType::OpsIncident, 100).size(),
              2u);
    EXPECT_EQ(hist.snapshotByType(FeedbackEventType::OpsRollbackTriggered, 100)
                  .size(),
              1u);
    EXPECT_EQ(hist.snapshotByType(FeedbackEventType::CostObservation, 100).size(),
              1u);

    bus.shutdown();
}

TEST(FeedbackEventHistoryTest, ConcurrentPublishSafe) {
    FeedbackBus bus(enabledConfig());
    bus.start();

    FeedbackEventHistory hist(bus, /*capacity=*/10000);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 100;

    std::atomic<int> ready{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            ready.fetch_add(1);
            while (ready.load() < kThreads) {}
            for (int i = 0; i < kPerThread; ++i) {
                bus.publish(makeEvent(FeedbackEventType::OpsIncident,
                                       "t" + std::to_string(t)));
            }
        });
    }
    for (auto& th : threads) th.join();
    ASSERT_TRUE(bus.flush(std::chrono::milliseconds(2000)));

    auto all = hist.snapshotByType(FeedbackEventType::OpsIncident, 10000);
    EXPECT_EQ(all.size(),
              static_cast<std::size_t>(kThreads * kPerThread));

    bus.shutdown();
}
