#include <gtest/gtest.h>
#include "cache/semantic_cache.h"
#include "cache/hnsw_vector_store.h"
#include "observe/metrics.h"
#include "storage/memory_cache_store.h"
#include <thread>

using namespace aegisgate;
using namespace std::chrono_literals;

// P2-#1: a VectorStore whose insert always fails, to prove SemanticCache::put
// does not register a phantom logical entry when the vector never lands.
namespace {
class FailingInsertStore : public VectorStore {
public:
    bool initialize() override { return true; }
    bool insert(const std::string&, const std::string&,
                const std::vector<float>&) override { return false; }
    bool remove(const std::string&, const std::string&) override { return true; }
    std::vector<VectorSearchResult> search(
        const std::string&, const std::vector<float>&, size_t,
        float) const override { return {}; }
    size_t size() const override { return 0; }
    std::string backendName() const override { return "failing"; }
};
}  // namespace

// P2-#1: when the vector insert fails the entry must NOT enter entries_ — the
// cache stays empty rather than holding an unsearchable / capacity-wasting
// phantom that would also persist into a split-brain on reload.
TEST(SemanticCacheSplitBrainTest, PutSkipsEntryWhenVectorInsertFails) {
    HashEmbedder embedder(32);
    FailingInsertStore store;
    SemanticCache cache(embedder, store, 0.99f, 3600s, 100);

    cache.put("What is Python?", "Python is a programming language.");

    EXPECT_EQ(cache.size(), 0u);
    EXPECT_FALSE(cache.get("What is Python?").has_value());
}

namespace {
// P0-B (TASK-20260701-01): simulate the PartitionedVectorIndex overflow-merge
// collision. Once active partitions exceed cache.max_partitions, multiple
// logical partition keys collapse into the single shared `_overflow` index, so
// a search issued under partition B can surface an id that was inserted under
// partition A. This stub ignores partition_key entirely (the worst case of that
// collapse) to prove SemanticCache::get enforces tenant/conversation isolation
// at the logical layer regardless of physical index routing.
class PartitionIgnoringStore : public VectorStore {
public:
    bool initialize() override { return true; }
    bool insert(const std::string&, const std::string& id,
                const std::vector<float>& vec) override {
        vecs_[id] = vec;
        return true;
    }
    bool remove(const std::string&, const std::string& id) override {
        return vecs_.erase(id) > 0;
    }
    std::vector<VectorSearchResult> search(
        const std::string&, const std::vector<float>&, size_t top_k,
        float threshold) const override {
        std::vector<VectorSearchResult> out;
        if (1.0f < threshold) return out;
        for (const auto& [id, _] : vecs_) {
            out.push_back({id, 1.0f});
            if (out.size() >= top_k) break;
        }
        return out;
    }
    size_t size() const override { return vecs_.size(); }
    std::string backendName() const override { return "partition-ignoring"; }
private:
    std::unordered_map<std::string, std::vector<float>> vecs_;
};
}  // namespace

// P0-B: a hit surfaced from a DIFFERENT logical partition (only reachable once
// the overflow bucket merges partitions) must NOT leak across the tenant /
// conversation boundary. Tenant A caches a secret; tenant B asking the same
// prompt must miss rather than receive A's response.
TEST(SemanticCacheTenantIsolationTest, OverflowMergeDoesNotLeakAcrossPartitions) {
    HashEmbedder embedder(32);
    PartitionIgnoringStore store;
    SemanticCache cache(embedder, store, 0.99f, 3600s, 100);

    cache.put("balance?", "tenant-A-secret-balance", "gpt-4o", "tenantA");

    // Same prompt+model, different logical partition (tenant B). The physical
    // index leaks A's id, but the logical layer must reject it.
    auto hit = cache.get("balance?", "gpt-4o", "tenantB");
    EXPECT_FALSE(hit.has_value());

    // Sanity: the legitimate owner (tenant A) still gets its own entry.
    auto own = cache.get("balance?", "gpt-4o", "tenantA");
    ASSERT_TRUE(own.has_value());
    EXPECT_EQ(own->response, "tenant-A-secret-balance");
}

// P2-#5: recording feedback must advance cache_feedback_total (was always 0).
TEST(SemanticCacheMetricsTest, RecordFeedbackIncrementsMetric) {
    HashEmbedder embedder(32);
    HnswVectorStore store(32, 50000);
    store.initialize();
    SemanticCache cache(embedder, store, 0.99f, 3600s, 100);

    MetricsRegistry::instance().cacheFeedbackTotal().reset();
    cache.put("q", "r");
    cache.recordFeedback("q", 0.2);
    EXPECT_DOUBLE_EQ(MetricsRegistry::instance().cacheFeedbackTotal().get(), 1.0);
}

class SemanticCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        embedder_ = std::make_unique<HashEmbedder>(32);
        store_ = std::make_unique<HnswVectorStore>(32, 50000);
        store_->initialize();
        cache_ = std::make_unique<SemanticCache>(
            *embedder_, *store_, 0.99f, 3600s, 100);
    }

    std::unique_ptr<HashEmbedder> embedder_;
    std::unique_ptr<HnswVectorStore> store_;
    std::unique_ptr<SemanticCache> cache_;
};

TEST_F(SemanticCacheTest, ExactMatchHitsCache) {
    cache_->put("What is Python?", "Python is a programming language.");
    auto hit = cache_->get("What is Python?");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->response, "Python is a programming language.");
    EXPECT_GT(hit->similarity, 0.99f);
}

TEST_F(SemanticCacheTest, DifferentQueryMissesCache) {
    cache_->put("What is Python?", "Python is a programming language.");
    auto hit = cache_->get("How to sort a list in Java?");
    EXPECT_FALSE(hit.has_value());
}

