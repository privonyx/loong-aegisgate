#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include "guardrail/audit.h"
#include "auth/encryption.h"
#include "storage/memory_persistent_store.h"

using namespace aegisgate;

class AuditLoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        logger_.clear();
    }
    void TearDown() override {
        logger_.shutdown();
    }
    AuditLogger logger_;
};

// P0-D (TASK-20260701-01): the /admin/logs/stream SSE handler unsubscribes
// from inside its delivery callback (on a failed send to a disconnected
// client). log() previously held subscribers_mutex_ across the callback loop,
// so a subscriber that calls unsubscribe() during delivery re-entered the same
// non-recursive mutex (deadlock) AND mutated the map being range-iterated (UB).
// The whole audit log path would hang the moment a streaming admin client
// disconnected. Delivery must occur without holding the lock.
TEST_F(AuditLoggerTest, SelfUnsubscribeDuringDeliveryDoesNotDeadlock) {
    size_t id = 0;
    std::atomic<int> deliveries{0};
    id = logger_.subscribe([&](const AuditEntry&) {
        ++deliveries;
        logger_.unsubscribe(id);  // re-enters subscribers_mutex_
    });

    std::atomic<bool> done{false};
    std::thread t([&] {
        logger_.logAction("req-d", "t", "stage", "act", "detail");
        done = true;
    });

    for (int i = 0; i < 300 && !done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bool finished = done.load();
    if (finished) {
        t.join();
    } else {
        t.detach();  // deadlocked; leak the thread so the suite can continue
    }

    ASSERT_TRUE(finished) << "log() deadlocked on self-unsubscribe";
    EXPECT_EQ(deliveries.load(), 1);

    // The self-unsubscribing subscriber must be gone: a second log() delivers
    // to nobody (delivery count stays at 1).
    logger_.logAction("req-d2", "t", "stage", "act", "detail");
    EXPECT_EQ(deliveries.load(), 1);
}

TEST_F(AuditLoggerTest, LogsAction) {
    logger_.logAction("req-001", "tenant-1", "injection", "blocked", "injection detected");
    ASSERT_EQ(logger_.entries().size(), 1u);
    EXPECT_EQ(logger_.entries()[0].request_id, "req-001");
    EXPECT_EQ(logger_.entries()[0].tenant_id, "tenant-1");
    EXPECT_EQ(logger_.entries()[0].stage_name, "injection");
    EXPECT_EQ(logger_.entries()[0].action, "blocked");
}

TEST_F(AuditLoggerTest, TimestampIsISO8601) {
    logger_.logAction("req-002", "tenant-1", "pii", "masked");
    auto ts = logger_.entries()[0].timestamp;
    EXPECT_NE(ts.find("T"), std::string::npos);
    EXPECT_NE(ts.find("Z"), std::string::npos);
}

TEST_F(AuditLoggerTest, MultipleLogs) {
    logger_.logAction("req-003", "t1", "stage1", "action1");
    logger_.logAction("req-004", "t2", "stage2", "action2");
    logger_.logAction("req-005", "t3", "stage3", "action3");
    EXPECT_EQ(logger_.entries().size(), 3u);
}

TEST_F(AuditLoggerTest, ClearRemovesAll) {
    logger_.logAction("req-006", "t1", "s1", "a1");
    EXPECT_EQ(logger_.entries().size(), 1u);
    logger_.clear();
    EXPECT_EQ(logger_.entries().size(), 0u);
}

TEST_F(AuditLoggerTest, SinkReceivesEntries) {
    std::vector<AuditEntry> received;
    logger_.setSink([&received](const AuditEntry& e) {
        received.push_back(e);
    });
    logger_.logAction("req-007", "t1", "test", "sink_test");
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].action, "sink_test");
}

