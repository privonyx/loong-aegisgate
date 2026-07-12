#include <gtest/gtest.h>
#include "rag/retrieval_stage.h"
#include "cache/embedder.h"
#include "cache/hnsw_vector_store.h"
#include "observe/metrics.h"

using namespace aegisgate;

class RetrievalStageTest : public ::testing::Test {
protected:
    void SetUp() override {
        embedder_ = std::make_unique<HashEmbedder>(128);
        store_ = std::make_unique<HnswVectorStore>(128, 1000, 1);
        ASSERT_TRUE(store_->initialize());
        kb_ = std::make_unique<KnowledgeBase>(*embedder_, *store_);
    }

    RequestContext makeCtx(const std::string& user_msg) {
        RequestContext ctx;
        ctx.request_id = "test-req";
        ctx.chat_request.messages.emplace_back("user", user_msg);
        return ctx;
    }

    std::unique_ptr<HashEmbedder> embedder_;
    std::unique_ptr<HnswVectorStore> store_;
    std::unique_ptr<KnowledgeBase> kb_;
};

TEST_F(RetrievalStageTest, ProcessWhenDisabled) {
    RetrievalConfig cfg;
    cfg.enabled = false;
    RetrievalStage stage(cfg);
    stage.setKnowledgeBase(kb_.get());

    auto ctx = makeCtx("hello");
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
    EXPECT_TRUE(ctx.retrieval_sources.empty());
}

TEST_F(RetrievalStageTest, ProcessWithNoKnowledgeBase) {
    RetrievalConfig cfg;
    cfg.enabled = true;
    RetrievalStage stage(cfg);

    auto ctx = makeCtx("hello");
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
    EXPECT_TRUE(ctx.retrieval_sources.empty());
}

TEST_F(RetrievalStageTest, ProcessWithResults) {
    kb_->addDocument("machine learning is great for data analysis");

    RetrievalConfig cfg;
    cfg.enabled = true;
    cfg.top_k = 3;
    cfg.min_relevance = 0.0f;
    RetrievalStage stage(cfg);
    stage.setKnowledgeBase(kb_.get());

    auto ctx = makeCtx("machine learning");
    auto result = stage.process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_FALSE(ctx.retrieval_sources.empty());
    EXPECT_FALSE(ctx.citations.empty());

    bool found_system_context = false;
    for (const auto& msg : ctx.chat_request.messages) {
        if (msg.role == "system" && msg.content.find("Based on the following knowledge") != std::string::npos) {
            found_system_context = true;
            break;
        }
    }
    EXPECT_TRUE(found_system_context);
}