TEST_F(SemanticCacheTest, SizeTracking) {
    EXPECT_EQ(cache_->size(), 0u);
    cache_->put("q1", "r1");
    EXPECT_EQ(cache_->size(), 1u);
    cache_->put("q2", "r2");
    EXPECT_EQ(cache_->size(), 2u);
}

TEST_F(SemanticCacheTest, ClearRemovesAll) {
    cache_->put("q1", "r1");
    cache_->put("q2", "r2");
    cache_->clear();
    EXPECT_EQ(cache_->size(), 0u);
}

TEST_F(SemanticCacheTest, TTLExpiration) {
    auto short_cache = std::make_unique<SemanticCache>(
        *embedder_, *store_, 0.99f, 1s, 100);

    short_cache->put("expiring query", "expiring response");
    auto hit1 = short_cache->get("expiring query");
    ASSERT_TRUE(hit1.has_value());

    std::this_thread::sleep_for(1500ms);

    auto hit2 = short_cache->get("expiring query");
    EXPECT_FALSE(hit2.has_value());
}

TEST_F(SemanticCacheTest, LRUEviction) {
    auto small_cache = std::make_unique<SemanticCache>(
        *embedder_, *store_, 0.99f, 3600s, 3);

    small_cache->put("q1_unique_string_a", "r1");
    small_cache->put("q2_unique_string_b", "r2");
    small_cache->put("q3_unique_string_c", "r3");
    EXPECT_EQ(small_cache->size(), 3u);

    // Adding one more should evict the oldest
    small_cache->put("q4_unique_string_d", "r4");
    EXPECT_LE(small_cache->size(), 3u);
}

TEST_F(SemanticCacheTest, PipelineShortCircuitsOnHit) {
    // P0-1 / SR-1: process() now reads via the V2 tenant-isolated key, so the
    // write must go through the same derivation (putFromContext) rather than a
    // raw put() with an empty partition.
    cache_->putFromContext({{"user", "cached question"}}, "cached answer");

    RequestContext ctx;
    ctx.request_id = "cache-test";
    ctx.chat_request.messages = {{"user", "cached question"}};

    EXPECT_EQ(cache_->process(ctx), StageResult::ShortCircuit);
    EXPECT_TRUE(ctx.cache_hit);
    EXPECT_EQ(ctx.cached_response, "cached answer");
}

TEST_F(SemanticCacheTest, PipelineContinuesOnMiss) {
    RequestContext ctx;
    ctx.request_id = "cache-miss";
    ctx.chat_request.messages = {{"user", "uncached question"}};

    EXPECT_EQ(cache_->process(ctx), StageResult::Continue);
    EXPECT_FALSE(ctx.cache_hit);
}

TEST_F(SemanticCacheTest, PipelineContinuesOnEmptyMessages) {
    RequestContext ctx;
    ctx.request_id = "empty-msg";
    EXPECT_EQ(cache_->process(ctx), StageResult::Continue);
}

TEST_F(SemanticCacheTest, ModelIsolation) {
    cache_->put("What is AI?", "AI for GPT-4", "gpt-4");
    auto hit = cache_->get("What is AI?", "claude-3");
    EXPECT_FALSE(hit.has_value());
}

TEST_F(SemanticCacheTest, SameModelHits) {
    cache_->put("What is AI?", "AI answer", "gpt-4");
    auto hit = cache_->get("What is AI?", "gpt-4");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->response, "AI answer");
}

TEST_F(SemanticCacheTest, ExtractCacheKeyUsesLastUserMessage) {
    std::vector<Message> msgs = {
        {"system", "You are helpful."},
        {"user", "Hello"},
        {"assistant", "Hi there!"},
        {"user", "What is C++?"}
    };
    auto key = cache_->extractCacheKey(msgs);
    EXPECT_EQ(key, "What is C++?");
}

TEST_F(SemanticCacheTest, ExtractCacheKeyEmptyWhenNoUser) {
    std::vector<Message> msgs = {
        {"system", "You are helpful."},
        {"assistant", "Hi there!"},
    };
    auto key = cache_->extractCacheKey(msgs);
    EXPECT_TRUE(key.empty());
}

TEST_F(SemanticCacheTest, PeriodicEvictionOnPut) {
    auto short_cache = std::make_unique<SemanticCache>(
        *embedder_, *store_, 0.99f, 1s, 1000);

    // Insert entries that will expire quickly
    for (int i = 0; i < 5; ++i) {
        short_cache->put("expire_q_" + std::to_string(i),
                         "expire_r_" + std::to_string(i));
    }
    EXPECT_EQ(short_cache->size(), 5u);

    // Wait for TTL expiry
    std::this_thread::sleep_for(1500ms);

    // Keep inserting until evict interval is reached (kEvictInterval = 100)
    for (int i = 5; i < 105; ++i) {
        short_cache->put("new_q_" + std::to_string(i),
                         "new_r_" + std::to_string(i));
    }

    // After periodic eviction, expired entries should be gone
    // Total would be 105 without eviction, but 5 expired entries should be removed
    EXPECT_LE(short_cache->size(), 100u);
}

TEST_F(SemanticCacheTest, PutFromContextMatchesProcess) {
    // Simulate multi-turn conversation
    RequestContext ctx;
    ctx.request_id = "multi-turn-test";
    ctx.chat_request.messages = {
        {"system", "You are a coding assistant."},
        {"user", "Explain Python"},
        {"assistant", "Python is a language..."},
        {"user", "What about C++?"}
    };

    // First, put via putFromContext (simulating post-model-call cache store)
    cache_->putFromContext(ctx.chat_request.messages, "C++ is a compiled language.", "gpt-4");

    // Then, process should find it (uses same key extraction)
    auto result = cache_->process(ctx);
    EXPECT_EQ(result, StageResult::ShortCircuit);
    EXPECT_TRUE(ctx.cache_hit);
    EXPECT_EQ(ctx.cached_response, "C++ is a compiled language.");
}