TEST_F(AuditLoggerTest, PipelineLogsRequest) {
    RequestContext ctx;
    ctx.request_id = "req-008";
    ctx.tenant_id = "tenant-x";
    ctx.chat_request.messages = {{"user", "Hello world"}};

    EXPECT_EQ(logger_.process(ctx), StageResult::Continue);
    ASSERT_EQ(logger_.entries().size(), 1u);
    EXPECT_EQ(logger_.entries()[0].request_id, "req-008");
    EXPECT_EQ(logger_.entries()[0].action, "request_received");
    EXPECT_FALSE(logger_.entries()[0].input_hash.empty());
}

TEST_F(AuditLoggerTest, HashIsDeterministic) {
    RequestContext ctx1, ctx2;
    ctx1.request_id = "r1"; ctx1.chat_request.messages = {{"user", "same"}};
    ctx2.request_id = "r2"; ctx2.chat_request.messages = {{"user", "same"}};

    logger_.process(ctx1);
    logger_.process(ctx2);
    EXPECT_EQ(logger_.entries()[0].input_hash, logger_.entries()[1].input_hash);
}

TEST_F(AuditLoggerTest, PersistentStoreReceivesEntriesAfterFlush) {
    MemoryPersistentStore store;
    store.initialize();
    logger_.setPersistentStore(&store);

    logger_.logAction("req-persist", "t1", "test", "persist_test");

    EXPECT_EQ(logger_.entries().size(), 1u);

    logger_.flush();
    EXPECT_EQ(store.auditCount(), 1);
    auto results = store.queryAudits();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].request_id, "req-persist");
    EXPECT_EQ(results[0].action, "persist_test");
}

TEST_F(AuditLoggerTest, NullStoreDoesNotCrash) {
    logger_.setPersistentStore(nullptr);
    logger_.logAction("req-null", "t1", "test", "null_test");
    EXPECT_EQ(logger_.entries().size(), 1u);
}

TEST_F(AuditLoggerTest, PipelineWithPersistentStore) {
    MemoryPersistentStore store;
    store.initialize();
    logger_.setPersistentStore(&store);

    RequestContext ctx;
    ctx.request_id = "req-pipe";
    ctx.tenant_id = "tenant-pipe";
    ctx.chat_request.messages = {{"user", "Hello"}};

    logger_.process(ctx);
    EXPECT_EQ(logger_.entries().size(), 1u);

    logger_.flush();
    EXPECT_EQ(store.auditCount(), 1);
}

TEST_F(AuditLoggerTest, ShutdownFlushesRemaining) {
    MemoryPersistentStore store;
    store.initialize();
    logger_.setPersistentStore(&store);

    for (int i = 0; i < 50; ++i) {
        logger_.logAction("req-" + std::to_string(i), "t1", "test", "action");
    }

    logger_.shutdown();
    EXPECT_EQ(store.auditCount(), 50);
}

TEST_F(AuditLoggerTest, AsyncBatchPersistence) {
    MemoryPersistentStore store;
    store.initialize();
    logger_.setPersistentStore(&store);

    logger_.logAction("req-a1", "t1", "test", "action1");
    logger_.logAction("req-a2", "t1", "test", "action2");
    logger_.logAction("req-a3", "t1", "test", "action3");

    EXPECT_EQ(logger_.entries().size(), 3u);

    logger_.flush();
    EXPECT_EQ(store.auditCount(), 3);
}

TEST_F(AuditLoggerTest, NoPersistenceWithoutStore) {
    for (int i = 0; i < 100; ++i) {
        logger_.logAction("req-" + std::to_string(i), "t1", "test", "action");
    }
    EXPECT_EQ(logger_.entries().size(), 100u);
}

TEST_F(AuditLoggerTest, FlushIsExactNotPolling) {
    for (int round = 0; round < 10; ++round) {
        AuditLogger logger;
        MemoryPersistentStore store;
        store.initialize();
        logger.setPersistentStore(&store);

        for (int i = 0; i < 100; ++i) {
            logger.logAction("req-" + std::to_string(i), "t1", "test", "action");
        }

        logger.flush();
        EXPECT_EQ(store.auditCount(), 100)
            << "Failed on round " << round;
        logger.shutdown();
    }
}

