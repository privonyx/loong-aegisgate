#include <gtest/gtest.h>
#include "cache/semantic_cache.h"
#include "cache/hnsw_vector_store.h"
#include "cache/rule_based_summarizer.h"
#include "observe/metrics.h"
#include <algorithm>

using namespace aegisgate;
using namespace std::chrono_literals;

class SemanticCacheV2Test : public ::testing::Test {
protected:
    void SetUp() override {
        embedder_ = std::make_unique<HashEmbedder>(32);
        store_ = std::make_unique<HnswVectorStore>(32, 50000);
        store_->initialize();
        cache_ = std::make_unique<SemanticCache>(
            *embedder_, *store_, 0.99f, 3600s, 100);

        ConversationHashConfig cfg;
        cfg.mode = ConversationHashMode::Full;
        cache_->setConversationHashConfig(cfg);
    }

    std::unique_ptr<HashEmbedder> embedder_;
    std::unique_ptr<HnswVectorStore> store_;
    std::unique_ptr<SemanticCache> cache_;
};

namespace {
std::vector<Message> twoTurn() {
    return {
        {"user", "What is Python?"},
        {"assistant", "A programming language."},
        {"user", "How is it different from C?"}
    };
}
} // namespace

// SR1: cross-tenant hard isolation. Two tenants with the same client-supplied
// conversation_id and identical history must end up in different partitions.
TEST_F(SemanticCacheV2Test, CrossTenantHardIsolation_SR1) {
    auto msgs = twoTurn();
    auto info_a = cache_->extractCacheKeyInfoV2(msgs, "tenant-a", "shared-conv-id");
    auto info_b = cache_->extractCacheKeyInfoV2(msgs, "tenant-b", "shared-conv-id");
    EXPECT_NE(info_a.partition_key, info_b.partition_key);
    EXPECT_FALSE(info_a.partition_key.empty());
}

// SR1 corollary: empty conversation_id must still keep tenants isolated.
TEST_F(SemanticCacheV2Test, CrossTenantIsolationEvenWithEmptyConversationId_SR1) {
    auto msgs = twoTurn();
    auto info_a = cache_->extractCacheKeyInfoV2(msgs, "tenant-a", "");
    auto info_b = cache_->extractCacheKeyInfoV2(msgs, "tenant-b", "");
    EXPECT_NE(info_a.partition_key, info_b.partition_key);
}

