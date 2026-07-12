#include <gtest/gtest.h>
#include "cache/vector_store.h"
#include "cache/hnsw_vector_store.h"
#include "cache/embedder.h"
#include <map>

using namespace aegisgate;

class HnswVectorStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        embedder_ = std::make_unique<HashEmbedder>(32);
        store_ = std::make_unique<HnswVectorStore>(32, 1000, 64);
        ASSERT_TRUE(store_->initialize());
    }

    std::vector<float> embed(const std::string& text) {
        return embedder_->embed(text);
    }

    std::unique_ptr<HashEmbedder> embedder_;
    std::unique_ptr<HnswVectorStore> store_;
};

TEST_F(HnswVectorStoreTest, BackendNameIsHnswlib) {
    EXPECT_EQ(store_->backendName(), "hnswlib");
}

TEST_F(HnswVectorStoreTest, InsertAndSearch) {
    store_->insert("ctx", "id1", embed("hello world"));
    auto results = store_->search("ctx", embed("hello world"), 1, 0.0f);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].id, "id1");
    EXPECT_GT(results[0].score, 0.9f);
}

// P2-#1: insert reports success/failure so the cache layer can avoid phantom
// entries. A good vector → true; a dimension mismatch → false.
TEST_F(HnswVectorStoreTest, InsertReturnsTrueOnSuccessFalseOnDimMismatch) {
    EXPECT_TRUE(store_->insert("ctx", "ok", embed("good vector")));
    std::vector<float> wrong_dim(8, 0.1f);  // store dim is 32
    EXPECT_FALSE(store_->insert("ctx", "bad", wrong_dim));
}

TEST_F(HnswVectorStoreTest, PartitionIsolation) {
    auto vec = embed("hello");
    store_->insert("ctx_a", "id1", vec);
    auto results = store_->search("ctx_b", vec, 1, 0.0f);
    EXPECT_TRUE(results.empty());

    results = store_->search("ctx_a", vec, 1, 0.0f);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].id, "id1");
}

TEST_F(HnswVectorStoreTest, Remove) {
    store_->insert("ctx", "id1", embed("test"));
    EXPECT_EQ(store_->size(), 1u);
    EXPECT_TRUE(store_->remove("ctx", "id1"));
    EXPECT_EQ(store_->size(), 0u);
}

TEST_F(HnswVectorStoreTest, SizeTracking) {
    EXPECT_EQ(store_->size(), 0u);
    store_->insert("a", "id1", embed("q1"));
    store_->insert("b", "id2", embed("q2"));
    EXPECT_EQ(store_->size(), 2u);
}

TEST_F(HnswVectorStoreTest, ThresholdFiltering) {
    store_->insert("ctx", "id1", embed("hello world"));
    auto results = store_->search("ctx", embed("completely different text"), 1, 0.999f);
    EXPECT_TRUE(results.empty());
}

TEST_F(HnswVectorStoreTest, EmptySearchReturnsNothing) {
    auto results = store_->search("empty_ctx", embed("anything"), 1, 0.0f);
    EXPECT_TRUE(results.empty());
}

TEST_F(HnswVectorStoreTest, SearchReturnsScoreField) {
    store_->insert("ctx", "id1", embed("test query"));
    auto results = store_->search("ctx", embed("test query"), 1);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_GT(results[0].score, 0.0f);
}

TEST_F(HnswVectorStoreTest, InitializeTwiceIsIdempotent) {
    store_->insert("ctx", "id1", embed("q1"));
    EXPECT_EQ(store_->size(), 1u);
    EXPECT_TRUE(store_->initialize());
    EXPECT_EQ(store_->size(), 0u);
}

TEST_F(HnswVectorStoreTest, MultipleEntriesSamePartition) {
    for (int i = 0; i < 10; ++i) {
        store_->insert("ctx", "id_" + std::to_string(i),
                       embed("query_" + std::to_string(i)));
    }
    EXPECT_EQ(store_->size(), 10u);
    auto results = store_->search("ctx", embed("query_5"), 3, 0.0f);
    EXPECT_GE(results.size(), 1u);
}

