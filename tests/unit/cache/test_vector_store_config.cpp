#include <gtest/gtest.h>
#include "core/config.h"
#include <fstream>
#include <filesystem>

using namespace aegisgate;

class VectorStoreConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = std::filesystem::temp_directory_path() / "aegisgate_vs_cfg_test";
        std::filesystem::create_directories(tmp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(tmp_dir_);
    }

    std::string writeConfig(const std::string& content) {
        auto path = (tmp_dir_ / "test_config.yaml").string();
        std::ofstream f(path);
        f << content;
        return path;
    }

    std::filesystem::path tmp_dir_;
};

TEST_F(VectorStoreConfigTest, DefaultsToHnswlib) {
    auto path = writeConfig("server:\n  port: 8080\n");
    Config config;
    ASSERT_TRUE(config.loadFromFile(path));
    EXPECT_EQ(config.vectorStoreBackend(), "hnswlib");
}

TEST_F(VectorStoreConfigTest, ParsesMilvusConfig) {
    auto path = writeConfig(R"(
vector_store:
  backend: milvus
  milvus:
    host: "10.0.0.1"
    port: 19530
    collection_prefix: "my_cache"
    metric_type: "COSINE"
    token: "secret123"
    connect_timeout_ms: 3000
    request_timeout_ms: 8000
    auto_create_collection: false
)");
    Config config;
    ASSERT_TRUE(config.loadFromFile(path));

    EXPECT_EQ(config.vectorStoreBackend(), "milvus");
    EXPECT_EQ(config.milvusHost(), "10.0.0.1");
    EXPECT_EQ(config.milvusPort(), 19530);
    EXPECT_EQ(config.milvusCollectionPrefix(), "my_cache");
    EXPECT_EQ(config.milvusMetricType(), "COSINE");
    EXPECT_EQ(config.milvusToken(), "secret123");
    EXPECT_EQ(config.milvusConnectTimeout(), 3000);
    EXPECT_EQ(config.milvusRequestTimeout(), 8000);
    EXPECT_FALSE(config.milvusAutoCreateCollection());
}

TEST_F(VectorStoreConfigTest, ParsesQdrantConfig) {
    auto path = writeConfig(R"(
vector_store:
  backend: qdrant
  qdrant:
    host: "qdrant.cluster.local"
    port: 6334
    collection_prefix: "semantic"
    distance: "Dot"
    api_key: "qdrant-key"
    connect_timeout_ms: 2000
    request_timeout_ms: 5000
    auto_create_collection: true
)");
    Config config;
    ASSERT_TRUE(config.loadFromFile(path));

    EXPECT_EQ(config.vectorStoreBackend(), "qdrant");
    EXPECT_EQ(config.qdrantHost(), "qdrant.cluster.local");
    EXPECT_EQ(config.qdrantPort(), 6334);
    EXPECT_EQ(config.qdrantCollectionPrefix(), "semantic");
    EXPECT_EQ(config.qdrantDistance(), "Dot");
    EXPECT_EQ(config.qdrantApiKey(), "qdrant-key");
    EXPECT_EQ(config.qdrantConnectTimeout(), 2000);
    EXPECT_EQ(config.qdrantRequestTimeout(), 5000);
    EXPECT_TRUE(config.qdrantAutoCreateCollection());
}

TEST_F(VectorStoreConfigTest, MilvusDefaultValues) {
    auto path = writeConfig("vector_store:\n  backend: milvus\n");
    Config config;
    ASSERT_TRUE(config.loadFromFile(path));

    EXPECT_EQ(config.milvusHost(), "127.0.0.1");
    EXPECT_EQ(config.milvusPort(), 19530);
    EXPECT_EQ(config.milvusCollectionPrefix(), "aegisgate_cache");
    EXPECT_EQ(config.milvusMetricType(), "IP");
    EXPECT_TRUE(config.milvusToken().empty());
    EXPECT_EQ(config.milvusConnectTimeout(), 5000);
    EXPECT_EQ(config.milvusRequestTimeout(), 10000);
    EXPECT_TRUE(config.milvusAutoCreateCollection());
}

TEST_F(VectorStoreConfigTest, QdrantDefaultValues) {
    auto path = writeConfig("vector_store:\n  backend: qdrant\n");
    Config config;
    ASSERT_TRUE(config.loadFromFile(path));

    EXPECT_EQ(config.qdrantHost(), "127.0.0.1");
    EXPECT_EQ(config.qdrantPort(), 6333);
    EXPECT_EQ(config.qdrantCollectionPrefix(), "aegisgate_cache");
    EXPECT_EQ(config.qdrantDistance(), "Cosine");
    EXPECT_TRUE(config.qdrantApiKey().empty());
    EXPECT_EQ(config.qdrantConnectTimeout(), 5000);
    EXPECT_EQ(config.qdrantRequestTimeout(), 10000);
    EXPECT_TRUE(config.qdrantAutoCreateCollection());
}
