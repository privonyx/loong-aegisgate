// TASK-20260518-02 Phase 11.5 — Epic 1.0 tests
//
// Typed-test fixture (A11 pattern) covering ApprovalProposal CRUD across
// MemoryPersistentStore + SqlitePersistentStore. The PgPersistentStore
// counterpart lives in test_pg_persistent_store.cpp because it shares
// the "no real PG" pattern with the rest of the PG suite.

#include "storage/memory_persistent_store.h"
#include "storage/sqlite_persistent_store.h"
#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <string>

using namespace aegisgate;

// --------- Factories: produce an initialized backend per typed test ---------

struct MemoryStoreFactory {
    static std::unique_ptr<PersistentStore> make() {
        auto store = std::make_unique<MemoryPersistentStore>();
        store->initialize();
        return store;
    }
    static const char* backend_name() { return "memory"; }
};

struct SqliteStoreFactory {
    static std::unique_ptr<PersistentStore> make() {
        auto store = std::make_unique<SQLitePersistentStore>(":memory:");
        store->initialize();
        return store;
    }
    static const char* backend_name() { return "sqlite"; }
};

// --------- Fixture ---------------------------------------------------------

template <typename Factory>
class ApprovalProposalStoreTest : public ::testing::Test {
protected:
    void SetUp() override { store_ = Factory::make(); }
    void TearDown() override { if (store_) store_->close(); }

    static ApprovalProposalRecord makeRec(const std::string& id,
                                           const std::string& state = "PROPOSED",
                                           const std::string& source = "CostOptimizer",
                                           std::int64_t proposed_at_ms = 1716030000000) {
        ApprovalProposalRecord r;
        r.id = id;
        r.source = source;
        r.subject = "Downgrade tenant " + id + " from premium to standard";
        r.payload_json = R"({"tenant_id":"t1","from":"premium","to":"standard"})";
        r.decision_trace_json = R"({"source_id":"cost_optimizer","algorithm_name":"cheapest_alternative_v1"})";
        r.proposed_at_ms = proposed_at_ms;
        r.proposer_user_id = "system";
        r.state = state;
        r.payload_sha256 = "a3f5b2c1d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0";
        return r;
    }

    std::unique_ptr<PersistentStore> store_;
};

using StoreTypes = ::testing::Types<MemoryStoreFactory, SqliteStoreFactory>;
TYPED_TEST_SUITE(ApprovalProposalStoreTest, StoreTypes);

// --------- Tests ----------------------------------------------------------

TYPED_TEST(ApprovalProposalStoreTest, InsertAndGet) {
    auto rec = this->makeRec("01HNAPPROVAL000000000000A1");
    EXPECT_TRUE(this->store_->insertApprovalProposal(rec));

    auto got = this->store_->getApprovalProposal(rec.id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id, rec.id);
    EXPECT_EQ(got->source, "CostOptimizer");
    EXPECT_EQ(got->state, "PROPOSED");
    EXPECT_EQ(got->proposed_at_ms, 1716030000000);
    EXPECT_EQ(got->payload_sha256, rec.payload_sha256);
    EXPECT_EQ(got->payload_json, rec.payload_json);
    EXPECT_EQ(got->decision_trace_json, rec.decision_trace_json);
}

TYPED_TEST(ApprovalProposalStoreTest, GetMissingReturnsNullopt) {
    auto got = this->store_->getApprovalProposal("nonexistent-id");
    EXPECT_FALSE(got.has_value());
}

TYPED_TEST(ApprovalProposalStoreTest, InsertDuplicateIdRejected) {
    auto rec = this->makeRec("01HNDUP00000000000000000A1");
    EXPECT_TRUE(this->store_->insertApprovalProposal(rec));
    EXPECT_FALSE(this->store_->insertApprovalProposal(rec));
}

TYPED_TEST(ApprovalProposalStoreTest, InsertEmptyIdRejected) {
    auto rec = this->makeRec("");
    EXPECT_FALSE(this->store_->insertApprovalProposal(rec));
}

TYPED_TEST(ApprovalProposalStoreTest, UpdateChangesState) {
    auto rec = this->makeRec("01HNUPD00000000000000000A1");
    ASSERT_TRUE(this->store_->insertApprovalProposal(rec));

    rec.state = "APPROVED";
    rec.reviewer_user_id = "manual:alice@example.com";
    rec.reviewed_at_ms = 1716030060000;
    EXPECT_TRUE(this->store_->updateApprovalProposal(rec));

    auto got = this->store_->getApprovalProposal(rec.id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->state, "APPROVED");
    EXPECT_EQ(got->reviewer_user_id, "manual:alice@example.com");
    EXPECT_EQ(got->reviewed_at_ms, 1716030060000);
}

TYPED_TEST(ApprovalProposalStoreTest, UpdateMissingRejected) {
    auto rec = this->makeRec("missing-id");
    EXPECT_FALSE(this->store_->updateApprovalProposal(rec));
}