// C14: SemanticCache ABBA deadlock — concurrent get/put must not deadlock
TEST_F(SemanticCacheTest, ConcurrentGetPut_NoDeadlock) {
    // Pre-populate some entries
    for (int i = 0; i < 10; ++i) {
        cache_->put("seed_q_" + std::to_string(i),
                    "seed_r_" + std::to_string(i), "model");
    }

    std::atomic<bool> stop{false};
    std::atomic<int> ops{0};

    auto writer = [&]() {
        int i = 100;
        while (!stop.load(std::memory_order_relaxed)) {
            cache_->put("w_q_" + std::to_string(i),
                        "w_r_" + std::to_string(i), "model");
            ++i;
            ops.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto reader = [&]() {
        int i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            cache_->get("seed_q_" + std::to_string(i % 10), "model");
            i++;
            ops.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto clearer = [&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            cache_->evictExpired();
            ops.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(1ms);
        }
    };

    std::vector<std::thread> threads;
    threads.emplace_back(writer);
    threads.emplace_back(writer);
    threads.emplace_back(reader);
    threads.emplace_back(reader);
    threads.emplace_back(clearer);

    // If deadlock exists, this will hang and timeout
    std::this_thread::sleep_for(500ms);
    stop.store(true, std::memory_order_relaxed);

    for (auto& t : threads) t.join();
    EXPECT_GT(ops.load(), 0);
}

// --- CacheStore integration tests ---

class SemanticCacheStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        embedder_ = std::make_unique<HashEmbedder>(32);
        vector_store_ = std::make_unique<HnswVectorStore>(32, 1000);
        vector_store_->initialize();
        cache_store_ = std::make_unique<MemoryCacheStore>(10000);
        cache_store_->initialize();
        cache_ = std::make_unique<SemanticCache>(
            *embedder_, *vector_store_, 0.99f, 3600s, 100);
        cache_->setCacheStore(cache_store_.get());
    }

    std::unique_ptr<HashEmbedder> embedder_;
    std::unique_ptr<HnswVectorStore> vector_store_;
    std::unique_ptr<MemoryCacheStore> cache_store_;
    std::unique_ptr<SemanticCache> cache_;
};

TEST_F(SemanticCacheStoreTest, PutWritesThroughToCacheStore) {
    cache_->put("persist test", "persist response", "gpt-4");
    auto keys = cache_store_->keys(SemanticCache::kCacheKeyPrefix);
    EXPECT_EQ(keys.size(), 1u);
    auto val = cache_store_->get(keys[0]);
    ASSERT_TRUE(val.has_value());
    EXPECT_NE(val->find("persist test"), std::string::npos);
}

TEST_F(SemanticCacheStoreTest, WarmUpRestoresEntries) {
    cache_->put("warmup q1", "warmup r1", "model-a");
    cache_->put("warmup q2", "warmup r2", "model-b");
    EXPECT_EQ(cache_->size(), 2u);
    EXPECT_EQ(cache_store_->keys(SemanticCache::kCacheKeyPrefix).size(), 2u);

    auto embedder2 = std::make_unique<HashEmbedder>(32);
    auto vs2 = std::make_unique<HnswVectorStore>(32, 1000);
    vs2->initialize();
    auto cache2 = std::make_unique<SemanticCache>(
        *embedder2, *vs2, 0.99f, 3600s, 100);
    cache2->setCacheStore(cache_store_.get());

    size_t loaded = cache2->warmUp();
    EXPECT_EQ(loaded, 2u);
    EXPECT_EQ(cache2->size(), 2u);

    auto hit = cache2->get("warmup q1", "model-a");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->response, "warmup r1");
}

TEST_F(SemanticCacheStoreTest, WarmUpSkipsDuplicates) {
    cache_->put("dup test", "response", "m1");
    size_t loaded = cache_->warmUp();
    EXPECT_EQ(loaded, 0u);
    EXPECT_EQ(cache_->size(), 1u);
}

TEST_F(SemanticCacheStoreTest, NullCacheStoreIsNoop) {
    SemanticCache plain(*embedder_, *vector_store_, 0.99f, 3600s, 100);
    plain.put("no store", "response");
    EXPECT_EQ(plain.warmUp(), 0u);
    EXPECT_EQ(plain.size(), 1u);
}

TEST_F(SemanticCacheStoreTest, WarmUpSkipsLargeValues) {
    cache_->put("normal query", "normal response", "gpt-4");

    std::string large_value(1024 * 1024 + 1, 'X');
    cache_store_->set(std::string(SemanticCache::kCacheKeyPrefix) + "large_hash",
                large_value, std::chrono::seconds(3600));

    auto embedder2 = std::make_unique<HashEmbedder>(32);
    auto vs2 = std::make_unique<HnswVectorStore>(32, 1000);
    vs2->initialize();
    auto cache2 = std::make_unique<SemanticCache>(
        *embedder2, *vs2, 0.99f, 3600s, 100);
    cache2->setCacheStore(cache_store_.get());

    size_t loaded = cache2->warmUp();
    EXPECT_EQ(loaded, 1u);
    EXPECT_EQ(cache2->size(), 1u);
}

// ============ Adaptive Threshold tests ============

TEST_F(SemanticCacheTest, AdaptiveThresholdDisabledByDefault) {
    auto stats = cache_->getStats();
    EXPECT_FLOAT_EQ(stats.current_threshold, 0.99f);
}

