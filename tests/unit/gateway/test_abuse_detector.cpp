#include <gtest/gtest.h>
#include "gateway/abuse_detector.h"
#include <thread>
#include <atomic>
#include <vector>

using namespace aegisgate;

class AbuseDetectorTest : public ::testing::Test {
protected:
    AbuseDetector::Config makeConfig() {
        AbuseDetector::Config c;
        c.window_seconds = 5;
        c.warn_threshold = 3;
        c.throttle_threshold = 5;
        c.block_threshold = 8;
        c.block_duration_seconds = 2;
        c.throttle_factor = 0.5;
        return c;
    }

    AbuseDetector::Config config_ = makeConfig();
    AbuseDetector detector_{config_};
};

TEST_F(AbuseDetectorTest, InitiallyAllows) {
    EXPECT_EQ(detector_.getAction("key-1"), AbuseDetector::Action::Allow);
}

TEST_F(AbuseDetectorTest, WarnsAfterThreshold) {
    for (int i = 0; i < 3; ++i) detector_.recordRejection("key-1");
    EXPECT_EQ(detector_.getAction("key-1"), AbuseDetector::Action::Warn);
}

TEST_F(AbuseDetectorTest, ThrottlesAfterThreshold) {
    for (int i = 0; i < 5; ++i) detector_.recordRejection("key-1");
    EXPECT_EQ(detector_.getAction("key-1"), AbuseDetector::Action::Throttle);
}

TEST_F(AbuseDetectorTest, BlocksAfterThreshold) {
    for (int i = 0; i < 8; ++i) detector_.recordRejection("key-1");
    EXPECT_EQ(detector_.getAction("key-1"), AbuseDetector::Action::Block);
}

TEST_F(AbuseDetectorTest, DifferentKeysIndependent) {
    for (int i = 0; i < 8; ++i) detector_.recordRejection("key-1");
    EXPECT_EQ(detector_.getAction("key-2"), AbuseDetector::Action::Allow);
}

TEST_F(AbuseDetectorTest, WindowExpiration) {
    for (int i = 0; i < 5; ++i) detector_.recordRejection("key-exp");
    EXPECT_EQ(detector_.getAction("key-exp"), AbuseDetector::Action::Throttle);
    std::this_thread::sleep_for(std::chrono::seconds(6));
    EXPECT_EQ(detector_.getAction("key-exp"), AbuseDetector::Action::Allow);
}

TEST_F(AbuseDetectorTest, BlockAutoRecovery) {
    for (int i = 0; i < 8; ++i) detector_.recordRejection("key-block");
    EXPECT_EQ(detector_.getAction("key-block"), AbuseDetector::Action::Block);
    // block_duration=2s, window=5s. After 6s both block and window expire.
    std::this_thread::sleep_for(std::chrono::seconds(6));
    EXPECT_EQ(detector_.getAction("key-block"), AbuseDetector::Action::Allow);
}

TEST_F(AbuseDetectorTest, RejectionCountAccurate) {
    detector_.recordRejection("key-cnt");
    detector_.recordRejection("key-cnt");
    EXPECT_EQ(detector_.rejectionCount("key-cnt"), 2);
}

TEST_F(AbuseDetectorTest, ConcurrentRecordAndQuery) {
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&, i] {
            while (!stop.load()) {
                auto key = "key-" + std::to_string(i % 4);
                detector_.recordRejection(key);
                detector_.getAction(key);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true);
    for (auto& t : threads) t.join();
}

TEST_F(AbuseDetectorTest, ZeroRejectionsForUnknownKey) {
    EXPECT_EQ(detector_.rejectionCount("unknown"), 0);
}

TEST(AbuseDetectorLRUTest, EvictsOldestKeyWhenShardFull) {
    AbuseDetector::Config cfg;
    cfg.max_keys_per_shard = 4;
    cfg.window_seconds = 300;
    cfg.block_threshold = 100;
    AbuseDetector detector(cfg);

    for (int i = 0; i < 10; ++i) {
        detector.recordRejection("key-" + std::to_string(i));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Key-9 (latest) should still be tracked
    EXPECT_GT(detector.rejectionCount("key-9"), 0);
}

TEST_F(AbuseDetectorTest, SimilarContentRaisesAction) {
    const std::string prompt =
        "Please summarize the quarterly report for Q3 and list risks.";
    // First observe seeds fingerprint (no hit). Subsequent identical → hits.
    EXPECT_EQ(detector_.observe("sim-key", prompt), AbuseDetector::Action::Allow);
    EXPECT_EQ(detector_.observe("sim-key", prompt), AbuseDetector::Action::Allow);  // count=1
    EXPECT_EQ(detector_.observe("sim-key", prompt), AbuseDetector::Action::Allow);  // count=2
    EXPECT_EQ(detector_.observe("sim-key", prompt), AbuseDetector::Action::Warn);   // count=3
}

TEST_F(AbuseDetectorTest, DifferentKeysSimilarityIsolated) {
    const std::string prompt = "Please summarize the quarterly report for Q3.";
    for (int i = 0; i < 8; ++i) {
        detector_.observe("iso-a", prompt);
    }
    EXPECT_EQ(detector_.getAction("iso-b"), AbuseDetector::Action::Allow);
}

TEST_F(AbuseDetectorTest, EmptyContentNoSimilarityBump) {
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(detector_.observe("empty-key", ""), AbuseDetector::Action::Allow);
    }
}

TEST_F(AbuseDetectorTest, NearEditTriggersSimilarityHit) {
    AbuseDetector::Config cfg = makeConfig();
    cfg.warn_threshold = 1;  // first hit → Warn
    cfg.throttle_threshold = 5;
    cfg.block_threshold = 8;
    AbuseDetector d(cfg);

    const std::string a =
        "Please summarize the quarterly report for Q3 and highlight risks.";
    std::string b = a;
    b[10] = 'X';

    EXPECT_EQ(d.observe("near-key", a), AbuseDetector::Action::Allow);
    EXPECT_EQ(d.observe("near-key", b), AbuseDetector::Action::Warn);
}

TEST_F(AbuseDetectorTest, OversizedContentDoesNotCrash) {
    std::string huge(20000, 'z');
    EXPECT_NO_THROW({
        auto action = detector_.observe("huge-key", huge);
        (void)action;
    });
}
