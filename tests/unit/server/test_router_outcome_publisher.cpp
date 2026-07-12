// Phase 9.3.4 Epic D.3 — RouterOutcome publisher tests.

#include "server/router_outcome_publisher.h"
#include "observe/feedback_bus.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace aegisgate {
namespace {

TEST(RouterOutcomePublisher, PublishGeneratesCorrectEvent) {
    FeedbackBusConfig cfg;
    cfg.enabled = true;
    cfg.max_queue_size = 100;
    FeedbackBus bus(cfg);

    std::atomic<int> count{0};
    FeedbackEvent captured;
    bus.subscribe([&](const FeedbackEvent& ev) {
                       captured = ev;
                       count.fetch_add(1);
                   }, "router.outcome");
    bus.start();

    RouterOutcomePublisher pub(bus);
    pub.publish("req-001", "tenant-a", "01VER_NEW", "us-east-1",
                "openai", "gpt-4", 42.5, "success");

    for (int i = 0; i < 50 && count.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    ASSERT_EQ(count.load(), 1);
    EXPECT_EQ(captured.type, FeedbackEventType::RouterOutcome);
    EXPECT_EQ(captured.topic, "router.outcome");
    EXPECT_EQ(captured.request_id, "req-001");
    EXPECT_EQ(captured.tenant_id, "tenant-a");
    EXPECT_EQ(captured.source, "api_controller");
    EXPECT_EQ(captured.payload["version_id"], "01VER_NEW");
    EXPECT_EQ(captured.payload["region"], "us-east-1");
    EXPECT_EQ(captured.payload["provider"], "openai");
    EXPECT_EQ(captured.payload["model"], "gpt-4");
    EXPECT_DOUBLE_EQ(captured.payload["latency_ms"].get<double>(), 42.5);
    EXPECT_EQ(captured.payload["outcome"], "success");

    bus.shutdown();
}

TEST(RouterOutcomePublisher, NullBusDoesNotCrash) {
    RouterOutcomePublisher pub;
    pub.publish("req-001", "tenant-a", "01VER_NEW", "us-east-1",
                "openai", "gpt-4", 10.0, "success");
}

TEST(RouterOutcomePublisher, MultiplePublishesDelivered) {
    FeedbackBusConfig cfg;
    cfg.enabled = true;
    cfg.max_queue_size = 100;
    FeedbackBus bus(cfg);

    std::atomic<int> count{0};
    bus.subscribe([&](const FeedbackEvent&) { count.fetch_add(1); },
                   "router.outcome");
    bus.start();

    RouterOutcomePublisher pub(bus);
    for (int i = 0; i < 5; ++i) {
        pub.publish("req-" + std::to_string(i), "tenant-x", "v1", "us",
                    "anthropic", "claude-3", 10.0 * i, "success");
    }

    for (int i = 0; i < 100 && count.load() < 5; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_EQ(count.load(), 5);
    bus.shutdown();
}

TEST(RouterOutcomePublisher, ErrorOutcomeDelivered) {
    FeedbackBusConfig cfg;
    cfg.enabled = true;
    cfg.max_queue_size = 100;
    FeedbackBus bus(cfg);

    std::atomic<int> count{0};
    FeedbackEvent captured;
    bus.subscribe([&](const FeedbackEvent& ev) {
                       captured = ev;
                       count.fetch_add(1);
                   }, "router.outcome");
    bus.start();

    RouterOutcomePublisher pub(bus);
    pub.publish("req-err", "tenant-b", "01VER_X", "eu",
                "google", "gemini-pro", 500.0, "error");

    for (int i = 0; i < 50 && count.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    ASSERT_EQ(count.load(), 1);
    EXPECT_EQ(captured.payload["outcome"], "error");
    EXPECT_DOUBLE_EQ(captured.payload["latency_ms"].get<double>(), 500.0);

    bus.shutdown();
}

}  // namespace
}  // namespace aegisgate