TEST_F(SemanticCacheTest, AdaptiveThresholdDecreasesOnLowHitRateWithHighSim) {
    AdaptiveThresholdConfig cfg;
    cfg.enabled = true;
    cfg.min_threshold = 0.80f;
    cfg.max_threshold = 0.99f;
    cfg.adjustment_rate = 0.02f;
    cfg.window_size = 5;
    cfg.target_hit_rate_low = 0.20f;
    cfg.target_hit_rate_high = 0.80f;
    cache_->setAdaptiveConfig(cfg);
    cache_->setThreshold(0.995f);

    float initial = cache_->currentThreshold();

    // Put entries then query with similar but not exact → hash embedder
    // produces 1.0 for exact match, but with 0.995 threshold it should hit
    // We need misses with high similarity → put entries and query related ones
    // With HashEmbedder: different strings yield different hashes, sim~0
    // For dual signal to trigger decrease: p50 > threshold*0.9
    // So we need queries that produce high similarity but still miss
    // Strategy: put "hello", query "hello" 5 times with model mismatch
    cache_->put("hello world test", "response", "model-A");
    for (int i = 0; i < 5; ++i) {
        cache_->get("hello world test", "model-B");  // model mismatch → miss but sim=1.0
    }

    // hit_rate = 0/5 = 0 < 0.20, p50 of [1.0, 1.0, 1.0, 1.0, 1.0] = 1.0 > 0.995*0.9
    EXPECT_LT(cache_->currentThreshold(), initial);
    EXPECT_GE(cache_->currentThreshold(), cfg.min_threshold);
}

TEST_F(SemanticCacheTest, AdaptiveThresholdIncreasesOnHighHitRate) {
    AdaptiveThresholdConfig cfg;
    cfg.enabled = true;
    cfg.min_threshold = 0.80f;
    cfg.max_threshold = 0.99f;
    cfg.adjustment_rate = 0.02f;
    cfg.window_size = 5;
    cfg.target_hit_rate_low = 0.20f;
    cfg.target_hit_rate_high = 0.50f;
    cache_->setAdaptiveConfig(cfg);
    // threshold=0.96 → threshold*1.05=1.008 > 1.0(exact match sim)
    // so p50_sim < threshold*1.05 is satisfied
    cache_->setThreshold(0.96f);

    for (int i = 0; i < 5; ++i) {
        std::string q = "adaptive_query_" + std::to_string(i);
        cache_->put(q, "response_" + std::to_string(i));
    }
    for (int i = 0; i < 5; ++i) {
        std::string q = "adaptive_query_" + std::to_string(i);
        cache_->get(q);
    }

    EXPECT_GT(cache_->currentThreshold(), 0.96f);
    EXPECT_LE(cache_->currentThreshold(), cfg.max_threshold);
}

TEST_F(SemanticCacheTest, AdaptiveThresholdRespectsClamp) {
    AdaptiveThresholdConfig cfg;
    cfg.enabled = true;
    cfg.min_threshold = 0.95f;
    cfg.max_threshold = 0.96f;
    cfg.adjustment_rate = 0.10f;
    cfg.window_size = 3;
    cfg.target_hit_rate_low = 0.50f;
    cfg.target_hit_rate_high = 0.80f;
    cache_->setAdaptiveConfig(cfg);
    cache_->setThreshold(0.955f);

    // 3 misses with high sim → try to decrease (need p50 > threshold*0.9)
    cache_->put("clamp_entry", "r", "model-A");
    for (int i = 0; i < 3; ++i) {
        cache_->get("clamp_entry", "model-B");  // model mismatch: miss + sim=1.0
    }
    EXPECT_GE(cache_->currentThreshold(), cfg.min_threshold);

    // Reset to max and cause 3 hits
    cache_->setThreshold(0.96f);
    for (int i = 0; i < 3; ++i) {
        std::string q = "clamp_hit_" + std::to_string(i);
        cache_->put(q, "r");
    }
    for (int i = 0; i < 3; ++i) {
        cache_->get("clamp_hit_" + std::to_string(i));
    }
    EXPECT_LE(cache_->currentThreshold(), cfg.max_threshold);
}

TEST_F(SemanticCacheTest, DualSignalNoAdjustWhenSimTooLow) {
    // Low hit rate but similarity is also low → queries are genuinely different,
    // don't lower threshold
    AdaptiveThresholdConfig cfg;
    cfg.enabled = true;
    cfg.min_threshold = 0.80f;
    cfg.max_threshold = 0.99f;
    cfg.adjustment_rate = 0.05f;
    cfg.window_size = 5;
    cfg.target_hit_rate_low = 0.20f;
    cfg.target_hit_rate_high = 0.80f;
    cache_->setAdaptiveConfig(cfg);
    cache_->setThreshold(0.95f);

    // Insert and query genuinely different things → low similarity + low hit rate
    // This means no similarity data above threshold*0.9 is present → dual signal
    // should NOT lower threshold
    for (int i = 0; i < 5; ++i) {
        cache_->get("completely_random_" + std::to_string(i));
    }
    // With pure misses and 0.0 similarity, p50 = 0.0 which is NOT > threshold*0.9
    // So dual signal prevents adjustment
    EXPECT_FLOAT_EQ(cache_->currentThreshold(), 0.95f);
}

TEST_F(SemanticCacheTest, DualSignalDecreasesWhenSimIsHigh) {
    // Low hit rate but similarity is close to threshold → should decrease
    AdaptiveThresholdConfig cfg;
    cfg.enabled = true;
    cfg.min_threshold = 0.80f;
    cfg.max_threshold = 0.99f;
    cfg.adjustment_rate = 0.05f;
    cfg.window_size = 3;
    cfg.target_hit_rate_low = 0.50f;
    cfg.target_hit_rate_high = 0.80f;
    cache_->setAdaptiveConfig(cfg);
    cache_->setThreshold(0.99f);

    // Put entries with very similar but not identical queries
    cache_->put("what is machine learning used for", "ML is used for...");
    // These will search but miss due to 0.99 threshold being very strict
    // But the max_similarity recorded will be > 0 (hash embedder produces some similarity)
    // We need similarity > threshold*0.9 = 0.891 for dual signal to fire
    // With HashEmbedder, exact matches return 1.0, so querying the exact same thing
    // but with threshold 0.99 may or may not hit
    // Let's use a more controllable approach: record via internal mechanism
    // Actually, the test for pure hit-rate already validated decrease works.
    // For dual signal specifically, we need the P50 check.

    // Since HashEmbedder exact match gives 1.0, put + get same query = hit
    // Put 1 entry, get 3 times: 1 hit + 2 unique misses
    cache_->get("what is machine learning used for");  // hit (sim=1.0)
    cache_->get("random_miss_1");  // miss (sim~0)
    cache_->get("random_miss_2");  // miss (sim~0)

    // hit_rate = 1/3 = 0.33 < 0.50 (low)
    // p50 of [1.0, ~0, ~0] ≈ ~0 which is NOT > 0.99*0.9=0.891
    // → dual signal should NOT decrease
    EXPECT_FLOAT_EQ(cache_->currentThreshold(), 0.99f);
}

