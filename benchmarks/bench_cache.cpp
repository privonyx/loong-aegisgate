#include <benchmark/benchmark.h>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "cache/embedder.h"
#include "cache/semantic_cache.h"
#include "cache/hnsw_vector_store.h"
#include "cache/partitioned_vector_index.h"
#include "cache/vector_index.h"

static void BM_HashEmbedder_Embed(benchmark::State& state) {
    aegisgate::HashEmbedder embedder(128);
    const std::string text = "The quick brown fox jumps over the lazy dog.";
    for (auto _ : state) {
        auto v = embedder.embed(text);
        benchmark::DoNotOptimize(v.data());
    }
}
BENCHMARK(BM_HashEmbedder_Embed);

static void BM_CosineSimilarity_128(benchmark::State& state) {
    static aegisgate::HashEmbedder embedder(128);
    static const std::vector<float> a = embedder.embed("cosine_vec_a");
    static const std::vector<float> b = embedder.embed("cosine_vec_b");
    for (auto _ : state) {
        float s = aegisgate::cosineSimilarity(a, b);
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_CosineSimilarity_128);

class VectorIndexInsertFixture : public benchmark::Fixture {
public:
    VectorIndexInsertFixture() : embedder_(128) {
        pool_.resize(1024);
        for (size_t i = 0; i < pool_.size(); ++i) {
            pool_[i] = embedder_.embed("pool_" + std::to_string(i));
        }
    }

    void SetUp(const benchmark::State&) override {
        index_ = std::make_unique<aegisgate::VectorIndex>(128, 50000);
        next_id_ = 0;
    }

    aegisgate::HashEmbedder embedder_;
    std::vector<std::vector<float>> pool_;
    std::unique_ptr<aegisgate::VectorIndex> index_;
    size_t next_id_ = 0;
};

BENCHMARK_DEFINE_F(VectorIndexInsertFixture, Insert)(benchmark::State& state) {
    for (auto _ : state) {
        const std::string id = std::to_string(next_id_);
        index_->insert(id, pool_[next_id_ % pool_.size()]);
        ++next_id_;
        benchmark::DoNotOptimize(index_->size());
    }
}
BENCHMARK_REGISTER_F(VectorIndexInsertFixture, Insert)->Name("BM_VectorIndex_Insert");

class VectorIndexSearchFixture : public benchmark::Fixture {
public:
    VectorIndexSearchFixture() : embedder_(128) {}

    void SetUp(const benchmark::State&) override {
        index_ = std::make_unique<aegisgate::VectorIndex>(128, 2000);
        for (size_t i = 0; i < 1000; ++i) {
            index_->insert(std::to_string(i),
                           embedder_.embed("prefill_" + std::to_string(i)));
        }
        query_ = embedder_.embed("search_query_vector");
    }

    aegisgate::HashEmbedder embedder_;
    std::unique_ptr<aegisgate::VectorIndex> index_;
    std::vector<float> query_;
};

BENCHMARK_DEFINE_F(VectorIndexSearchFixture, Search)(benchmark::State& state) {
    for (auto _ : state) {
        auto results = index_->search(query_, 10, 0.0f);
        benchmark::DoNotOptimize(results);
    }
}
BENCHMARK_REGISTER_F(VectorIndexSearchFixture, Search)->Name("BM_VectorIndex_Search");

class SemanticCachePutFixture : public benchmark::Fixture {
public:
    SemanticCachePutFixture() : embedder_(128) {}

    void SetUp(const benchmark::State&) override {
        store_ = std::make_unique<aegisgate::HnswVectorStore>(128, 100000);
        store_->initialize();
        cache_ = std::make_unique<aegisgate::SemanticCache>(
            embedder_, *store_, 0.90f, std::chrono::seconds(3600), 100000);
        next_ = 0;
    }

    aegisgate::HashEmbedder embedder_;
    std::unique_ptr<aegisgate::HnswVectorStore> store_;
    std::unique_ptr<aegisgate::SemanticCache> cache_;
    size_t next_ = 0;
};

BENCHMARK_DEFINE_F(SemanticCachePutFixture, Put)(benchmark::State& state) {
    for (auto _ : state) {
        const std::string prompt = "bench_put_prompt_" + std::to_string(next_);
        cache_->put(prompt, "bench_put_response");
        ++next_;
        benchmark::DoNotOptimize(cache_->size());
    }
}
BENCHMARK_REGISTER_F(SemanticCachePutFixture, Put)->Name("BM_SemanticCache_Put");

class SemanticCacheGetHitFixture : public benchmark::Fixture {
public:
    SemanticCacheGetHitFixture() : embedder_(128) {}

    void SetUp(const benchmark::State&) override {
        store_ = std::make_unique<aegisgate::HnswVectorStore>(128, 10000);
        store_->initialize();
        cache_ = std::make_unique<aegisgate::SemanticCache>(
            embedder_, *store_, 0.90f, std::chrono::seconds(3600), 10000);
        cache_->put("bench query", "bench response");
    }

    aegisgate::HashEmbedder embedder_;
    std::unique_ptr<aegisgate::HnswVectorStore> store_;
    std::unique_ptr<aegisgate::SemanticCache> cache_;
};

BENCHMARK_DEFINE_F(SemanticCacheGetHitFixture, GetHit)(benchmark::State& state) {
    for (auto _ : state) {
        auto hit = cache_->get("bench query");
        benchmark::DoNotOptimize(hit);
    }
}
BENCHMARK_REGISTER_F(SemanticCacheGetHitFixture, GetHit)
    ->Name("BM_SemanticCache_Get_Hit");

class SemanticCacheGetMissFixture : public benchmark::Fixture {
public:
    SemanticCacheGetMissFixture() : embedder_(128) {}

    void SetUp(const benchmark::State&) override {
        store_ = std::make_unique<aegisgate::HnswVectorStore>(128, 1000);
        store_->initialize();
        cache_ = std::make_unique<aegisgate::SemanticCache>(
            embedder_, *store_, 0.90f, std::chrono::seconds(3600), 10000);
    }

    aegisgate::HashEmbedder embedder_;
    std::unique_ptr<aegisgate::HnswVectorStore> store_;
    std::unique_ptr<aegisgate::SemanticCache> cache_;
};

BENCHMARK_DEFINE_F(SemanticCacheGetMissFixture, GetMiss)(benchmark::State& state) {
    const std::string miss_prompt =
        "zzzz_semantic_cache_benchmark_miss_no_such_entry";
    for (auto _ : state) {
        auto hit = cache_->get(miss_prompt);
        benchmark::DoNotOptimize(hit);
    }
}
BENCHMARK_REGISTER_F(SemanticCacheGetMissFixture, GetMiss)
    ->Name("BM_SemanticCache_Get_Miss");