// Test that VectorStore interface works polymorphically
TEST(VectorStoreInterfaceTest, PolymorphicUsage) {
    auto store = std::make_unique<HnswVectorStore>(32, 100);
    VectorStore* base = store.get();

    EXPECT_TRUE(base->initialize());
    EXPECT_EQ(base->backendName(), "hnswlib");
    EXPECT_EQ(base->size(), 0u);

    HashEmbedder embedder(32);
    base->insert("", "test_id", embedder.embed("hello"));
    EXPECT_EQ(base->size(), 1u);

    auto results = base->search("", embedder.embed("hello"), 1);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].id, "test_id");

    EXPECT_TRUE(base->remove("", "test_id"));
    EXPECT_EQ(base->size(), 0u);
}

// ---------------------------------------------------------------------------
// Epic 3.1 (DA-1, D8=A): VectorStore::enumerate
// ---------------------------------------------------------------------------

TEST_F(HnswVectorStoreTest, EnumerateEmptyStoreReturnsTrueWithNoVisits) {
    int count = 0;
    bool ok = store_->enumerate(
        [&](const std::string&, const std::string&,
            const std::vector<float>&) { ++count; return true; });
    EXPECT_TRUE(ok);
    EXPECT_EQ(count, 0);
}

TEST_F(HnswVectorStoreTest, EnumerateVisitsAllInsertedEntries) {
    store_->insert("p1", "a", embed("alpha"));
    store_->insert("p1", "b", embed("bravo"));
    store_->insert("p2", "c", embed("charlie"));

    std::map<std::string, std::string> id_to_partition;
    bool ok = store_->enumerate(
        [&](const std::string& p, const std::string& id,
            const std::vector<float>& vec) {
            EXPECT_EQ(vec.size(), 32u);
            id_to_partition[id] = p;
            return true;
        });
    EXPECT_TRUE(ok);
    EXPECT_EQ(id_to_partition.size(), 3u);
    EXPECT_EQ(id_to_partition["a"], "p1");
    EXPECT_EQ(id_to_partition["b"], "p1");
    EXPECT_EQ(id_to_partition["c"], "p2");
}

TEST_F(HnswVectorStoreTest, EnumeratePropagatesVectorContents) {
    auto vec_in = embed("vector-fidelity-check");
    store_->insert("only", "x", vec_in);

    std::vector<float> vec_out;
    store_->enumerate(
        [&](const std::string&, const std::string&,
            const std::vector<float>& v) {
            vec_out = v;
            return true;
        });
    ASSERT_EQ(vec_out.size(), vec_in.size());
    for (size_t i = 0; i < vec_in.size(); ++i) {
        EXPECT_FLOAT_EQ(vec_out[i], vec_in[i]) << "mismatch at index " << i;
    }
}

TEST_F(HnswVectorStoreTest, EnumerateStopsWhenVisitorReturnsFalse) {
    for (int i = 0; i < 5; ++i) {
        store_->insert("p", "id_" + std::to_string(i),
                       embed("q_" + std::to_string(i)));
    }
    int visits = 0;
    bool ok = store_->enumerate(
        [&](const std::string&, const std::string&,
            const std::vector<float>&) {
            ++visits;
            return visits < 2;  // stop after the 2nd visit
        });
    EXPECT_FALSE(ok);  // early stop returns false
    EXPECT_EQ(visits, 2);
}

TEST_F(HnswVectorStoreTest, EnumeratePolymorphicViaBaseInterface) {
    store_->insert("p", "id1", embed("polymorphic"));
    VectorStore* base = store_.get();
    int n = 0;
    base->enumerate(
        [&](const std::string&, const std::string&,
            const std::vector<float>&) { ++n; return true; });
    EXPECT_EQ(n, 1);
}

// DA-1 contract: remote backends must opt-out by returning false. We
// validate that via the default base implementation directly (Milvus/Qdrant
// inherit it). Constructing the network-bound stores would require a live
// service; the interface contract is what dump relies on.
TEST(VectorStoreInterfaceTest, EnumerateDefaultsToUnsupportedFalse) {
    struct StubStore : public VectorStore {
        bool initialize() override { return true; }
        bool insert(const std::string&, const std::string&,
                    const std::vector<float>&) override { return true; }
        bool remove(const std::string&, const std::string&) override { return false; }
        std::vector<VectorSearchResult> search(
            const std::string&, const std::vector<float>&, size_t,
            float) const override { return {}; }
        size_t size() const override { return 0; }
        std::string backendName() const override { return "stub"; }
    };
    StubStore s;
    int visits = 0;
    bool ok = s.enumerate(
        [&](const std::string&, const std::string&,
            const std::vector<float>&) { ++visits; return true; });
    EXPECT_FALSE(ok);
    EXPECT_EQ(visits, 0);
}