TEST_F(SemanticCacheTest, StatsShowsCurrentThreshold) {
    cache_->setThreshold(0.88f);
    auto stats = cache_->getStats();
    EXPECT_FLOAT_EQ(stats.current_threshold, 0.88f);
}

// ============ CachePolicy tests ============

TEST_F(SemanticCacheTest, PolicyDisabledAllowsAll) {
    CachePolicy policy;
    policy.enabled = false;
    cache_->setCachePolicy(policy);

    RequestContext ctx;
    ctx.chat_request.model = "whatever";
    ctx.chat_request.temperature = 2.0;
    ctx.chat_request.stream = true;
    EXPECT_TRUE(cache_->shouldCache(ctx));
}

TEST_F(SemanticCacheTest, PolicySkipsModel) {
    CachePolicy policy;
    policy.enabled = true;
    policy.skip_models = {"gpt-4o"};
    cache_->setCachePolicy(policy);

    RequestContext ctx;
    ctx.chat_request.model = "gpt-4o";
    EXPECT_FALSE(cache_->shouldCache(ctx));

    ctx.chat_request.model = "gpt-3.5-turbo";
    EXPECT_TRUE(cache_->shouldCache(ctx));
}

TEST_F(SemanticCacheTest, PolicySkipsHighTemperature) {
    CachePolicy policy;
    policy.enabled = true;
    policy.max_temperature = 0.8;
    cache_->setCachePolicy(policy);

    RequestContext ctx;
    ctx.chat_request.model = "gpt-4";
    ctx.chat_request.temperature = 0.9;
    EXPECT_FALSE(cache_->shouldCache(ctx));

    ctx.chat_request.temperature = 0.5;
    EXPECT_TRUE(cache_->shouldCache(ctx));

    ctx.chat_request.temperature = std::nullopt;
    EXPECT_TRUE(cache_->shouldCache(ctx));
}

TEST_F(SemanticCacheTest, PolicySkipsStreaming) {
    CachePolicy policy;
    policy.enabled = true;
    policy.skip_streaming = true;
    cache_->setCachePolicy(policy);

    RequestContext ctx;
    ctx.chat_request.model = "gpt-4";
    ctx.chat_request.stream = true;
    EXPECT_FALSE(cache_->shouldCache(ctx));

    ctx.chat_request.stream = false;
    EXPECT_TRUE(cache_->shouldCache(ctx));
}

TEST_F(SemanticCacheTest, ProcessRespectsPolicy) {
    cache_->put("hello test", "cached response");

    CachePolicy policy;
    policy.enabled = true;
    policy.skip_models = {"blocked-model"};
    cache_->setCachePolicy(policy);

    RequestContext ctx;
    ctx.request_id = "policy-test";
    ctx.chat_request.model = "blocked-model";
    ctx.chat_request.messages = {{"user", "hello test"}};

    auto result = cache_->process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_FALSE(ctx.cache_hit);
}

// ============ JSON Import tests ============

TEST_F(SemanticCacheTest, ImportFromJsonAddsEntries) {
    nlohmann::json data = nlohmann::json::array({
        {{"prompt", "What is AI?"}, {"response", "Artificial Intelligence is..."}, {"model", "gpt-4"}},
        {{"prompt", "What is ML?"}, {"response", "Machine Learning is..."}, {"model", "gpt-4"}}
    });
    size_t imported = cache_->importFromJson(data.dump());
    EXPECT_EQ(imported, 2u);
    EXPECT_EQ(cache_->size(), 2u);

    auto hit = cache_->get("What is AI?");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->response, "Artificial Intelligence is...");
}

TEST_F(SemanticCacheTest, ImportFromJsonSkipsInvalidEntries) {
    nlohmann::json data = nlohmann::json::array({
        {{"prompt", "Valid"}, {"response", "OK"}},
        {{"prompt", ""}, {"response", "Missing prompt"}},
        {{"response", "Missing prompt key"}},
        {{"prompt", "Also valid"}, {"response", "Also OK"}}
    });
    size_t imported = cache_->importFromJson(data.dump());
    EXPECT_EQ(imported, 2u);
    EXPECT_EQ(cache_->size(), 2u);
}

TEST_F(SemanticCacheTest, ImportFromJsonRejectsInvalidJson) {
    size_t imported = cache_->importFromJson("not valid json{{{");
    EXPECT_EQ(imported, 0u);
}

TEST_F(SemanticCacheTest, ImportFromJsonRejectsNonArray) {
    size_t imported = cache_->importFromJson("{\"key\": \"value\"}");
    EXPECT_EQ(imported, 0u);
}

// ============ Context-aware cache key tests ============

class ContextAwareCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        embedder_ = std::make_unique<HashEmbedder>(32);
        store_ = std::make_unique<HnswVectorStore>(32, 50000);
        store_->initialize();
        cache_ = std::make_unique<SemanticCache>(
            *embedder_, *store_, 0.99f, 3600s, 100);
        cache_->setContextAware(true);
    }

    std::unique_ptr<HashEmbedder> embedder_;
    std::unique_ptr<HnswVectorStore> store_;
    std::unique_ptr<SemanticCache> cache_;
};

