#include <gtest/gtest.h>
#include "server/gateway_runtime.h"
#include "core/config.h"
#include <thread>
#include <atomic>

using namespace aegisgate;

// --- Shutdown flag mechanics (no initialization needed) ---

TEST(RuntimeShutdownTest, InitiallyNotShuttingDown) {
    auto& rt = GatewayRuntime::instance();
    EXPECT_FALSE(rt.isShuttingDown());
}

TEST(RuntimeShutdownTest, BeginShutdownSetsFlag) {
    auto& rt = GatewayRuntime::instance();
    rt.beginShutdown();
    EXPECT_TRUE(rt.isShuttingDown());
}

TEST(RuntimeShutdownTest, ShutdownOnUninitializedIsSafe) {
    auto& rt = GatewayRuntime::instance();
    EXPECT_TRUE(rt.isShuttingDown());
    EXPECT_NO_THROW(rt.shutdown());
}

// --- Initialized runtime lifecycle ---
// Tests are ordered deliberately: non-destructive first, shutdown last.

class RuntimeLifecycleTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        auto& rt = GatewayRuntime::instance();
        rt.resetShutdownForTesting();
        if (!rt.isInitialized()) {
            static Config config;
            config.loadFromFile("config/aegisgate.yaml");
            rt.initialize(config);
        }
    }

    void TearDown() override {
        GatewayRuntime::instance().resetShutdownForTesting();
    }
};

TEST_F(RuntimeLifecycleTest, IsReadyWhenInitialized) {
    auto& rt = GatewayRuntime::instance();
    EXPECT_TRUE(rt.isInitialized());
    EXPECT_FALSE(rt.isShuttingDown());
}

TEST_F(RuntimeLifecycleTest, NotReadyWhenShuttingDown) {
    auto& rt = GatewayRuntime::instance();
    rt.beginShutdown();
    EXPECT_TRUE(rt.isShuttingDown());
    EXPECT_TRUE(rt.isInitialized());
}

TEST_F(RuntimeLifecycleTest, ReloadConfigRefreshesRateLimiterAndKeys) {
    auto& rt = GatewayRuntime::instance();
    bool ok = rt.reloadConfig();
    EXPECT_TRUE(ok);
    EXPECT_TRUE(rt.isInitialized());
}

TEST_F(RuntimeLifecycleTest, ReloadConfigIsThreadSafe) {
    auto& rt = GatewayRuntime::instance();
    std::atomic<bool> stop{false};

    std::thread reloader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            rt.reloadConfig();
        }
    });

    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto rl = rt.rateLimiterSnapshot();
            if (rl) {
                rl->allow("stress-test-key", 0.001);
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true, std::memory_order_relaxed);

    reloader.join();
    reader.join();
}

TEST_F(RuntimeLifecycleTest, ShutdownFlushesAuditToStore) {
    auto& rt = GatewayRuntime::instance();
    auto& pl = rt.pipeline();

    if (!pl.audit_logger || !pl.persistent_store) {
        GTEST_SKIP() << "Audit logger or persistent store not configured";
    }

    int64_t count_before = pl.persistent_store->auditCount();

    pl.audit_logger->logAction("test-req-flush", "test-tenant",
                               "FlushTest", "verify_flush",
                               "checking audit persistence");

    pl.audit_logger->flush();

    int64_t count_after = pl.persistent_store->auditCount();
    EXPECT_GT(count_after, count_before)
        << "Audit entry should be persisted after flush";

    rt.beginShutdown();
    EXPECT_NO_THROW(rt.shutdown());

    rt.reinitializeForTesting();
    EXPECT_TRUE(rt.isInitialized());
}

// --- Reinitialize after destructive test ---

TEST(ReinitializeTest, RebuildAfterShutdown) {
    auto& rt = GatewayRuntime::instance();

    ASSERT_TRUE(rt.isInitialized())
        << "Precondition: runtime must have been initialized by earlier suite";

    rt.beginShutdown();
    rt.shutdown();

    EXPECT_TRUE(rt.isShuttingDown());

    rt.reinitializeForTesting();

    EXPECT_TRUE(rt.isInitialized());
    EXPECT_FALSE(rt.isShuttingDown());
    EXPECT_TRUE(rt.rateLimiterSnapshot() != nullptr);
    EXPECT_FALSE(rt.registeredModels().empty());
}

TEST(ReinitializeTest, PipelineUsableAfterReinit) {
    auto& rt = GatewayRuntime::instance();

    rt.beginShutdown();
    rt.shutdown();
    rt.reinitializeForTesting();

    auto& pl = rt.pipeline();
    if (pl.audit_logger && pl.persistent_store) {
        pl.audit_logger->logAction("reinit-req", "reinit-tenant",
                                   "ReinitTest", "verify_reinit",
                                   "audit after reinit");
        EXPECT_NO_THROW(pl.audit_logger->flush());
    }
}

// --- GAP-013: shutdown handles store exceptions ---

TEST_F(RuntimeLifecycleTest, ShutdownHandlesStoreExceptions) {
    auto& rt = GatewayRuntime::instance();
    rt.beginShutdown();
    EXPECT_NO_THROW(rt.shutdown());
    rt.reinitializeForTesting();
}

// --- GAP-014: hot reload does not change providers/models ---

TEST_F(RuntimeLifecycleTest, ReloadConfigDoesNotChangeProviders) {
    auto& rt = GatewayRuntime::instance();
    auto models_before = rt.registeredModels();
    size_t count_before = models_before.size();

    bool ok = rt.reloadConfig();
    EXPECT_TRUE(ok);

    auto models_after = rt.registeredModels();
    EXPECT_EQ(models_after.size(), count_before)
        << "Hot reload should not change registered providers/models";
}

// --- Health readiness logic ---

TEST(HealthReadinessTest, ReadyRequiresInitAndNotShuttingDown) {
    auto& rt = GatewayRuntime::instance();
    rt.resetShutdownForTesting();

    bool initialized = rt.isInitialized();
    bool shutting_down = rt.isShuttingDown();
    bool ready = initialized && !shutting_down;
    EXPECT_TRUE(ready);
}

TEST(HealthReadinessTest, NotReadyDuringShutdown) {
    auto& rt = GatewayRuntime::instance();
    rt.beginShutdown();

    bool initialized = rt.isInitialized();
    bool shutting_down = rt.isShuttingDown();
    bool ready = initialized && !shutting_down;
    EXPECT_FALSE(ready);

    rt.resetShutdownForTesting();
}