TEST_F(SemanticCacheV2Test, PartitionKeyIsSha256Hex) {
    auto info = cache_->extractCacheKeyInfoV2(twoTurn(), "tenant-x", "conv-1");
    ASSERT_EQ(info.partition_key.size(), 64u); // sha256 hex
    EXPECT_TRUE(std::all_of(info.partition_key.begin(), info.partition_key.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
}

TEST_F(SemanticCacheV2Test, V2DiffersFromV1) {
    auto v1 = SemanticCache::computeConversationHash(
        twoTurn(), {ConversationHashMode::Full, 4});
    auto v2 = SemanticCache::computeConversationHashV2(
        twoTurn(), {ConversationHashMode::Full, 4}, nullptr);
    EXPECT_NE(v1, v2);
    EXPECT_EQ(v2.size(), 64u);
}

TEST_F(SemanticCacheV2Test, ConversationIdAffectsPartitionKey) {
    auto msgs = twoTurn();
    auto k1 = cache_->extractCacheKeyInfoV2(msgs, "t1", "conv-1");
    auto k2 = cache_->extractCacheKeyInfoV2(msgs, "t1", "conv-2");
    EXPECT_NE(k1.partition_key, k2.partition_key);
}

TEST_F(SemanticCacheV2Test, SameInputsProduceSameKey) {
    auto msgs = twoTurn();
    auto k1 = cache_->extractCacheKeyInfoV2(msgs, "t1", "conv-1");
    auto k2 = cache_->extractCacheKeyInfoV2(msgs, "t1", "conv-1");
    EXPECT_EQ(k1.partition_key, k2.partition_key);
}

TEST_F(SemanticCacheV2Test, SystemPromptInfluencesKey) {
    std::vector<Message> with_sys = {
        {"system", "You are a helpful assistant."},
        {"user", "What is Python?"},
        {"assistant", "A programming language."},
        {"user", "How is it different from C?"}
    };
    std::vector<Message> without_sys = twoTurn();
    auto k1 = cache_->extractCacheKeyInfoV2(with_sys, "t", "c");
    auto k2 = cache_->extractCacheKeyInfoV2(without_sys, "t", "c");
    EXPECT_NE(k1.partition_key, k2.partition_key);
}

TEST_F(SemanticCacheV2Test, NullSummarizerStillProducesValidKey) {
    auto info = cache_->extractCacheKeyInfoV2(twoTurn(), "t", "c", nullptr);
    EXPECT_EQ(info.partition_key.size(), 64u);
    EXPECT_FALSE(info.conversation_hash.empty());
}

TEST_F(SemanticCacheV2Test, SummarizerInjectionAltersConversationHash) {
    RuleBasedSummarizer summarizer(256, 5);
    auto with = SemanticCache::computeConversationHashV2(
        twoTurn(), {ConversationHashMode::Full, 4}, &summarizer);
    auto without = SemanticCache::computeConversationHashV2(
        twoTurn(), {ConversationHashMode::Full, 4}, nullptr);
    EXPECT_NE(with, without);
    EXPECT_EQ(with.size(), 64u);
}

TEST_F(SemanticCacheV2Test, EmptyMessagesProducesEmptyKeyMaterial) {
    auto info = cache_->extractCacheKeyInfoV2({}, "tenant", "conv");
    EXPECT_TRUE(info.partition_key.empty());
    EXPECT_TRUE(info.prompt.empty());
    EXPECT_TRUE(info.conversation_hash.empty());
}

TEST_F(SemanticCacheV2Test, V1EntryStillAvailableForBackwardCompat) {
    // V1 entry point unchanged: extractCacheKeyInfo(msgs) still works.
    auto info_v1 = cache_->extractCacheKeyInfo(twoTurn());
    EXPECT_FALSE(info_v1.prompt.empty());
}

// SR1 hot-path isolation (Epic 1): process()/putFromContext must derive a
// tenant-scoped partition key so tenant B can never read tenant A's cached
// response, and a tenant with no cached entry must never get a cross-tenant
// leak. Exercises the real hot path (SemanticCache::process + putFromContext),
// not just key derivation.
TEST_F(SemanticCacheV2Test, CrossTenantHotPathIsolation_SR1) {
    const std::vector<Message> msgs = {{"user", "What is the launch password?"}};

    cache_->putFromContext(msgs, "secret-for-A", "gpt-4", "tenant-a");
    cache_->putFromContext(msgs, "secret-for-B", "gpt-4", "tenant-b");

    auto makeCtx = [&](const std::string& tenant) {
        RequestContext ctx;
        ctx.tenant_id = tenant;
        ctx.chat_request.model = "gpt-4";
        ctx.chat_request.messages = msgs;
        return ctx;
    };

    auto ctx_a = makeCtx("tenant-a");
    EXPECT_EQ(cache_->process(ctx_a), StageResult::ShortCircuit);
    EXPECT_TRUE(ctx_a.cache_hit);
    EXPECT_EQ(ctx_a.cached_response, "secret-for-A");

    auto ctx_b = makeCtx("tenant-b");
    EXPECT_EQ(cache_->process(ctx_b), StageResult::ShortCircuit);
    EXPECT_TRUE(ctx_b.cache_hit);
    EXPECT_EQ(ctx_b.cached_response, "secret-for-B");

    // Tenant with no cached entry must miss — no cross-tenant leak.
    auto ctx_c = makeCtx("tenant-c");
    EXPECT_EQ(cache_->process(ctx_c), StageResult::Continue);
    EXPECT_FALSE(ctx_c.cache_hit);
}

// --- D3=A / SR-6: cross_tenant fail-safe isolation matrix (TASK-20260703-04) ---
// The getCrossTenant() path bypasses partition_key tenant isolation, so it is a
// data-exfiltration surface. It MUST stay off by default, gate on the configured
// similarity threshold, audit every hit, and never affect same-tenant lookups.

// Row 1: default config (never wired) → enabled=false → no cross-tenant hit.
// This is the zero-regression baseline; mutating the default to true breaks it.
TEST_F(SemanticCacheV2Test, CrossTenantDisabledByDefault_NoCrossHit_SR6) {
    cache_->put("what is the deploy key", "SECRET-A", "gpt-4", "tenant-a");
    auto hit = cache_->get("what is the deploy key", "gpt-4", "tenant-b");
    EXPECT_FALSE(hit.has_value());
}

// Row 2: enabled but candidate similarity below threshold → no cross hit.
// Mutating away the threshold comparison lets this leak.
TEST_F(SemanticCacheV2Test, CrossTenantEnabledBelowThreshold_NoCrossHit_SR6) {
    cache_->setCrossTenantConfig(CrossTenantConfig(true, 1.01f));
    cache_->put("what is the deploy key", "SECRET-A", "gpt-4", "tenant-a");
    auto hit = cache_->get("what is the deploy key", "gpt-4", "tenant-b");
    EXPECT_FALSE(hit.has_value());
}

// Row 3: enabled + similarity >= threshold → cross hit AND audited (+1 metric).
TEST_F(SemanticCacheV2Test, CrossTenantEnabledAboveThreshold_HitAndAudited_SR6) {
    auto& metric = MetricsRegistry::instance().crossTenantCacheHitsTotal();
    double before = metric.get();

    cache_->setCrossTenantConfig(CrossTenantConfig(true, 0.95f));
    cache_->put("what is the deploy key", "SHARED-ANSWER", "gpt-4", "tenant-a");
    auto hit = cache_->get("what is the deploy key", "gpt-4", "tenant-b");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->response, "SHARED-ANSWER");
    EXPECT_GT(MetricsRegistry::instance().crossTenantCacheHitsTotal().get(),
              before);
}

// Row 4: same-tenant path is unaffected by the cross_tenant switch.
TEST_F(SemanticCacheV2Test, SameTenantPathUnaffectedByCrossTenantFlag_SR6) {
    cache_->setCrossTenantConfig(CrossTenantConfig(true, 0.95f));
    cache_->put("hello world question", "ANSWER-A", "gpt-4", "tenant-a");
    auto hit = cache_->get("hello world question", "gpt-4", "tenant-a");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->response, "ANSWER-A");
}

// SR1 separator-injection guard: tenant containing the separator byte must
// not collide with split forms.
TEST_F(SemanticCacheV2Test, SeparatorInjectionGuard_SR1) {
    auto msgs = twoTurn();
    auto k1 = cache_->extractCacheKeyInfoV2(msgs, std::string("a\x1f""b"), "");
    auto k2 = cache_->extractCacheKeyInfoV2(msgs, "a", "b");
    // Both must be hex sha256 and different from each other thanks to the
    // ordered field layout (tenant first, conversation_id second).
    ASSERT_EQ(k1.partition_key.size(), 64u);
    ASSERT_EQ(k2.partition_key.size(), 64u);
    EXPECT_NE(k1.partition_key, k2.partition_key);
}