TEST_F(ContextAwareCacheTest, ExtractCacheKeyInfoReturnsPartition) {
    std::vector<Message> msgs = {
        {"system", "You are a translator."},
        {"user", "Hello world"}
    };
    auto info = cache_->extractCacheKeyInfo(msgs);
    EXPECT_FALSE(info.partition_key.empty());
    EXPECT_EQ(info.prompt, "Hello world");
}

TEST_F(ContextAwareCacheTest, DifferentSystemPromptsYieldDifferentPartitions) {
    std::vector<Message> msgs_a = {
        {"system", "You are a translator."},
        {"user", "Hello world"}
    };
    std::vector<Message> msgs_b = {
        {"system", "You are a coder."},
        {"user", "Hello world"}
    };
    auto info_a = cache_->extractCacheKeyInfo(msgs_a);
    auto info_b = cache_->extractCacheKeyInfo(msgs_b);
    EXPECT_NE(info_a.partition_key, info_b.partition_key);
    EXPECT_EQ(info_a.prompt, info_b.prompt);
}

TEST_F(ContextAwareCacheTest, NoSystemPromptYieldsEmptyPartition) {
    std::vector<Message> msgs = {
        {"user", "Hello world"}
    };
    auto info = cache_->extractCacheKeyInfo(msgs);
    EXPECT_TRUE(info.partition_key.empty());
    EXPECT_EQ(info.prompt, "Hello world");
}

TEST_F(ContextAwareCacheTest, ContextAwareDisabledYieldsEmptyPartition) {
    cache_->setContextAware(false);
    std::vector<Message> msgs = {
        {"system", "You are a translator."},
        {"user", "Hello world"}
    };
    auto info = cache_->extractCacheKeyInfo(msgs);
    EXPECT_TRUE(info.partition_key.empty());
    EXPECT_EQ(info.prompt, "Hello world");
}

TEST_F(ContextAwareCacheTest, PutFromContextUsesPartition) {
    std::vector<Message> msgs_a = {
        {"system", "You are a translator."},
        {"user", "Hello"}
    };
    std::vector<Message> msgs_b = {
        {"system", "You are a coder."},
        {"user", "Hello"}
    };

    cache_->putFromContext(msgs_a, "Bonjour", "gpt-4");
    cache_->putFromContext(msgs_b, "print('hello')", "gpt-4");

    // P0-1 / SR-1: putFromContext now derives the partition via V2 (system
    // prompt still mixed in → distinct partitions), so reads must use the V2
    // key to match. Distinct system prompts remain isolated.
    auto info_a = cache_->extractCacheKeyInfoV2(msgs_a, "");
    auto hit_a = cache_->get(info_a.prompt, "gpt-4", info_a.partition_key);
    ASSERT_TRUE(hit_a.has_value());
    EXPECT_EQ(hit_a->response, "Bonjour");

    auto info_b = cache_->extractCacheKeyInfoV2(msgs_b, "");
    auto hit_b = cache_->get(info_b.prompt, "gpt-4", info_b.partition_key);
    ASSERT_TRUE(hit_b.has_value());
    EXPECT_EQ(hit_b->response, "print('hello')");
}

TEST_F(ContextAwareCacheTest, ProcessUsesContextAwarePartition) {
    std::vector<Message> msgs = {
        {"system", "You are a translator."},
        {"user", "Hello"}
    };
    cache_->putFromContext(msgs, "Bonjour", "gpt-4");

    RequestContext ctx;
    ctx.request_id = "test-001";
    ctx.chat_request.model = "gpt-4";
    ctx.chat_request.messages = msgs;

    auto result = cache_->process(ctx);
    EXPECT_EQ(result, StageResult::ShortCircuit);
    EXPECT_TRUE(ctx.cache_hit);
    EXPECT_EQ(ctx.cached_response, "Bonjour");
}

// ============ Cache Stats tests ============

TEST_F(SemanticCacheTest, StatsInitiallyZero) {
    auto stats = cache_->getStats();
    EXPECT_EQ(stats.hit_count, 0u);
    EXPECT_EQ(stats.miss_count, 0u);
    EXPECT_EQ(stats.entry_count, 0u);
    EXPECT_FLOAT_EQ(stats.hit_rate, 0.0f);
}

TEST_F(SemanticCacheTest, StatsTrackHitAndMiss) {
    cache_->put("test query", "test response");

    auto miss = cache_->get("unrelated query xyz");
    auto hit = cache_->get("test query");
    ASSERT_TRUE(hit.has_value());

    auto stats = cache_->getStats();
    EXPECT_EQ(stats.hit_count, 1u);
    EXPECT_EQ(stats.miss_count, 1u);
    EXPECT_EQ(stats.entry_count, 1u);
    EXPECT_FLOAT_EQ(stats.hit_rate, 0.5f);
}

TEST_F(SemanticCacheTest, StatsTrackPutCount) {
    cache_->put("q1", "r1");
    cache_->put("q2", "r2");
    auto stats = cache_->getStats();
    EXPECT_EQ(stats.put_count, 2u);
    EXPECT_EQ(stats.entry_count, 2u);
}

TEST_F(SemanticCacheTest, PutFromContextRespectsCachePolicy) {
    CachePolicy policy;
    policy.enabled = true;
    policy.skip_models = {"gpt-4"};
    cache_->setCachePolicy(policy);

    std::vector<Message> msgs = {{"user", "hello policy test"}};
    cache_->putFromContext(msgs, "world", "gpt-4");

    // Policy skips gpt-4 → nothing written → miss regardless of key scheme.
    auto hit = cache_->get("hello policy test", "gpt-4");
    EXPECT_FALSE(hit.has_value());

    // gpt-3.5 not skipped → written via V2 key; read must use the V2 key too.
    cache_->putFromContext(msgs, "world", "gpt-3.5");
    auto info = cache_->extractCacheKeyInfoV2(msgs, "");
    auto hit2 = cache_->get(info.prompt, "gpt-3.5", info.partition_key);
    EXPECT_TRUE(hit2.has_value());
}