TEST_F(AuditLoggerTest, ConcurrentFlushAllComplete) {
    MemoryPersistentStore store;
    store.initialize();
    logger_.setPersistentStore(&store);

    for (int i = 0; i < 20; ++i) {
        logger_.logAction("req-" + std::to_string(i), "t1", "test", "action");
    }

    std::vector<std::thread> flushers;
    for (int i = 0; i < 4; ++i) {
        flushers.emplace_back([this]() { logger_.flush(); });
    }
    for (auto& t : flushers) t.join();

    EXPECT_EQ(store.auditCount(), 20);
}

TEST_F(AuditLoggerTest, FlushWithoutStoreIsNoop) {
    logger_.logAction("req-noop", "t1", "test", "action");
    logger_.flush();
    EXPECT_EQ(logger_.entries().size(), 1u);
}

// --- subscribe/unsubscribe tests (logs tail core path) ---

TEST_F(AuditLoggerTest, SubscribeReceivesEntries) {
    std::vector<AuditEntry> received;
    auto id = logger_.subscribe([&](const AuditEntry& e) {
        received.push_back(e);
    });

    logger_.logAction("req-sub1", "t1", "test", "sub_action");
    logger_.logAction("req-sub2", "t1", "test", "sub_action2");

    ASSERT_EQ(received.size(), 2u);
    EXPECT_EQ(received[0].request_id, "req-sub1");
    EXPECT_EQ(received[1].request_id, "req-sub2");

    logger_.unsubscribe(id);
}

TEST_F(AuditLoggerTest, MultipleSubscribersAllReceive) {
    std::vector<std::string> received_a, received_b;
    auto id_a = logger_.subscribe([&](const AuditEntry& e) {
        received_a.push_back(e.request_id);
    });
    auto id_b = logger_.subscribe([&](const AuditEntry& e) {
        received_b.push_back(e.request_id);
    });

    logger_.logAction("req-multi", "t1", "test", "multi_action");

    EXPECT_EQ(received_a.size(), 1u);
    EXPECT_EQ(received_b.size(), 1u);
    EXPECT_EQ(received_a[0], "req-multi");
    EXPECT_EQ(received_b[0], "req-multi");

    logger_.unsubscribe(id_a);
    logger_.unsubscribe(id_b);
}

TEST_F(AuditLoggerTest, UnsubscribeStopsDelivery) {
    std::vector<std::string> received;
    auto id = logger_.subscribe([&](const AuditEntry& e) {
        received.push_back(e.request_id);
    });

    logger_.logAction("req-before", "t1", "test", "action");
    ASSERT_EQ(received.size(), 1u);

    logger_.unsubscribe(id);

    logger_.logAction("req-after", "t1", "test", "action");
    EXPECT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0], "req-before");
}

TEST_F(AuditLoggerTest, SubscriberExceptionDoesNotCrash) {
    auto id_throw = logger_.subscribe([](const AuditEntry&) {
        throw std::runtime_error("subscriber error");
    });

    std::vector<std::string> received;
    auto id_ok = logger_.subscribe([&](const AuditEntry& e) {
        received.push_back(e.request_id);
    });

    logger_.logAction("req-exc", "t1", "test", "action");

    EXPECT_EQ(logger_.entries().size(), 1u);
    EXPECT_GE(received.size(), 0u);

    logger_.unsubscribe(id_throw);
    logger_.unsubscribe(id_ok);
}

TEST_F(AuditLoggerTest, FlushReturnsOnTimeout) {
    MemoryPersistentStore store;
    logger_.setPersistentStore(&store);

    logger_.logAction("req-timeout", "t1", "test", "timeout_test");
    bool ok = logger_.flush(std::chrono::seconds{5});
    EXPECT_TRUE(ok);

    bool fast = logger_.flush(std::chrono::seconds{1});
    EXPECT_TRUE(fast);
}

