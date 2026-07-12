#include <gtest/gtest.h>
#include "cache/partitioned_vector_index.h"
#include "cache/embedder.h"

using namespace aegisgate;

class PVITest : public ::testing::Test {
protected:
    void SetUp() override {
        embedder_ = std::make_unique<HashEmbedder>(32);
        pvi_ = std::make_unique<PartitionedVectorIndex>(32, 1000, 64);
    }

    std::vector<float> embed(const std::string& text) {
        return embedder_->embed(text);
    }

    std::unique_ptr<HashEmbedder> embedder_;
    std::unique_ptr<PartitionedVectorIndex> pvi_;
};

TEST_F(PVITest, InsertAndSearchSamePartition) {
    pvi_->insert("ctx_a", "id1", embed("hello world"));
    auto results = pvi_->search("ctx_a", embed("hello world"), 1, 0.0f);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].id, "id1");
}

TEST_F(PVITest, DifferentPartitionsIsolated) {
    auto vec = embed("hello");
    pvi_->insert("ctx_a", "id1", vec);

    auto results = pvi_->search("ctx_b", vec, 1, 0.0f);
    EXPECT_TRUE(results.empty());

    results = pvi_->search("ctx_a", vec, 1, 0.0f);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].id, "id1");
}

TEST_F(PVITest, DefaultPartitionForEmptyKey) {
    pvi_->insert("", "id1", embed("test"));
    auto results = pvi_->search("", embed("test"), 1, 0.0f);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].id, "id1");
}

TEST_F(PVITest, SizeAcrossPartitions) {
    pvi_->insert("a", "id1", embed("q1"));
    pvi_->insert("b", "id2", embed("q2"));
    pvi_->insert("a", "id3", embed("q3"));
    EXPECT_EQ(pvi_->size(), 3u);
    EXPECT_EQ(pvi_->partitionCount(), 2u);
}

TEST_F(PVITest, RemoveFromPartition) {
    pvi_->insert("ctx", "id1", embed("q1"));
    EXPECT_EQ(pvi_->size(), 1u);
    EXPECT_TRUE(pvi_->contains("ctx", "id1"));

    pvi_->remove("ctx", "id1");
    EXPECT_EQ(pvi_->size(), 0u);
    EXPECT_FALSE(pvi_->contains("ctx", "id1"));
}

TEST_F(PVITest, RemoveFromNonexistentPartition) {
    EXPECT_FALSE(pvi_->remove("no_such_ctx", "id1"));
}

TEST_F(PVITest, ContainsChecksCrossPartition) {
    pvi_->insert("ctx_a", "id1", embed("q1"));
    EXPECT_TRUE(pvi_->contains("ctx_a", "id1"));
    EXPECT_FALSE(pvi_->contains("ctx_b", "id1"));
}

TEST_F(PVITest, OverflowPartitionWhenMaxReached) {
    auto small_pvi = std::make_unique<PartitionedVectorIndex>(32, 1000, 2);

    small_pvi->insert("ctx_1", "id1", embed("q1"));
    small_pvi->insert("ctx_2", "id2", embed("q2"));
    // 3rd distinct context → should go to overflow
    small_pvi->insert("ctx_3", "id3", embed("q3"));
    EXPECT_EQ(small_pvi->size(), 3u);

    // overflow partition should be searchable via the original key
    auto results = small_pvi->search("ctx_3", embed("q3"), 1, 0.0f);
    EXPECT_EQ(results.size(), 1u);
}

TEST_F(PVITest, SearchEmptyPartitionReturnsEmpty) {
    auto results = pvi_->search("empty_ctx", embed("anything"), 1, 0.0f);
    EXPECT_TRUE(results.empty());
}

TEST_F(PVITest, ThresholdFiltering) {
    pvi_->insert("ctx", "id1", embed("hello world"));
    auto results = pvi_->search("ctx", embed("completely different text"), 1, 0.999f);
    EXPECT_TRUE(results.empty());
}

TEST_F(PVITest, MultipleEntriesSamePartition) {
    for (int i = 0; i < 10; ++i) {
        pvi_->insert("ctx", "id_" + std::to_string(i),
                     embed("query_" + std::to_string(i)));
    }
    EXPECT_EQ(pvi_->size(), 10u);
    EXPECT_EQ(pvi_->partitionCount(), 1u);

    auto results = pvi_->search("ctx", embed("query_5"), 3, 0.0f);
    EXPECT_GE(results.size(), 1u);
}

TEST_F(PVITest, RemoveUsesNonConstPartitionAccess) {
    pvi_->insert("tenant-a", "id1", embed("query"));
    EXPECT_EQ(pvi_->size(), 1u);
    EXPECT_TRUE(pvi_->remove("tenant-a", "id1"));
    EXPECT_EQ(pvi_->size(), 0u);

    EXPECT_FALSE(pvi_->remove("non-existent", "id1"));
}
