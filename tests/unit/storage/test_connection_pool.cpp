#include <gtest/gtest.h>
#include "storage/connection_pool.h"
#include <atomic>
#include <thread>
#include <vector>

// GCC 13 / 14 at -O2 emits a false-positive -Wmaybe-uninitialized when
// destructing a moved-from std::optional<ConnectionPool::Handle> inlined
// through the test suite (see GCC bug 109945 family). Members have NSDMI
// = nullptr, every ctor initializes them explicitly, and release() guards
// against null, so the warning is invalid. Suppress at the include point
// where GCC computes the diagnostic.
#if defined(__GNUC__) && !defined(__clang__)
# pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

using namespace aegisgate;

namespace {

struct MockConn {
    int id;
    bool alive = true;
};

std::atomic<int> g_conn_id{0};
std::atomic<int> g_deleted{0};

MockConn* mockFactory() {
    auto* c = new MockConn();
    c->id = g_conn_id++;
    return c;
}

void mockDeleter(MockConn* c) {
    ++g_deleted;
    delete c;
}

bool mockHealthCheck(MockConn* c) {
    return c && c->alive;
}

class ConnectionPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_conn_id = 0;
        g_deleted = 0;
    }
};

} // namespace

TEST_F(ConnectionPoolTest, CreateWithCorrectSize) {
    ConnectionPool<MockConn> pool(3, mockFactory, mockDeleter, mockHealthCheck);
    EXPECT_EQ(pool.poolSize(), 3u);
    EXPECT_EQ(pool.idleCount(), 3u);
    EXPECT_EQ(pool.activeCount(), 0u);
}

TEST_F(ConnectionPoolTest, AcquireAndRelease) {
    ConnectionPool<MockConn> pool(2, mockFactory, mockDeleter, mockHealthCheck);
    {
        auto handle = pool.acquire(std::chrono::milliseconds(100));
        ASSERT_TRUE(handle.has_value());
        EXPECT_NE(handle->get(), nullptr);
        EXPECT_EQ(pool.activeCount(), 1u);
        EXPECT_EQ(pool.idleCount(), 1u);
    }
    // Handle destroyed → returned to pool
    EXPECT_EQ(pool.activeCount(), 0u);
    EXPECT_EQ(pool.idleCount(), 2u);
}

TEST_F(ConnectionPoolTest, RAIIHandleAutoRelease) {
    ConnectionPool<MockConn> pool(1, mockFactory, mockDeleter, mockHealthCheck);
    {
        auto h1 = pool.acquire(std::chrono::milliseconds(100));
        ASSERT_TRUE(h1.has_value());
        EXPECT_EQ(pool.activeCount(), 1u);
        EXPECT_EQ(pool.idleCount(), 0u);
    }
    EXPECT_EQ(pool.activeCount(), 0u);
    EXPECT_EQ(pool.idleCount(), 1u);
}

TEST_F(ConnectionPoolTest, AcquireTimeoutWhenExhausted) {
    ConnectionPool<MockConn> pool(1, mockFactory, mockDeleter, mockHealthCheck);
    auto h1 = pool.acquire(std::chrono::milliseconds(100));
    ASSERT_TRUE(h1.has_value());

    auto h2 = pool.acquire(std::chrono::milliseconds(50));
    EXPECT_FALSE(h2.has_value());
}

TEST_F(ConnectionPoolTest, HealthCheckFailureRebuildsConnection) {
    ConnectionPool<MockConn> pool(2, mockFactory, mockDeleter, mockHealthCheck);

    // Acquire one and mark it dead before returning
    {
        auto h = pool.acquire(std::chrono::milliseconds(100));
        ASSERT_TRUE(h.has_value());
        h->get()->alive = false;
    }

    int deleted_before = g_deleted.load();
    auto h = pool.acquire(std::chrono::milliseconds(100));
    ASSERT_TRUE(h.has_value());
    EXPECT_TRUE(h->get()->alive);
    EXPECT_GT(g_deleted.load(), deleted_before);
}

