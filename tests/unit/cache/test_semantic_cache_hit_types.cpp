// TASK-20260527-02 Epic 3.1 — cache_hit_by_type taxonomy (D6=A 3 类).
//
// SemanticCache::CacheStats 加 hit_exact / hit_semantic / hit_conversation
// 3 字段，让 MVP-5 case-study Row 4 Card 2 能展示"cache hit Y%"细分。
//
// 测试三档：
//   1. ExactHitClassification — partition_key="" + max_sim≈1.0 → exact
//   2. SemanticHitClassification — partition_key="" + 0.99..>max_sim≥threshold → semantic
//   3. ConversationHitClassification — partition_key 非空 → conversation（V2 path 信号）
//
// 由于 HashEmbedder 离散性导致语义命中难以稳定触发，主要走 classifyHitType
// 静态 helper 单元测试 + 端到端 exact / conversation 流程。

#include <gtest/gtest.h>
#include "cache/semantic_cache.h"
#include "cache/hnsw_vector_store.h"
#include <chrono>

using namespace aegisgate;
using namespace std::chrono_literals;

class SemanticCacheHitTypeTest : public ::testing::Test {
protected:
    void SetUp() override {
        embedder_ = std::make_unique<HashEmbedder>(32);
        store_ = std::make_unique<HnswVectorStore>(32, 50000);
        store_->initialize();
        cache_ = std::make_unique<SemanticCache>(
            *embedder_, *store_, 0.9f, 3600s, 100);
    }

    std::unique_ptr<HashEmbedder> embedder_;
    std::unique_ptr<HnswVectorStore> store_;
    std::unique_ptr<SemanticCache> cache_;
};

// === Helper-level classification (deterministic) ===

TEST_F(SemanticCacheHitTypeTest, ClassifyExactHit) {
    // TASK-20260609-01 (D1=C): conversation signal is now an explicit bool,
    // decoupled from the (always-non-empty) V2 tenant-isolated partition_key.
    EXPECT_EQ(SemanticCache::classifyHitType(1.0f, false), "exact");
    EXPECT_EQ(SemanticCache::classifyHitType(0.9999f, false), "exact");
}

TEST_F(SemanticCacheHitTypeTest, ClassifySemanticHit) {
    EXPECT_EQ(SemanticCache::classifyHitType(0.95f, false), "semantic");
    EXPECT_EQ(SemanticCache::classifyHitType(0.5f, false), "semantic");
}

TEST_F(SemanticCacheHitTypeTest, ClassifyConversationHit) {
    // conversation_scoped=true (real multi-turn context) → conversation,
    // regardless of similarity. tenant_id mixing no longer forces this.
    EXPECT_EQ(SemanticCache::classifyHitType(1.0f, true), "conversation");
    EXPECT_EQ(SemanticCache::classifyHitType(0.95f, true), "conversation");
}

// === End-to-end (per plan T3.1.2-T3.1.4) ===

TEST_F(SemanticCacheHitTypeTest, HitExactIncrementsCounter) {
    cache_->put("What is Python?", "Python is a programming language.");
    auto hit = cache_->get("What is Python?");
    ASSERT_TRUE(hit.has_value());

    auto stats = cache_->getStats();
    EXPECT_EQ(stats.hit_count, 1u);
    EXPECT_EQ(stats.hit_exact, 1u);
    EXPECT_EQ(stats.hit_semantic, 0u);
    EXPECT_EQ(stats.hit_conversation, 0u);
}

TEST_F(SemanticCacheHitTypeTest, HitConversationIncrementsCounter) {
    // V2 path：put + get 同一 prompt 但传非空 partition_key
    // → 视为 conversation-scoped hit（spec §3.2.2）。
    const std::string pk = "conv:demo-tenant:user-1:hash-abc";
    cache_->put("Hello?", "Hi there!", "", pk);
    // conversation_scoped=true marks this as a real multi-turn (conversation)
    // hit (the signal that used to be inferred from a non-empty partition_key).
    auto hit = cache_->get("Hello?", "", pk, true);
    ASSERT_TRUE(hit.has_value());

    auto stats = cache_->getStats();
    EXPECT_EQ(stats.hit_count, 1u);
    EXPECT_EQ(stats.hit_conversation, 1u);
    EXPECT_EQ(stats.hit_exact, 0u);
    EXPECT_EQ(stats.hit_semantic, 0u);
}