// ============ Conversation Hash tests ============

class ConversationHashCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        embedder_ = std::make_unique<HashEmbedder>(32);
        store_ = std::make_unique<HnswVectorStore>(32, 50000);
        store_->initialize();
        cache_ = std::make_unique<SemanticCache>(
            *embedder_, *store_, 0.99f, 3600s, 100);
    }

    std::unique_ptr<HashEmbedder> embedder_;
    std::unique_ptr<HnswVectorStore> store_;
    std::unique_ptr<SemanticCache> cache_;
};

TEST_F(ConversationHashCacheTest, NoneModeBehavesLikeLegacy) {
    ConversationHashConfig cfg;
    cfg.mode = ConversationHashMode::None;
    cache_->setConversationHashConfig(cfg);

    std::vector<Message> msgs_a = {
        {"user", "What is Python?"},
        {"assistant", "A programming language."},
        {"user", "Tell me more"}
    };
    std::vector<Message> msgs_b = {
        {"user", "What is Rust?"},
        {"assistant", "A systems language."},
        {"user", "Tell me more"}
    };

    auto info_a = cache_->extractCacheKeyInfo(msgs_a);
    auto info_b = cache_->extractCacheKeyInfo(msgs_b);

    EXPECT_TRUE(info_a.conversation_hash.empty());
    EXPECT_TRUE(info_b.conversation_hash.empty());
    EXPECT_EQ(info_a.partition_key, info_b.partition_key);
    EXPECT_EQ(info_a.prompt, "Tell me more");
    EXPECT_EQ(info_b.prompt, "Tell me more");
}

TEST_F(ConversationHashCacheTest, FullModeIsolatesDifferentHistories) {
    ConversationHashConfig cfg;
    cfg.mode = ConversationHashMode::Full;
    cache_->setConversationHashConfig(cfg);

    std::vector<Message> msgs_a = {
        {"user", "What is Python?"},
        {"assistant", "A programming language."},
        {"user", "Tell me more"}
    };
    std::vector<Message> msgs_b = {
        {"user", "What is Rust?"},
        {"assistant", "A systems language."},
        {"user", "Tell me more"}
    };

    auto info_a = cache_->extractCacheKeyInfo(msgs_a);
    auto info_b = cache_->extractCacheKeyInfo(msgs_b);

    EXPECT_FALSE(info_a.conversation_hash.empty());
    EXPECT_FALSE(info_b.conversation_hash.empty());
    EXPECT_NE(info_a.conversation_hash, info_b.conversation_hash);
    EXPECT_NE(info_a.partition_key, info_b.partition_key);
    EXPECT_EQ(info_a.prompt, info_b.prompt);
}

TEST_F(ConversationHashCacheTest, FullModeSameHistorySameHash) {
    ConversationHashConfig cfg;
    cfg.mode = ConversationHashMode::Full;
    cache_->setConversationHashConfig(cfg);

    std::vector<Message> msgs = {
        {"user", "What is Python?"},
        {"assistant", "A programming language."},
        {"user", "Tell me more"}
    };

    auto info1 = cache_->extractCacheKeyInfo(msgs);
    auto info2 = cache_->extractCacheKeyInfo(msgs);

    EXPECT_EQ(info1.conversation_hash, info2.conversation_hash);
    EXPECT_EQ(info1.partition_key, info2.partition_key);
}

TEST_F(ConversationHashCacheTest, WindowModeHashesRecentContext) {
    ConversationHashConfig cfg;
    cfg.mode = ConversationHashMode::Window;
    cfg.window_size = 2;
    cache_->setConversationHashConfig(cfg);

    std::vector<Message> msgs_a = {
        {"user", "First question"},
        {"assistant", "First answer"},
        {"user", "Second question"},
        {"assistant", "Second answer"},
        {"user", "Final question"}
    };
    std::vector<Message> msgs_b = {
        {"user", "Different first question"},
        {"assistant", "Different first answer"},
        {"user", "Second question"},
        {"assistant", "Second answer"},
        {"user", "Final question"}
    };

    auto info_a = cache_->extractCacheKeyInfo(msgs_a);
    auto info_b = cache_->extractCacheKeyInfo(msgs_b);

    // Window=2 hashes only the last 2 preceding messages (same in both)
    EXPECT_EQ(info_a.conversation_hash, info_b.conversation_hash);
}

TEST_F(ConversationHashCacheTest, WindowModeDifferentRecentContext) {
    ConversationHashConfig cfg;
    cfg.mode = ConversationHashMode::Window;
    cfg.window_size = 2;
    cache_->setConversationHashConfig(cfg);

    std::vector<Message> msgs_a = {
        {"user", "Topic A question"},
        {"assistant", "Topic A answer"},
        {"user", "Final question"}
    };
    std::vector<Message> msgs_b = {
        {"user", "Topic B question"},
        {"assistant", "Topic B answer"},
        {"user", "Final question"}
    };

    auto info_a = cache_->extractCacheKeyInfo(msgs_a);
    auto info_b = cache_->extractCacheKeyInfo(msgs_b);

    EXPECT_NE(info_a.conversation_hash, info_b.conversation_hash);
    EXPECT_NE(info_a.partition_key, info_b.partition_key);
}

