#include "storage/memory_cache_store.h"
#include <gtest/gtest.h>
#include <thread>

using namespace aegisgate;

class MemoryCacheStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryCacheStore>(5);
        store_->initialize();
    }
    std::unique_ptr<MemoryCacheStore> store_;
};

TEST_F(MemoryCacheStoreTest, BackendName) {
    EXPECT_EQ(store_->backendName(), "memory");
}

TEST_F(MemoryCacheStoreTest, InitializeAndHealth) {
    MemoryCacheStore fresh(100);
    EXPECT_FALSE(fresh.isHealthy());
    EXPECT_TRUE(fresh.initialize());
    EXPECT_TRUE(fresh.isHealthy());
}

TEST_F(MemoryCacheStoreTest, SetAndGet) {
    EXPECT_TRUE(store_->set("k1", "v1"));
    auto val = store_->get("k1");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "v1");
}

TEST_F(MemoryCacheStoreTest, GetMissing) {
    EXPECT_FALSE(store_->get("nonexistent").has_value());
}

TEST_F(MemoryCacheStoreTest, Delete) {
    store_->set("k1", "v1");
    EXPECT_TRUE(store_->del("k1"));
    EXPECT_FALSE(store_->get("k1").has_value());
    EXPECT_FALSE(store_->del("k1"));
}

TEST_F(MemoryCacheStoreTest, Exists) {
    EXPECT_FALSE(store_->exists("k1"));
    store_->set("k1", "v1");
    EXPECT_TRUE(store_->exists("k1"));
    store_->del("k1");
    EXPECT_FALSE(store_->exists("k1"));
}

TEST_F(MemoryCacheStoreTest, TTLExpiration) {
    store_->set("k1", "v1", std::chrono::seconds(1));
    EXPECT_TRUE(store_->get("k1").has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    EXPECT_FALSE(store_->get("k1").has_value());
}

TEST_F(MemoryCacheStoreTest, LRUEviction) {
    for (int i = 1; i <= 5; ++i)
        store_->set("k" + std::to_string(i), "v" + std::to_string(i));
    EXPECT_EQ(store_->size(), 5);

    // access k1 to keep it fresh, k2 becomes LRU
    store_->get("k1");

    store_->set("k6", "v6");
    EXPECT_EQ(store_->size(), 5);
    EXPECT_FALSE(store_->get("k2").has_value());
    EXPECT_TRUE(store_->get("k1").has_value());
    EXPECT_TRUE(store_->get("k6").has_value());
}

TEST_F(MemoryCacheStoreTest, ClearAll) {
    store_->set("k1", "v1");
    store_->set("k2", "v2");
    EXPECT_EQ(store_->size(), 2);
    store_->clear();
    EXPECT_EQ(store_->size(), 0);
}

TEST_F(MemoryCacheStoreTest, OverwriteExistingKey) {
    store_->set("k1", "v1");
    store_->set("k1", "v2");
    auto val = store_->get("k1");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "v2");
    EXPECT_EQ(store_->size(), 1);
}

TEST_F(MemoryCacheStoreTest, CloseAndReinitialize) {
    store_->set("k1", "v1");
    store_->close();
    EXPECT_FALSE(store_->isHealthy());
    EXPECT_FALSE(store_->get("k1").has_value());
    store_->initialize();
    EXPECT_TRUE(store_->isHealthy());
    EXPECT_EQ(store_->size(), 0);
}
