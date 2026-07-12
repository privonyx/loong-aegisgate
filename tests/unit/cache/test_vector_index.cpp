#include <gtest/gtest.h>
#include "cache/vector_index.h"
#include "cache/embedder.h"
#include <cmath>
#include <filesystem>

using namespace aegisgate;

class VectorIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        index_ = std::make_unique<VectorIndex>(dim_, 1000);
    }

    std::vector<float> makeVec(float val) {
        std::vector<float> v(dim_, 0.0f);
        v[0] = val;
        // L2-normalize
        float norm = std::abs(val);
        if (norm > 1e-8f) v[0] /= norm;
        return v;
    }

    std::vector<float> makeNormVec(size_t seed) {
        std::vector<float> v(dim_, 0.0f);
        for (size_t i = 0; i < dim_; i++) {
            v[i] = static_cast<float>((seed * 31 + i * 17) % 1000) / 1000.0f - 0.5f;
        }
        float norm = 0.0f;
        for (float x : v) norm += x * x;
        norm = std::sqrt(norm);
        if (norm > 1e-8f) for (float& x : v) x /= norm;
        return v;
    }

    size_t dim_ = 32;
    std::unique_ptr<VectorIndex> index_;
};

TEST_F(VectorIndexTest, InsertAndSearch) {
    auto v1 = makeNormVec(1);
    index_->insert("id-1", v1);

    auto results = index_->search(v1, 1);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].id, "id-1");
    EXPECT_GT(results[0].similarity, 0.9f);
}

TEST_F(VectorIndexTest, SearchReturnsTopK) {
    for (int i = 0; i < 10; i++) {
        index_->insert("id-" + std::to_string(i), makeNormVec(i));
    }

    auto query = makeNormVec(0);
    auto results = index_->search(query, 3);
    EXPECT_LE(results.size(), 3u);
    EXPECT_EQ(results[0].id, "id-0");
}

TEST_F(VectorIndexTest, ThresholdFiltering) {
    auto v1 = makeNormVec(1);
    auto v2 = makeNormVec(100);
    index_->insert("similar", v1);
    index_->insert("different", v2);

    auto results = index_->search(v1, 10, 0.99f);
    for (auto& r : results) {
        EXPECT_GE(r.similarity, 0.99f);
    }
}

TEST_F(VectorIndexTest, SizeTracking) {
    EXPECT_EQ(index_->size(), 0u);
    index_->insert("a", makeNormVec(1));
    EXPECT_EQ(index_->size(), 1u);
    index_->insert("b", makeNormVec(2));
    EXPECT_EQ(index_->size(), 2u);
}

TEST_F(VectorIndexTest, Contains) {
    index_->insert("exists", makeNormVec(1));
    EXPECT_TRUE(index_->contains("exists"));
    EXPECT_FALSE(index_->contains("missing"));
}

TEST_F(VectorIndexTest, Remove) {
    index_->insert("removable", makeNormVec(1));
    EXPECT_TRUE(index_->contains("removable"));
    EXPECT_TRUE(index_->remove("removable"));
    EXPECT_FALSE(index_->contains("removable"));
}

TEST_F(VectorIndexTest, DuplicateInsertIgnored) {
    auto v = makeNormVec(1);
    index_->insert("dup", v);
    index_->insert("dup", v);
    EXPECT_EQ(index_->size(), 1u);
}

TEST_F(VectorIndexTest, EmptySearchReturnsNothing) {
    auto results = index_->search(makeNormVec(1), 5);
    EXPECT_TRUE(results.empty());
}

TEST_F(VectorIndexTest, Dimension) {
    EXPECT_EQ(index_->dimension(), 32u);
}

TEST_F(VectorIndexTest, SaveAndLoadRestoresIdMap) {
    auto v1 = makeNormVec(42);
    index_->insert("persist-id", v1);
    const auto base = (std::filesystem::temp_directory_path() / "aegisgate_vi_test").string();
    const std::string path = base + ".bin";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + ".idmap", ec);
    index_->save(path);

    VectorIndex loaded(dim_, 1000);
    loaded.load(path);
    EXPECT_TRUE(loaded.contains("persist-id"));
    auto results = loaded.search(v1, 1);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].id, "persist-id");

    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + ".idmap", ec);
}