TYPED_TEST(ApprovalProposalStoreTest, ListAllOrderedByProposedAtDesc) {
    this->store_->insertApprovalProposal(this->makeRec("01HNLST00000000000000000A1",
                                                         "PROPOSED", "CostOptimizer", 100));
    this->store_->insertApprovalProposal(this->makeRec("01HNLST00000000000000000A2",
                                                         "APPROVED", "CostOptimizer", 200));
    this->store_->insertApprovalProposal(this->makeRec("01HNLST00000000000000000A3",
                                                         "PROPOSED", "AutoRecovery", 300));

    ApprovalProposalQuery q;
    auto items = this->store_->listApprovalProposals(q);
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0].id, "01HNLST00000000000000000A3");  // newest first
    EXPECT_EQ(items[1].id, "01HNLST00000000000000000A2");
    EXPECT_EQ(items[2].id, "01HNLST00000000000000000A1");
}

TYPED_TEST(ApprovalProposalStoreTest, ListFilterByState) {
    this->store_->insertApprovalProposal(this->makeRec("01HNFLT00000000000000000A1",
                                                         "PROPOSED"));
    this->store_->insertApprovalProposal(this->makeRec("01HNFLT00000000000000000A2",
                                                         "APPROVED"));
    this->store_->insertApprovalProposal(this->makeRec("01HNFLT00000000000000000A3",
                                                         "APPROVED"));

    ApprovalProposalQuery q;
    q.state_filter = "APPROVED";
    auto items = this->store_->listApprovalProposals(q);
    EXPECT_EQ(items.size(), 2u);
    for (const auto& it : items) EXPECT_EQ(it.state, "APPROVED");
}

TYPED_TEST(ApprovalProposalStoreTest, ListFilterBySource) {
    this->store_->insertApprovalProposal(this->makeRec("01HNSRC00000000000000000A1",
                                                         "PROPOSED", "CostOptimizer"));
    this->store_->insertApprovalProposal(this->makeRec("01HNSRC00000000000000000A2",
                                                         "PROPOSED", "AutoRecovery"));

    ApprovalProposalQuery q;
    q.source_filter = "CostOptimizer";
    auto items = this->store_->listApprovalProposals(q);
    EXPECT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].source, "CostOptimizer");
}

TYPED_TEST(ApprovalProposalStoreTest, ListLimitAndOffset) {
    for (int i = 0; i < 10; ++i) {
        std::string id = "01HNPAG00000000000000000Z" + std::to_string(i);
        this->store_->insertApprovalProposal(
            this->makeRec(id, "PROPOSED", "CostOptimizer", 100 + i));
    }

    ApprovalProposalQuery q;
    q.limit = 3;
    q.offset = 0;
    auto page1 = this->store_->listApprovalProposals(q);
    EXPECT_EQ(page1.size(), 3u);

    q.offset = 3;
    auto page2 = this->store_->listApprovalProposals(q);
    EXPECT_EQ(page2.size(), 3u);
    EXPECT_NE(page1.front().id, page2.front().id);
}

TYPED_TEST(ApprovalProposalStoreTest, CrossRestartRecovery) {
    // Insert 100 records
    for (int i = 0; i < 100; ++i) {
        std::string id = "01HNREC0000000000000000Z" + std::to_string(i + 100);
        this->store_->insertApprovalProposal(
            this->makeRec(id, "PROPOSED", "CostOptimizer", 1000 + i));
    }

    // "Restart" by listing — production code uses this exact pattern in
    // ApprovalQueue::initialize() to repopulate its in-memory cache.
    ApprovalProposalQuery q;
    q.limit = 1000;
    auto restored = this->store_->listApprovalProposals(q);
    EXPECT_EQ(restored.size(), 100u);
    for (const auto& rec : restored) {
        EXPECT_EQ(rec.source, "CostOptimizer");
        EXPECT_EQ(rec.state, "PROPOSED");
        EXPECT_FALSE(rec.payload_sha256.empty());
    }
}

TYPED_TEST(ApprovalProposalStoreTest, PruneOldRecords) {
    // Anchor on wall clock so the cutoff (computed inside the backend via
    // std::chrono::system_clock::now()) lines up with our synthetic
    // proposed_at_ms values. Hard-coded epochs would drift the moment the
    // system clock crosses retention_days.
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto day_ms = 86400LL * 1000;

    // Insert: 1 record 100 days old, 1 record 1 day old.
    auto old_rec = this->makeRec("01HNPRN0000000000000000A1",
                                  "ROLLED_BACK", "CostOptimizer",
                                  now_ms - 100 * day_ms);
    auto new_rec = this->makeRec("01HNPRN0000000000000000A2",
                                  "PROPOSED", "CostOptimizer",
                                  now_ms - 1 * day_ms);
    this->store_->insertApprovalProposal(old_rec);
    this->store_->insertApprovalProposal(new_rec);

    auto pruned = this->store_->pruneApprovalProposals(90);
    EXPECT_GE(pruned, 1);
    EXPECT_FALSE(this->store_->getApprovalProposal(old_rec.id).has_value());
    EXPECT_TRUE(this->store_->getApprovalProposal(new_rec.id).has_value());
}

TYPED_TEST(ApprovalProposalStoreTest, EmptyQueryReturnsEmpty) {
    ApprovalProposalQuery q;
    auto items = this->store_->listApprovalProposals(q);
    EXPECT_EQ(items.size(), 0u);
}