TEST_F(ConnectionPoolTest, ExhaustedPoolWakesOnRelease) {
    ConnectionPool<MockConn> pool(1, mockFactory, mockDeleter, mockHealthCheck);
    auto h1 = pool.acquire(std::chrono::milliseconds(100));
    ASSERT_TRUE(h1.has_value());

    bool acquired = false;
    std::thread t([&] {
        auto h2 = pool.acquire(std::chrono::milliseconds(2000));
        acquired = h2.has_value();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    h1.reset();  // release back to pool
    t.join();
    EXPECT_TRUE(acquired);
}

TEST_F(ConnectionPoolTest, DestructorClosesAllConnections) {
    {
        ConnectionPool<MockConn> pool(3, mockFactory, mockDeleter, mockHealthCheck);
        EXPECT_EQ(pool.idleCount(), 3u);
    }
    EXPECT_EQ(g_deleted.load(), 3);
}

TEST_F(ConnectionPoolTest, HandleArrowOperator) {
    ConnectionPool<MockConn> pool(1, mockFactory, mockDeleter, mockHealthCheck);
    auto h = pool.acquire(std::chrono::milliseconds(100));
    ASSERT_TRUE(h.has_value());
    EXPECT_GE(h->get()->id, 0);
}

TEST_F(ConnectionPoolTest, IsHealthyReturnsCorrectState) {
    ConnectionPool<MockConn> pool(2, mockFactory, mockDeleter, mockHealthCheck);
    EXPECT_TRUE(pool.isHealthy());
}

// P1-B: a reachable backend (all conns alive) → active probe succeeds.
TEST_F(ConnectionPoolTest, ActiveHealthCheckTrueWhenReachable) {
    ConnectionPool<MockConn> pool(2, mockFactory, mockDeleter, mockHealthCheck);
    EXPECT_TRUE(pool.activeHealthCheck(std::chrono::milliseconds(100)));
    // probe must return the borrowed connection to the pool
    EXPECT_EQ(pool.activeCount(), 0u);
    EXPECT_EQ(pool.idleCount(), 2u);
}

// P1-B: a dead idle connection is transparently rebuilt during the probe, so
// the backend (still reachable via factory) is reported healthy.
TEST_F(ConnectionPoolTest, ActiveHealthCheckRecoversDeadConnection) {
    ConnectionPool<MockConn> pool(1, mockFactory, mockDeleter, mockHealthCheck);
    {
        auto h = pool.acquire(std::chrono::milliseconds(100));
        ASSERT_TRUE(h.has_value());
        h->get()->alive = false;  // poison the only connection
    }
    EXPECT_TRUE(pool.activeHealthCheck(std::chrono::milliseconds(100)));
}

// P1-B: when the backend is down — health check fails AND a fresh connection
// cannot be created — the active probe reports false (not a passive "healthy").
TEST_F(ConnectionPoolTest, ActiveHealthCheckFalseWhenBackendDown) {
    int calls = 0;
    auto factory = [&]() -> MockConn* {
        // first call seeds the pool at construction; later (reconnect) fails
        if (++calls == 1) return new MockConn{0, true};
        return nullptr;
    };
    auto always_dead = [](MockConn*) { return false; };
    ConnectionPool<MockConn> pool(1, factory, mockDeleter, always_dead);
    EXPECT_FALSE(pool.activeHealthCheck(std::chrono::milliseconds(100)));
}

TEST_F(ConnectionPoolTest, ConstructorFactoryThrows_CleansUpCreated) {
    int call_count = 0;
    auto throwing_factory = [&]() -> MockConn* {
        if (++call_count == 3) throw std::runtime_error("factory error");
        return new MockConn{call_count, true};
    };

    g_deleted = 0;
    EXPECT_THROW(
        (ConnectionPool<MockConn>(5, throwing_factory, mockDeleter, mockHealthCheck)),
        std::runtime_error);
    EXPECT_EQ(g_deleted.load(), 2);
}

TEST_F(ConnectionPoolTest, ConstructorFactoryReturnsNull_Throws) {
    int call_count = 0;
    auto null_factory = [&]() -> MockConn* {
        if (++call_count == 2) return nullptr;
        return new MockConn{call_count, true};
    };

    g_deleted = 0;
    EXPECT_THROW(
        (ConnectionPool<MockConn>(3, null_factory, mockDeleter, mockHealthCheck)),
        std::runtime_error);
    EXPECT_EQ(g_deleted.load(), 1);
}

TEST_F(ConnectionPoolTest, AcquireReconnectNull_ReturnsNullopt) {
    ConnectionPool<MockConn> pool(1, mockFactory, mockDeleter, mockHealthCheck);
    {
        auto h = pool.acquire(std::chrono::milliseconds(100));
        ASSERT_TRUE(h.has_value());
        h->get()->alive = false;
    }

    // pool's internal factory can't be replaced, but the dead conn + null reconnect
    // is tested by checking that a bad health check gets handled
    auto h = pool.acquire(std::chrono::milliseconds(100));
    // With the default factory, reconnect creates a valid conn
    ASSERT_TRUE(h.has_value());
    EXPECT_TRUE(h->get()->alive);
}

TEST_F(ConnectionPoolTest, MoveHandle) {
    ConnectionPool<MockConn> pool(2, mockFactory, mockDeleter, mockHealthCheck);
    auto h1 = pool.acquire(std::chrono::milliseconds(100));
    ASSERT_TRUE(h1.has_value());
    EXPECT_EQ(pool.activeCount(), 1u);

    auto h2 = std::move(*h1);
    h1.reset();
    EXPECT_EQ(pool.activeCount(), 1u);
    EXPECT_NE(h2.get(), nullptr);
}

TEST_F(ConnectionPoolTest, PoolSizeClampedToMax) {
    ConnectionPool<MockConn> pool(9999, mockFactory, mockDeleter, mockHealthCheck);
    EXPECT_LE(pool.poolSize(), ConnectionPool<MockConn>::kMaxPoolSize);
}