TEST_F(AuditLoggerTest, FlushWithoutStoreReturnsTrue) {
    bool ok = logger_.flush(std::chrono::seconds{1});
    EXPECT_TRUE(ok);
}

// --- Audit chain hash tests ---

TEST_F(AuditLoggerTest, ChainHashIsPopulated) {
    logger_.logAction("req-chain-1", "t1", "test", "action1");
    auto entries = logger_.entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_FALSE(entries[0].chain_hash.empty());
    EXPECT_EQ(entries[0].chain_hash.size(), 64u);
}

TEST_F(AuditLoggerTest, ChainHashFormsDeterministicChain) {
    logger_.logAction("req-c1", "t1", "test", "a1");
    logger_.logAction("req-c2", "t1", "test", "a2");
    logger_.logAction("req-c3", "t1", "test", "a3");

    auto entries = logger_.entries();
    ASSERT_EQ(entries.size(), 3u);

    EXPECT_NE(entries[0].chain_hash, entries[1].chain_hash);
    EXPECT_NE(entries[1].chain_hash, entries[2].chain_hash);
    EXPECT_NE(entries[0].chain_hash, entries[2].chain_hash);
}

TEST_F(AuditLoggerTest, VerifyChainSucceeds) {
    logger_.logAction("req-v1", "t1", "test", "a1");
    logger_.logAction("req-v2", "t1", "test", "a2");
    logger_.logAction("req-v3", "t1", "test", "a3");

    EXPECT_TRUE(logger_.verifyChain());
}

TEST_F(AuditLoggerTest, TamperedEntryFailsVerification) {
    logger_.logAction("req-t1", "t1", "test", "a1");
    logger_.logAction("req-t2", "t1", "test", "a2");
    logger_.logAction("req-t3", "t1", "test", "a3");

    auto entries = logger_.entries();
    ASSERT_EQ(entries.size(), 3u);

    entries[1].action = "tampered_action";
    EXPECT_FALSE(logger_.verifyChain(entries));
}

TEST_F(AuditLoggerTest, ChainHashDependsOnPrevious) {
    logger_.logAction("req-d1", "t1", "test", "a1");
    auto first_entries = logger_.entries();

    AuditLogger logger2;
    logger2.logAction("req-d1", "t1", "test", "a1");
    auto second_entries = logger2.entries();
    logger2.shutdown();

    EXPECT_EQ(first_entries[0].chain_hash, second_entries[0].chain_hash);
}

TEST_F(AuditLoggerTest, EmptyChainVerifies) {
    EXPECT_TRUE(logger_.verifyChain());
}

TEST_F(AuditLoggerTest, ChainHashPersistedToStore) {
    MemoryPersistentStore store;
    store.initialize();
    logger_.setPersistentStore(&store);

    logger_.logAction("req-persist-chain", "t1", "test", "action");
    logger_.flush();

    auto results = store.queryAudits();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].chain_hash.empty());
    EXPECT_EQ(results[0].chain_hash.size(), 64u);
}

TEST_F(AuditLoggerTest, ChainHashSentToSubscribers) {
    std::string received_hash;
    auto id = logger_.subscribe([&](const AuditEntry& e) {
        received_hash = e.chain_hash;
    });

    logger_.logAction("req-sub-chain", "t1", "test", "action");

    EXPECT_FALSE(received_hash.empty());
    auto entries = logger_.entries();
    EXPECT_EQ(received_hash, entries[0].chain_hash);

    logger_.unsubscribe(id);
}