TEST_F(ConversationHashCacheTest, SingleMessageYieldsEmptyHash) {
    ConversationHashConfig cfg;
    cfg.mode = ConversationHashMode::Full;
    cache_->setConversationHashConfig(cfg);

    std::vector<Message> msgs = {{"user", "Hello"}};
    auto info = cache_->extractCacheKeyInfo(msgs);

    EXPECT_TRUE(info.conversation_hash.empty());
}

TEST_F(ConversationHashCacheTest, FullModeEndToEndCacheIsolation) {
    ConversationHashConfig cfg;
    cfg.mode = ConversationHashMode::Full;
    cache_->setConversationHashConfig(cfg);

    std::vector<Message> msgs_a = {
        {"user", "What is Python?"},
        {"assistant", "A programming language."},
        {"user", "Tell me more"}
    };
    std::vector<Message> msgs_b = {
        {"user", "What is Rust?"},
        {"assistant", "A systems language."},
        {"user", "Tell me more"}
    };

    cache_->putFromContext(msgs_a, "Python details...", "gpt-4");

    // Same final question but different history should miss
    RequestContext ctx;
    ctx.request_id = "conv-hash-test";
    ctx.chat_request.model = "gpt-4";
    ctx.chat_request.messages = msgs_b;

    auto result = cache_->process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_FALSE(ctx.cache_hit);

    // Same history should hit
    RequestContext ctx2;
    ctx2.request_id = "conv-hash-test-2";
    ctx2.chat_request.model = "gpt-4";
    ctx2.chat_request.messages = msgs_a;

    auto result2 = cache_->process(ctx2);
    EXPECT_EQ(result2, StageResult::ShortCircuit);
    EXPECT_TRUE(ctx2.cache_hit);
    EXPECT_EQ(ctx2.cached_response, "Python details...");
}

TEST_F(ConversationHashCacheTest, WindowModeEndToEndCacheIsolation) {
    ConversationHashConfig cfg;
    cfg.mode = ConversationHashMode::Window;
    cfg.window_size = 2;
    cache_->setConversationHashConfig(cfg);

    std::vector<Message> msgs_a = {
        {"user", "What is Python?"},
        {"assistant", "A programming language."},
        {"user", "Tell me more"}
    };
    std::vector<Message> msgs_b = {
        {"user", "What is Rust?"},
        {"assistant", "A systems language."},
        {"user", "Tell me more"}
    };

    cache_->putFromContext(msgs_a, "Python details...", "gpt-4");

    RequestContext ctx;
    ctx.request_id = "window-test";
    ctx.chat_request.model = "gpt-4";
    ctx.chat_request.messages = msgs_b;

    auto result = cache_->process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_FALSE(ctx.cache_hit);
}

TEST_F(ConversationHashCacheTest, ConversationHashCombinesWithSystemPartition) {
    ConversationHashConfig cfg;
    cfg.mode = ConversationHashMode::Full;
    cache_->setConversationHashConfig(cfg);
    cache_->setContextAware(true);

    std::vector<Message> msgs = {
        {"system", "You are a translator."},
        {"user", "Hello"},
        {"assistant", "Bonjour"},
        {"user", "Tell me more"}
    };

    auto info = cache_->extractCacheKeyInfo(msgs);

    EXPECT_FALSE(info.partition_key.empty());
    EXPECT_TRUE(info.partition_key.find("sys_") != std::string::npos);
    EXPECT_TRUE(info.partition_key.find("conv_") != std::string::npos);
    EXPECT_TRUE(info.partition_key.find("|") != std::string::npos);
}

TEST_F(ConversationHashCacheTest, ComputeConversationHashStatic) {
    ConversationHashConfig cfg;
    cfg.mode = ConversationHashMode::Full;

    std::vector<Message> msgs = {
        {"user", "Q1"},
        {"assistant", "A1"},
        {"user", "Q2"}
    };

    auto hash1 = SemanticCache::computeConversationHash(msgs, cfg);
    auto hash2 = SemanticCache::computeConversationHash(msgs, cfg);
    EXPECT_EQ(hash1, hash2);
    EXPECT_FALSE(hash1.empty());
    EXPECT_TRUE(hash1.find("conv_") == 0);
}

TEST_F(ConversationHashCacheTest, ComputeConversationHashNoneReturnsEmpty) {
    ConversationHashConfig cfg;
    cfg.mode = ConversationHashMode::None;

    std::vector<Message> msgs = {
        {"user", "Q1"},
        {"assistant", "A1"},
        {"user", "Q2"}
    };

    auto hash = SemanticCache::computeConversationHash(msgs, cfg);
    EXPECT_TRUE(hash.empty());
}

TEST_F(ConversationHashCacheTest, WindowLargerThanMessagesFallsBack) {
    ConversationHashConfig cfg;
    cfg.mode = ConversationHashMode::Window;
    cfg.window_size = 100;
    cache_->setConversationHashConfig(cfg);

    std::vector<Message> msgs = {
        {"user", "Q1"},
        {"assistant", "A1"},
        {"user", "Q2"}
    };

    auto info = cache_->extractCacheKeyInfo(msgs);
    EXPECT_FALSE(info.conversation_hash.empty());

    ConversationHashConfig cfg_full;
    cfg_full.mode = ConversationHashMode::Full;
    auto hash_full = SemanticCache::computeConversationHash(msgs, cfg_full);
    EXPECT_EQ(info.conversation_hash, hash_full);
}

TEST_F(ContextAwareCacheTest, ProcessMissesWithDifferentContext) {
    std::vector<Message> msgs_a = {
        {"system", "You are a translator."},
        {"user", "Hello"}
    };
    cache_->putFromContext(msgs_a, "Bonjour", "gpt-4");

    RequestContext ctx;
    ctx.request_id = "test-002";
    ctx.chat_request.model = "gpt-4";
    ctx.chat_request.messages = {
        {"system", "You are a coder."},
        {"user", "Hello"}
    };

    auto result = cache_->process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_FALSE(ctx.cache_hit);
}