TEST_F(RetrievalStageTest, BuildContextBlockFormat) {
    kb_->addDocument("apples are a fruit that grows on trees");

    RetrievalConfig cfg;
    cfg.enabled = true;
    cfg.top_k = 3;
    cfg.min_relevance = 0.0f;
    RetrievalStage stage(cfg);
    stage.setKnowledgeBase(kb_.get());

    auto ctx = makeCtx("apples");
    stage.process(ctx);

    bool found = false;
    for (const auto& msg : ctx.chat_request.messages) {
        if (msg.role == "system" && msg.content.find("[1]") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(RetrievalStageTest, InjectionPositionBeforeUser) {
    kb_->addDocument("knowledge about cats and dogs");

    RetrievalConfig cfg;
    cfg.enabled = true;
    cfg.top_k = 3;
    cfg.min_relevance = 0.0f;
    cfg.injection_position = InjectionPosition::BeforeUser;
    RetrievalStage stage(cfg);
    stage.setKnowledgeBase(kb_.get());

    auto ctx = makeCtx("cats");
    stage.process(ctx);

    auto& msgs = ctx.chat_request.messages;
    ASSERT_GE(msgs.size(), 2u);
    EXPECT_EQ(msgs.back().role, "user");
    bool sys_before_user = false;
    for (size_t i = 0; i + 1 < msgs.size(); ++i) {
        if (msgs[i].role == "system" &&
            msgs[i].content.find("Based on") != std::string::npos &&
            msgs[i + 1].role == "user") {
            sys_before_user = true;
            break;
        }
    }
    EXPECT_TRUE(sys_before_user);
}

// P1-B: a retrieval that injects context must increment the rag_retrievals_total
// counter (previously registered but never written → dashboards saw 0).
TEST_F(RetrievalStageTest, ProcessWithResultsIncrementsRagMetric) {
    auto& metric = MetricsRegistry::instance().ragRetrievalsTotal();
    double before = metric.get();

    kb_->addDocument("retrieval metric coverage document");
    RetrievalConfig cfg;
    cfg.enabled = true;
    cfg.top_k = 3;
    cfg.min_relevance = 0.0f;
    RetrievalStage stage(cfg);
    stage.setKnowledgeBase(kb_.get());

    auto ctx = makeCtx("retrieval metric");
    stage.process(ctx);

    EXPECT_GT(MetricsRegistry::instance().ragRetrievalsTotal().get(), before);
}

TEST_F(RetrievalStageTest, DefaultConfig) {
    RetrievalStage stage;
    EXPECT_FALSE(stage.retrievalConfig().enabled);
    EXPECT_EQ(stage.retrievalConfig().top_k, 3);
}

namespace {
// Extract the injected RAG system block ("Based on the following knowledge") if any.
std::string injectedBlock(const RequestContext& ctx) {
    for (const auto& msg : ctx.chat_request.messages) {
        if (msg.role == "system" &&
            msg.content.find("Based on the following knowledge") !=
                std::string::npos) {
            return msg.content;
        }
    }
    return {};
}
size_t countEntries(const std::string& block) {
    size_t n = 0, pos = 0;
    while ((pos = block.find("] ", pos)) != std::string::npos) {
        ++n;
        pos += 2;
    }
    return n;
}
} // namespace

// I32 (TASK-20260703-04): a tiny token budget must truncate the injected
// context — previously buildContextBlock concatenated every result unbounded,
// so a large corpus could blow past the upstream context window / inflate cost.
TEST_F(RetrievalStageTest, MaxContextTokensTruncatesLongContext) {
    for (int i = 0; i < 8; ++i) {
        kb_->addDocument("shared retrieval topic document chunk number " +
                         std::to_string(i) + " with extra padding text here");
    }

    RetrievalConfig cfg;
    cfg.enabled = true;
    cfg.top_k = 8;
    cfg.min_relevance = 0.0f;
    cfg.max_context_tokens = 25; // ~100 chars budget → only a subset fits
    RetrievalStage stage(cfg);
    stage.setKnowledgeBase(kb_.get());

    auto ctx = makeCtx("shared retrieval topic");
    stage.process(ctx);

    std::string block = injectedBlock(ctx);
    ASSERT_FALSE(block.empty());
    EXPECT_NE(block.find("[context truncated to fit token budget]"),
              std::string::npos);
    EXPECT_LT(countEntries(block), 8u);
}

// Complementary GREEN guard: a generous budget keeps every entry and adds no
// truncation marker (locks the boundary comparison against off-by-one mutation).
TEST_F(RetrievalStageTest, LargeBudgetKeepsAllEntriesNoTruncation) {
    for (int i = 0; i < 4; ++i) {
        kb_->addDocument("budget headroom doc " + std::to_string(i));
    }

    RetrievalConfig cfg;
    cfg.enabled = true;
    cfg.top_k = 4;
    cfg.min_relevance = 0.0f;
    cfg.max_context_tokens = 100000;
    RetrievalStage stage(cfg);
    stage.setKnowledgeBase(kb_.get());

    auto ctx = makeCtx("budget headroom");
    stage.process(ctx);

    std::string block = injectedBlock(ctx);
    ASSERT_FALSE(block.empty());
    EXPECT_EQ(block.find("[context truncated to fit token budget]"),
              std::string::npos);
    EXPECT_EQ(countEntries(block), 4u);
}

// injection_position enum handling (assembler maps config string → these enums).
TEST_F(RetrievalStageTest, InjectionPositionAfterSystem) {
    kb_->addDocument("routing knowledge for after-system injection");

    RetrievalConfig cfg;
    cfg.enabled = true;
    cfg.top_k = 3;
    cfg.min_relevance = 0.0f;
    cfg.injection_position = InjectionPosition::AfterSystem;
    RetrievalStage stage(cfg);
    stage.setKnowledgeBase(kb_.get());

    RequestContext ctx;
    ctx.request_id = "test-after-system";
    ctx.chat_request.messages.emplace_back("system", "you are helpful");
    ctx.chat_request.messages.emplace_back("user", "routing knowledge");
    stage.process(ctx);

    auto& msgs = ctx.chat_request.messages;
    ASSERT_GE(msgs.size(), 3u);
    EXPECT_EQ(msgs[0].role, "system");
    EXPECT_EQ(msgs[0].content, "you are helpful");
    EXPECT_NE(msgs[1].content.find("Based on the following knowledge"),
              std::string::npos);
}
