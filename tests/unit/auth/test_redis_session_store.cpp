#include <gtest/gtest.h>
#include <cstdlib>

#ifdef AEGISGATE_ENABLE_REDIS
#include "storage/redis_cache_store.h"
#endif

#include "auth/redis_session_store.h"

using namespace aegisgate;

TEST(RedisSessionStoreTest, UnavailableWhenNoRedis) {
#ifdef AEGISGATE_ENABLE_REDIS
    RedisSessionStore store(nullptr);
#else
    RedisSessionStore store;
#endif
    EXPECT_FALSE(store.isAvailable());
}

TEST(RedisSessionStoreTest, InsertFailsWhenUnavailable) {
#ifdef AEGISGATE_ENABLE_REDIS
    RedisSessionStore store(nullptr);
#else
    RedisSessionStore store;
#endif
    Session s;
    s.id = "test-session";
    s.user_id = "user-1";
    EXPECT_FALSE(store.insertSession(s));
}

TEST(RedisSessionStoreTest, GetReturnsNulloptWhenUnavailable) {
#ifdef AEGISGATE_ENABLE_REDIS
    RedisSessionStore store(nullptr);
#else
    RedisSessionStore store;
#endif
    EXPECT_FALSE(store.getSession("nonexistent").has_value());
}

TEST(RedisSessionStoreTest, DeleteExpiredReturnsZero) {
#ifdef AEGISGATE_ENABLE_REDIS
    RedisSessionStore store(nullptr);
#else
    RedisSessionStore store;
#endif
    EXPECT_EQ(store.deleteExpiredSessions(), 0);
}

#ifdef AEGISGATE_ENABLE_REDIS

TEST(RedisSessionStoreTest, LiveRedisRoundTrip) {
    const char* url = std::getenv("AEGISGATE_REDIS_URL");
    if (!url) {
        GTEST_SKIP() << "AEGISGATE_REDIS_URL not set, skipping Redis integration";
    }

    std::string s(url);
    RedisConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 6379;
    if (s.find("redis://") == 0) {
        auto hp = s.substr(8);
        auto colon = hp.find(':');
        if (colon != std::string::npos) {
            cfg.host = hp.substr(0, colon);
            cfg.port = std::stoi(hp.substr(colon + 1));
        } else {
            cfg.host = hp;
        }
    }
    cfg.pool_size = 2;
    cfg.db = 15;

    RedisCacheStore redis(cfg);
    ASSERT_TRUE(redis.initialize());

    RedisSessionStore store(&redis, 3600);
    ASSERT_TRUE(store.isAvailable());

    Session sess;
    sess.id = "sess-live-test-1";
    sess.user_id = "user-live-1";
    sess.tenant_id = "t1";
    sess.auth_method = "password";
    sess.mfa_verified = false;
    sess.created_at = "c1";
    sess.last_active_at = "a1";
    sess.expires_at = "e1";

    ASSERT_TRUE(store.insertSession(sess));
    auto got = store.getSession(sess.id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id, sess.id);
    EXPECT_EQ(got->user_id, sess.user_id);

    EXPECT_TRUE(store.updateSessionActivity(sess.id, "a2"));
    got = store.getSession(sess.id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->last_active_at, "a2");

    EXPECT_TRUE(store.updateSessionMfaVerified(sess.id, true));
    got = store.getSession(sess.id);
    ASSERT_TRUE(got.has_value());
    EXPECT_TRUE(got->mfa_verified);

    EXPECT_EQ(store.countSessionsByUser(sess.user_id), 1);
    auto listed = store.listSessionsByUser(sess.user_id);
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0].id, sess.id);

    EXPECT_TRUE(store.deleteSession(sess.id));
    EXPECT_FALSE(store.getSession(sess.id).has_value());

    redis.close();
}

#else

TEST(RedisSessionStoreTest, LiveRedisSkippedWhenRedisNotCompiled) {
    GTEST_SKIP() << "ENABLE_REDIS is OFF; live Redis session tests not built";
}

#endif
