#include <gtest/gtest.h>
#include "rag/knowledge_base.h"
#include "cache/embedder.h"
#include "cache/hnsw_vector_store.h"

using namespace aegisgate;

class KnowledgeBaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        embedder_ = std::make_unique<HashEmbedder>(128);
        store_ = std::make_unique<HnswVectorStore>(128, 1000, 1);
        ASSERT_TRUE(store_->initialize());
        kb_ = std::make_unique<KnowledgeBase>(*embedder_, *store_);
    }

    std::unique_ptr<HashEmbedder> embedder_;
    std::unique_ptr<HnswVectorStore> store_;
    std::unique_ptr<KnowledgeBase> kb_;
};

TEST(ChunkTextTest, Basic) {
    auto chunks = KnowledgeBase::chunkText("abcdefghij", 5, 0);
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0], "abcde");
    EXPECT_EQ(chunks[1], "fghij");
}

TEST(ChunkTextTest, WithOverlap) {
    auto chunks = KnowledgeBase::chunkText("abcdefghij", 5, 2);
    ASSERT_GE(chunks.size(), 2u);
    EXPECT_EQ(chunks[0], "abcde");
    std::string overlap_region = chunks[0].substr(chunks[0].size() - 2);
    EXPECT_EQ(overlap_region, chunks[1].substr(0, 2));
}

TEST(ChunkTextTest, ShortText) {
    auto chunks = KnowledgeBase::chunkText("short", 512, 64);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0], "short");
}

TEST(ChunkTextTest, EmptyText) {
    auto chunks = KnowledgeBase::chunkText("", 512, 64);
    EXPECT_TRUE(chunks.empty());
}

TEST_F(KnowledgeBaseTest, AddAndSearchDocument) {
    std::string doc_id = kb_->addDocument("the quick brown fox jumps over the lazy dog");
    EXPECT_FALSE(doc_id.empty());
    EXPECT_EQ(kb_->documentCount(), 1u);

    auto results = kb_->search("quick brown fox", 3, 0.0f);
    EXPECT_GE(results.size(), 1u);
    if (!results.empty()) {
        EXPECT_EQ(results[0].document_id, doc_id);
        EXPECT_FALSE(results[0].content.empty());
    }
}

TEST_F(KnowledgeBaseTest, RemoveDocument) {
    std::string doc_id = kb_->addDocument("test document content for removal");
    EXPECT_EQ(kb_->documentCount(), 1u);
    EXPECT_TRUE(kb_->removeDocument(doc_id));
    EXPECT_EQ(kb_->documentCount(), 0u);
    EXPECT_FALSE(kb_->removeDocument("nonexistent_id"));
}

TEST_F(KnowledgeBaseTest, ListDocuments) {
    kb_->addDocument("first document");
    kb_->addDocument("second document");
    auto docs = kb_->listDocuments();
    EXPECT_EQ(docs.size(), 2u);
}

TEST_F(KnowledgeBaseTest, SearchRelevance) {
    kb_->addDocument("machine learning is a subset of artificial intelligence");
    auto results = kb_->search("machine learning", 3, 0.0f);
    ASSERT_GE(results.size(), 1u);
    EXPECT_GT(results[0].relevance, 0.0f);
}

TEST_F(KnowledgeBaseTest, GetDocument) {
    std::string doc_id = kb_->addDocument("some content");
    auto info = kb_->getDocument(doc_id);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->id, doc_id);
    EXPECT_GT(info->chunk_count, 0u);

    auto missing = kb_->getDocument("no_such_id");
    EXPECT_FALSE(missing.has_value());
}

TEST_F(KnowledgeBaseTest, Clear) {
    kb_->addDocument("document one");
    kb_->addDocument("document two");
    EXPECT_EQ(kb_->documentCount(), 2u);
    kb_->clear();
    EXPECT_EQ(kb_->documentCount(), 0u);
    EXPECT_EQ(kb_->chunkCount(), 0u);
}

TEST_F(KnowledgeBaseTest, ChunkCount) {
    std::string long_text(1200, 'a');
    kb_->addDocument(long_text);
    EXPECT_GT(kb_->chunkCount(), 1u);
}

TEST_F(KnowledgeBaseTest, AddDocumentWithMetadata) {
    nlohmann::json meta = {{"title", "My Doc"}, {"author", "Test"}};
    std::string doc_id = kb_->addDocument("content", meta);
    auto info = kb_->getDocument(doc_id);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->title, "My Doc");
}
