#include <gtest/gtest.h>
#include "storage/redis_cache_store.h"

using namespace aegisgate;

// Unit tests for RedisCacheStore API surface — no real Redis required.
// Integration tests (with real Redis) only run when AEGISGATE_REDIS_URL is set.

TEST(RedisCacheStoreUnit, BackendName) {
    RedisConfig cfg;
    RedisCacheStore store(cfg);
    EXPECT_EQ(store.backendName(), "redis");
}

TEST(RedisCacheStoreUnit, NotHealthyBeforeInit) {
    RedisConfig cfg;
    RedisCacheStore store(cfg);
    EXPECT_FALSE(store.isHealthy());
}

TEST(RedisCacheStoreUnit, OperationsReturnFalseBeforeInit) {
    RedisConfig cfg;
    RedisCacheStore store(cfg);
    EXPECT_FALSE(store.set("key", "val"));
    EXPECT_FALSE(store.get("key").has_value());
    EXPECT_FALSE(store.del("key"));
    EXPECT_FALSE(store.exists("key"));
    EXPECT_EQ(store.size(), 0);
}

TEST(RedisCacheStoreUnit, KeysReturnsEmptyBeforeInit) {
    RedisConfig cfg;
    RedisCacheStore store(cfg);
    auto keys = store.keys("prefix");
    EXPECT_TRUE(keys.empty());
}

TEST(RedisCacheStoreUnit, ConfigParsing) {
    RedisConfig cfg;
    cfg.host = "10.0.0.1";
    cfg.port = 6380;
    cfg.db = 2;
    cfg.pool_size = 8;
    cfg.connect_timeout_ms = 5000;
    cfg.command_timeout_ms = 2000;

    RedisCacheStore store(cfg);
    EXPECT_EQ(store.backendName(), "redis");
}

TEST(RedisCacheStoreUnit, CloseIsIdempotent) {
    RedisConfig cfg;
    RedisCacheStore store(cfg);
    store.close();
    store.close();
    EXPECT_FALSE(store.isHealthy());
}

// Integration tests — only run when AEGISGATE_REDIS_URL is set
class RedisIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* url = std::getenv("AEGISGATE_REDIS_URL");
        if (!url) {
            GTEST_SKIP() << "AEGISGATE_REDIS_URL not set, skipping integration tests";
        }
        // Parse url like redis://host:port
        std::string s(url);
        cfg_.host = "127.0.0.1";
        cfg_.port = 6379;
        if (s.find("redis://") == 0) {
            auto hp = s.substr(8);
            auto colon = hp.find(':');
            if (colon != std::string::npos) {
                cfg_.host = hp.substr(0, colon);
                cfg_.port = std::stoi(hp.substr(colon + 1));
            } else {
                cfg_.host = hp;
            }
        }
        cfg_.pool_size = 2;
        cfg_.db = 15; // use db 15 for tests to avoid clobbering real data
        store_ = std::make_unique<RedisCacheStore>(cfg_);
        ASSERT_TRUE(store_->initialize());
        store_->clear();
    }

    void TearDown() override {
        if (store_) {
            store_->clear();
            store_->close();
        }
    }

    RedisConfig cfg_;
    std::unique_ptr<RedisCacheStore> store_;
};

TEST_F(RedisIntegrationTest, SetGetDel) {
    ASSERT_TRUE(store_->set("test_key", "test_value"));
    auto val = store_->get("test_key");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "test_value");

    EXPECT_TRUE(store_->exists("test_key"));
    EXPECT_TRUE(store_->del("test_key"));
    EXPECT_FALSE(store_->exists("test_key"));
}

TEST_F(RedisIntegrationTest, SetWithTTL) {
    ASSERT_TRUE(store_->set("ttl_key", "value", std::chrono::seconds(60)));
    auto val = store_->get("ttl_key");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "value");
}

TEST_F(RedisIntegrationTest, SizeAndClear) {
    store_->set("k1", "v1");
    store_->set("k2", "v2");
    EXPECT_GE(store_->size(), 2);
    store_->clear();
    EXPECT_EQ(store_->size(), 0);
}

TEST_F(RedisIntegrationTest, KeysScan) {
    store_->set("cache:1", "v1");
    store_->set("cache:2", "v2");
    store_->set("other:key", "v3");

    auto keys = store_->keys("cache:");
    EXPECT_EQ(keys.size(), 2u);
    for (const auto& k : keys) {
        EXPECT_EQ(k.find("aegisgate:"), std::string::npos);
        EXPECT_EQ(k.find("cache:"), 0u);
    }
}

TEST_F(RedisIntegrationTest, IsHealthy) {
    EXPECT_TRUE(store_->isHealthy());
}
