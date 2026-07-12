#include <gtest/gtest.h>
#include "cache/semantic_cache.h"
#include "cache/embedder.h"
#include "cache/hnsw_vector_store.h"

using namespace aegisgate;
using namespace std::chrono_literals;

class CacheFeedbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        embedder_ = std::make_unique<HashEmbedder>(128);
        store_ = std::make_unique<HnswVectorStore>(128, 1000, 1);
        store_->initialize();
        cache_ = std::make_unique<SemanticCache>(*embedder_, *store_, 0.8f);
    }
    std::unique_ptr<HashEmbedder> embedder_;
    std::unique_ptr<HnswVectorStore> store_;
    std::unique_ptr<SemanticCache> cache_;
};

TEST_F(CacheFeedbackTest, RecordFeedback) {
    cache_->put("hello world", "response");
    cache_->recordFeedback("hello world", 0.9);
    cache_->recordFeedback("hello world", 0.7);

    double avg = cache_->getAverageSatisfaction();
    EXPECT_NEAR(avg, 0.8, 0.01);
}

TEST_F(CacheFeedbackTest, FeedbackWindowLimit) {
    cache_->put("query", "response");
    for (size_t i = 0; i < 1000; ++i) {
        cache_->recordFeedback("query", 0.0);
    }
    for (size_t i = 0; i < 1000; ++i) {
        cache_->recordFeedback("query", 1.0);
    }
    double avg = cache_->getAverageSatisfaction();
    EXPECT_NEAR(avg, 1.0, 0.01);
}

TEST_F(CacheFeedbackTest, QueryPatternTracking) {
    cache_->recordQueryPattern("alpha");
    cache_->recordQueryPattern("alpha");
    cache_->recordQueryPattern("alpha");
    cache_->recordQueryPattern("beta");
    cache_->recordQueryPattern("beta");
    cache_->recordQueryPattern("gamma");

    auto top = cache_->getTopPatterns(2);
    ASSERT_EQ(top.size(), 2u);
    EXPECT_EQ(top[0].prompt, "alpha");
    EXPECT_EQ(top[0].frequency, 3u);
    EXPECT_EQ(top[1].prompt, "beta");
    EXPECT_EQ(top[1].frequency, 2u);
}

TEST_F(CacheFeedbackTest, QueryPatternLimit) {
    for (size_t i = 0; i < 1100; ++i) {
        cache_->recordQueryPattern("pattern_" + std::to_string(i));
    }
    auto top = cache_->getTopPatterns(1100);
    EXPECT_LE(top.size(), 1000u);
}

TEST_F(CacheFeedbackTest, CrossTenantConfig) {
    CrossTenantConfig cfg(true, 0.92f);
    cache_->setCrossTenantConfig(cfg);

    auto hit = cache_->getCrossTenant("nonexistent");
    EXPECT_FALSE(hit.has_value());
}

TEST_F(CacheFeedbackTest, CrossTenantSearch) {
    cache_->put("cross tenant question", "shared answer", "", "");

    CrossTenantConfig cfg(true, 0.8f);
    cache_->setCrossTenantConfig(cfg);

    auto hit = cache_->getCrossTenant("cross tenant question");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->response, "shared answer");
}
