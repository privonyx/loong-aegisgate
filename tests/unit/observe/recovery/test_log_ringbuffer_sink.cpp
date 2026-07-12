// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 1 task 1.3.
//
// LogRingbufferSink tests (6).
//
// Coverage:
//   1. AddEntryAppendsToDeque
//   2. CapacityFifoEviction
//   3. PiiMaskingApplied         — M5 mutation target (SR-NEW2 key test)
//   4. ConcurrentAddSafe
//   5. SnapshotSinceTimestampReturnsRecent
//   6. DumpAllReturnsAll

#include "guardrail/inbound/pii_filter.h"
#include "observe/recovery/log_ringbuffer_sink.h"

#include <gtest/gtest.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/sink.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace aegisgate;

namespace {

std::shared_ptr<PIIFilter> makePiiFilter() {
    auto f = std::make_shared<PIIFilter>();
    // PIIFilter ctor already loads default patterns including email.
    return f;
}

std::shared_ptr<spdlog::logger> attachLogger(
    std::shared_ptr<LogRingbufferSink> sink,
    const std::string& name = "test_rb") {
    auto logger = std::make_shared<spdlog::logger>(name, sink);
    logger->set_level(spdlog::level::trace);
    return logger;
}

} // namespace

TEST(LogRingbufferSinkTest, AddEntryAppendsToDeque) {
    auto pii = makePiiFilter();
    auto sink = std::make_shared<LogRingbufferSink>(pii, /*capacity=*/100);
    auto log = attachLogger(sink);

    log->info("hello world");
    log->warn("danger");

    auto all = sink->dumpAll();
    ASSERT_EQ(all.size(), 2u);
    EXPECT_NE(all[0].msg_masked.find("hello"), std::string::npos);
    EXPECT_NE(all[1].msg_masked.find("danger"), std::string::npos);
}

TEST(LogRingbufferSinkTest, CapacityFifoEviction) {
    auto pii = makePiiFilter();
    auto sink = std::make_shared<LogRingbufferSink>(pii, /*capacity=*/3);
    auto log = attachLogger(sink);

    log->info("a");
    log->info("b");
    log->info("c");
    log->info("d");          // should evict "a"
    log->info("e");          // should evict "b"

    auto all = sink->dumpAll();
    ASSERT_EQ(all.size(), 3u);
    EXPECT_NE(all[0].msg_masked.find("c"), std::string::npos);
    EXPECT_NE(all[1].msg_masked.find("d"), std::string::npos);
    EXPECT_NE(all[2].msg_masked.find("e"), std::string::npos);
}

TEST(LogRingbufferSinkTest, PiiMaskingApplied) {
    auto pii = makePiiFilter();
    auto sink = std::make_shared<LogRingbufferSink>(pii, /*capacity=*/10);
    auto log = attachLogger(sink);

    log->info("user email: alice@example.com submitted form");

    auto all = sink->dumpAll();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].msg_masked.find("alice@example.com"), std::string::npos)
        << "Raw email must NOT survive into the ring buffer "
        << "(M5 mutation target, SR-NEW2 SR check)";
}

TEST(LogRingbufferSinkTest, ConcurrentAddSafe) {
    auto pii = makePiiFilter();
    auto sink = std::make_shared<LogRingbufferSink>(pii, /*capacity=*/10000);
    auto log = attachLogger(sink);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 200;

    std::atomic<int> ready{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            ready.fetch_add(1);
            while (ready.load() < kThreads) {}
            for (int i = 0; i < kPerThread; ++i) {
                log->info("t={} i={}", t, i);
            }
        });
    }
    for (auto& th : threads) th.join();

    auto all = sink->dumpAll();
    EXPECT_EQ(all.size(),
              static_cast<std::size_t>(kThreads * kPerThread));
}

TEST(LogRingbufferSinkTest, SnapshotSinceTimestampReturnsRecent) {
    auto pii = makePiiFilter();
    auto sink = std::make_shared<LogRingbufferSink>(pii, /*capacity=*/100);
    auto log = attachLogger(sink);

    log->info("first");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto cutoff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    log->info("second");
    log->info("third");

    auto recent = sink->snapshotSince(cutoff_ms);
    ASSERT_GE(recent.size(), 2u);
    EXPECT_NE(recent[0].msg_masked.find("second"), std::string::npos);
    EXPECT_NE(recent[1].msg_masked.find("third"), std::string::npos);
    for (const auto& e : recent) {
        EXPECT_EQ(e.msg_masked.find("first"), std::string::npos);
    }
}

TEST(LogRingbufferSinkTest, DumpAllReturnsAll) {
    auto pii = makePiiFilter();
    auto sink = std::make_shared<LogRingbufferSink>(pii, /*capacity=*/100);
    auto log = attachLogger(sink);

    for (int i = 0; i < 5; ++i) log->info("entry-{}", i);

    auto all = sink->dumpAll();
    EXPECT_EQ(all.size(), 5u);
}
