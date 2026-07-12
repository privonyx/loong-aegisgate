#include <gtest/gtest.h>
#include "cache/embedder.h"

using namespace aegisgate;

class HashEmbedderTest : public ::testing::Test {
protected:
    HashEmbedder embedder_{128};
};

TEST_F(HashEmbedderTest, ProducesCorrectDimension) {
    auto vec = embedder_.embed("Hello world");
    EXPECT_EQ(vec.size(), 128u);
}

TEST_F(HashEmbedderTest, DeterministicOutput) {
    auto v1 = embedder_.embed("test input");
    auto v2 = embedder_.embed("test input");
    ASSERT_EQ(v1.size(), v2.size());
    for (size_t i = 0; i < v1.size(); i++) {
        EXPECT_FLOAT_EQ(v1[i], v2[i]);
    }
}

TEST_F(HashEmbedderTest, DifferentTextsProduceDifferentVectors) {
    auto v1 = embedder_.embed("Hello world");
    auto v2 = embedder_.embed("Goodbye world");
    bool all_same = true;
    for (size_t i = 0; i < v1.size(); i++) {
        if (std::abs(v1[i] - v2[i]) > 1e-6f) {
            all_same = false;
            break;
        }
    }
    EXPECT_FALSE(all_same);
}

TEST_F(HashEmbedderTest, OutputIsNormalized) {
    auto vec = embedder_.embed("Normalize this");
    float norm = 0.0f;
    for (float v : vec) norm += v * v;
    EXPECT_NEAR(std::sqrt(norm), 1.0f, 0.01f);
}

TEST_F(HashEmbedderTest, EmptyInputProducesZeroVector) {
    auto vec = embedder_.embed("");
    float sum = 0.0f;
    for (float v : vec) sum += std::abs(v);
    EXPECT_NEAR(sum, 0.0f, 1e-6f);
}

TEST_F(HashEmbedderTest, CustomDimension) {
    HashEmbedder e256(256);
    auto vec = e256.embed("test");
    EXPECT_EQ(vec.size(), 256u);
    EXPECT_EQ(e256.dimension(), 256u);
}

TEST(CosineSimilarityTest, IdenticalVectorsReturnOne) {
    std::vector<float> v = {1.0f, 0.0f, 0.0f};
    EXPECT_NEAR(cosineSimilarity(v, v), 1.0f, 1e-6f);
}

TEST(CosineSimilarityTest, OrthogonalVectorsReturnZero) {
    std::vector<float> v1 = {1.0f, 0.0f, 0.0f};
    std::vector<float> v2 = {0.0f, 1.0f, 0.0f};
    EXPECT_NEAR(cosineSimilarity(v1, v2), 0.0f, 1e-6f);
}

TEST(CosineSimilarityTest, OppositeVectorsReturnNegativeOne) {
    std::vector<float> v1 = {1.0f, 0.0f};
    std::vector<float> v2 = {-1.0f, 0.0f};
    EXPECT_NEAR(cosineSimilarity(v1, v2), -1.0f, 1e-6f);
}

TEST(CosineSimilarityTest, EmptyVectorsReturnZero) {
    std::vector<float> v1, v2;
    EXPECT_NEAR(cosineSimilarity(v1, v2), 0.0f, 1e-6f);
}

#ifdef AEGISGATE_ENABLE_ONNX
#include "cache/onnx_embedder.h"

TEST(OnnxEmbedderTest, MissingModelReturnsNotReady) {
    aegisgate::OnnxEmbedder embedder("/nonexistent/model.onnx", "/nonexistent/vocab.txt");
    EXPECT_FALSE(embedder.isReady());
}

TEST(OnnxEmbedderTest, MissingModelEmbedReturnsZeroVector) {
    aegisgate::OnnxEmbedder embedder("/nonexistent/model.onnx", "/nonexistent/vocab.txt");
    auto vec = embedder.embed("test");
    bool all_zero = true;
    for (float v : vec) {
        if (v != 0.0f) { all_zero = false; break; }
    }
    EXPECT_TRUE(all_zero);
}
#endif