TEST_F(AuditLoggerTest, ConcurrentSubscribeUnsubscribe) {
    std::atomic<int> total_received{0};
    constexpr int kThreads = 4;
    constexpr int kLogsPerThread = 50;

    std::vector<size_t> sub_ids(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        sub_ids[i] = logger_.subscribe([&](const AuditEntry&) {
            total_received.fetch_add(1, std::memory_order_relaxed);
        });
    }

    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([this, t]() {
            for (int i = 0; i < kLogsPerThread; ++i) {
                logger_.logAction(
                    "req-" + std::to_string(t) + "-" + std::to_string(i),
                    "t1", "test", "concurrent");
            }
        });
    }
    for (auto& w : writers) w.join();

    for (int i = 0; i < kThreads; ++i) {
        logger_.unsubscribe(sub_ids[i]);
    }

    int total_logs = kThreads * kLogsPerThread;
    EXPECT_EQ(static_cast<int>(logger_.entries().size()), total_logs);
    EXPECT_EQ(total_received.load(), total_logs * kThreads);
}

// --- Audit encryption tests ---

TEST_F(AuditLoggerTest, SetEncryptionWithNullDoesNotCrash) {
    logger_.setEncryption(nullptr);
    logger_.logAction("req-enc-null", "t1", "test", "action", "detail");
    EXPECT_EQ(logger_.entries().size(), 1u);
}

TEST_F(AuditLoggerTest, SetEncryptionWithUnavailableDoesNotEncrypt) {
    Encryption enc;
    EXPECT_FALSE(enc.isAvailable());
    logger_.setEncryption(&enc);

    MemoryPersistentStore store;
    store.initialize();
    logger_.setPersistentStore(&store);

    logger_.logAction("req-enc-unavail", "t1", "test", "action", "plain detail");
    logger_.flush();

    auto results = store.queryAudits();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].detail, "plain detail");
}

TEST_F(AuditLoggerTest, InMemoryEntriesNotEncrypted) {
    logger_.logAction("req-inmem", "t1", "test", "action", "sensitive data");
    auto entries = logger_.entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].detail, "sensitive data");
}

// --- TASK-20260702-01 P1-3：审计 detail 加密后读路径解密回读 ---

static constexpr const char* kTestEncKey =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

TEST_F(AuditLoggerTest, DecryptDetailRoundTripWhenEncrypted) {
    setenv("AEGISGATE_ENCRYPTION_KEY", kTestEncKey, 1);
    Encryption enc;
    ASSERT_TRUE(enc.isAvailable());
    logger_.setEncryption(&enc);

    MemoryPersistentStore store;
    store.initialize();
    logger_.setPersistentStore(&store);

    logger_.logAction("req-enc", "t1", "test", "action", "sensitive payload");
    logger_.flush();

    auto results = store.queryAudits();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_NE(results[0].detail, "sensitive payload") << "落库应为密文";
    EXPECT_EQ(AuditLogger::decryptDetail(results[0].detail, &enc), "sensitive payload")
        << "P1-3: 读路径须解密回明文";
    unsetenv("AEGISGATE_ENCRYPTION_KEY");
}

TEST_F(AuditLoggerTest, DecryptDetailPassthroughWhenUnavailable) {
    unsetenv("AEGISGATE_ENCRYPTION_KEY");
    Encryption enc;
    EXPECT_FALSE(enc.isAvailable());
    EXPECT_EQ(AuditLogger::decryptDetail("plain", &enc), "plain");
    EXPECT_EQ(AuditLogger::decryptDetail("plain", nullptr), "plain");
}

TEST_F(AuditLoggerTest, DecryptDetailPassthroughForPlaintextWithKey) {
    setenv("AEGISGATE_ENCRYPTION_KEY", kTestEncKey, 1);
    Encryption enc;
    ASSERT_TRUE(enc.isAvailable());
    // 明文不是有效密文 → 认证解密失败 → 原样返回（不误伤未加密历史数据）。
    EXPECT_EQ(AuditLogger::decryptDetail("not-ciphertext", &enc), "not-ciphertext");
    unsetenv("AEGISGATE_ENCRYPTION_KEY");
}
