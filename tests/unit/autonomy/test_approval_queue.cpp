// Phase 11.5 TASK-20260518-02 Epic 1.4 — ApprovalQueue tests.
//
// Coverage (plan §D Task 1.4):
//   1. insert succeeds + persists in store + cache
//   2. get round-trip
//   3. update mutates cache + store
//   4. list filter + sort + page semantics
//   5. cross-restart recovery via initialize() — the headline SR guard
//   + memory-only mode (no store) sanity
//   + duplicate id rejection
//   + prune semantics

#include "observe/autonomy/approval_queue.h"

#include "observe/autonomy/approval_proposal.h"
#include "storage/memory_persistent_store.h"

#include <gtest/gtest.h>
#include <chrono>
#include <memory>

using namespace aegisgate;
using namespace aegisgate::autonomy;

namespace {

ApprovalProposal makeProp(const std::string& id,
                          ApprovalState state = ApprovalState::PROPOSED,
                          AutonomySource source = AutonomySource::CostOptimizer,
                          std::int64_t proposed_at_ms = 1716030000000LL) {
    ApprovalProposal p;
    p.id              = id;
    p.source          = source;
    p.subject         = "test " + id;
    p.payload         = nlohmann::json{{"k", id}};
    p.decision_trace  = nlohmann::json{{"source_id", "test"},
                                        {"algorithm_name", "v1"},
                                        {"input_hash_sha256", "deadbeef"},
                                        {"proposed_at_ms", proposed_at_ms}};
    p.proposed_at_ms  = proposed_at_ms;
    p.state           = state;
    p.payload_sha256  = computePayloadSha256(p.payload);
    return p;
}

} // namespace

// ---------- insert / get --------------------------------------------------

TEST(ApprovalQueueTest, InsertWritesToStoreAndCache) {
    auto store = std::make_unique<MemoryPersistentStore>();
    store->initialize();
    ApprovalQueue q(store.get());
    ASSERT_TRUE(q.initialize());

    auto p = makeProp("01HNAQ00000000000000000A1");
    EXPECT_EQ(q.insert(p), p.id);

    // Cached value visible to subsequent get()
    auto got = q.get(p.id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id, p.id);
    EXPECT_EQ(got->payload_sha256, p.payload_sha256);

    // Store witnesses the same record
    auto rec = store->getApprovalProposal(p.id);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->state, "PROPOSED");
    EXPECT_EQ(q.size(), 1u);
}

TEST(ApprovalQueueTest, InsertEmptyIdRejected) {
    auto store = std::make_unique<MemoryPersistentStore>();
    store->initialize();
    ApprovalQueue q(store.get());
    q.initialize();
    auto p = makeProp("");
    EXPECT_EQ(q.insert(p), std::string());
    EXPECT_EQ(q.size(), 0u);
}

TEST(ApprovalQueueTest, InsertDuplicateRejectedByStore) {
    auto store = std::make_unique<MemoryPersistentStore>();
    store->initialize();
    ApprovalQueue q(store.get());
    q.initialize();
    auto p = makeProp("01HNDUP00000000000000000A1");
    ASSERT_EQ(q.insert(p), p.id);
    EXPECT_EQ(q.insert(p), std::string()) << "store unique constraint";
}

TEST(ApprovalQueueTest, GetMissingReturnsNullopt) {
    MemoryPersistentStore store;
    store.initialize();
    ApprovalQueue q(&store);
    q.initialize();
    EXPECT_FALSE(q.get("missing-id").has_value());
}

// ---------- update --------------------------------------------------------

TEST(ApprovalQueueTest, UpdateChangesStateInCacheAndStore) {
    MemoryPersistentStore store;
    store.initialize();
    ApprovalQueue q(&store);
    q.initialize();
    auto p = makeProp("01HNUPD00000000000000000A1");
    q.insert(p);

    p.state             = ApprovalState::APPROVED;
    p.reviewer_user_id  = "manual:alice";
    p.reviewed_at_ms    = 1716030060000LL;
    EXPECT_TRUE(q.update(p));

    auto cached = q.get(p.id);
    ASSERT_TRUE(cached.has_value());
    EXPECT_EQ(cached->state, ApprovalState::APPROVED);
    EXPECT_EQ(cached->reviewer_user_id, "manual:alice");

    auto rec = store.getApprovalProposal(p.id);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->state, "APPROVED");
}

TEST(ApprovalQueueTest, UpdateMissingFailsCleanly) {
    MemoryPersistentStore store;
    store.initialize();
    ApprovalQueue q(&store);
    q.initialize();
    auto p = makeProp("01HNNOSUCH000000000000A1");
    EXPECT_FALSE(q.update(p)) << "store has no row to update";
}

// ---------- list filter / sort / page -------------------------------------

TEST(ApprovalQueueTest, ListFiltersBySourceAndSortsDesc) {
    MemoryPersistentStore store;
    store.initialize();
    ApprovalQueue q(&store);
    q.initialize();

    q.insert(makeProp("01HNL000000000000000000A1",
                       ApprovalState::PROPOSED,
                       AutonomySource::CostOptimizer, 100));
    q.insert(makeProp("01HNL000000000000000000A2",
                       ApprovalState::PROPOSED,
                       AutonomySource::AutoRecovery, 200));
    q.insert(makeProp("01HNL000000000000000000A3",
                       ApprovalState::APPROVED,
                       AutonomySource::CostOptimizer, 300));

    ApprovalQueueQuery query;
    query.source_filter = AutonomySource::CostOptimizer;
    auto items = q.list(query);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].id, "01HNL000000000000000000A3");  // newest first
    EXPECT_EQ(items[1].id, "01HNL000000000000000000A1");
}

TEST(ApprovalQueueTest, ListFiltersByStateAndPages) {
    MemoryPersistentStore store;
    store.initialize();
    ApprovalQueue q(&store);
    q.initialize();
    for (int i = 0; i < 5; ++i) {
        q.insert(makeProp("01HNPAG00000000000000000Z" + std::to_string(i),
                           ApprovalState::PROPOSED,
                           AutonomySource::CostOptimizer, 1000 + i));
    }
    q.insert(makeProp("01HNPAG00000000000000000Z9",
                       ApprovalState::APPROVED));

    ApprovalQueueQuery query;
    query.state_filter = ApprovalState::PROPOSED;
    query.limit  = 2;
    query.offset = 0;
    auto page1 = q.list(query);
    EXPECT_EQ(page1.size(), 2u);

    query.offset = 2;
    auto page2 = q.list(query);
    EXPECT_EQ(page2.size(), 2u);
    EXPECT_NE(page1[0].id, page2[0].id);
}

// ---------- cross-restart recovery (the headline SR test) ---------------

TEST(ApprovalQueueTest, CrossRestartRecoversFromStore) {
    auto store = std::make_unique<MemoryPersistentStore>();
    store->initialize();
    {
        ApprovalQueue q1(store.get());
        q1.initialize();
        for (int i = 0; i < 50; ++i) {
            q1.insert(makeProp("01HNREC000000000000000Z" + std::to_string(i),
                                ApprovalState::PROPOSED,
                                AutonomySource::CostOptimizer, 1000 + i));
        }
        EXPECT_EQ(q1.size(), 50u);
    }
    // "Process restart": drop the first queue, create a fresh one against
    // the same store, initialize() must repopulate cache verbatim.
    ApprovalQueue q2(store.get());
    ASSERT_TRUE(q2.initialize());
    EXPECT_EQ(q2.size(), 50u);

    // Verify content (not just count) — order-independent.
    auto items = q2.list({});
    EXPECT_EQ(items.size(), 50u);
    for (const auto& it : items) {
        EXPECT_EQ(it.source, AutonomySource::CostOptimizer);
        EXPECT_EQ(it.state, ApprovalState::PROPOSED);
        EXPECT_FALSE(it.payload_sha256.empty());
    }
}

// ---------- memory-only mode (no store) ----------------------------------

TEST(ApprovalQueueTest, MemoryOnlyModeWorksWithoutStore) {
    ApprovalQueue q(/*store=*/nullptr);
    ASSERT_TRUE(q.initialize());
    auto p = makeProp("01HNNO0000000000000000A1");
    EXPECT_EQ(q.insert(p), p.id);
    EXPECT_TRUE(q.get(p.id).has_value());
    EXPECT_EQ(q.size(), 1u);
}

TEST(ApprovalQueueTest, MemoryOnlyModeDuplicateRejected) {
    ApprovalQueue q(nullptr);
    q.initialize();
    auto p = makeProp("01HNDUPMEM00000000000A1");
    ASSERT_EQ(q.insert(p), p.id);
    EXPECT_EQ(q.insert(p), std::string());
}

// ---------- prune ---------------------------------------------------------

TEST(ApprovalQueueTest, PruneEvictsOldRecords) {
    MemoryPersistentStore store;
    store.initialize();
    ApprovalQueue q(&store);
    q.initialize();

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto day_ms = 86400LL * 1000;
    q.insert(makeProp("01HNPRNQ00000000000000A1",
                       ApprovalState::ROLLED_BACK,
                       AutonomySource::CostOptimizer,
                       now_ms - 100 * day_ms));
    q.insert(makeProp("01HNPRNQ00000000000000A2",
                       ApprovalState::PROPOSED,
                       AutonomySource::CostOptimizer,
                       now_ms - 1 * day_ms));

    EXPECT_EQ(q.size(), 2u);
    auto pruned = q.prune(90);
    EXPECT_GE(pruned, 1);
    EXPECT_FALSE(q.get("01HNPRNQ00000000000000A1").has_value());
    EXPECT_TRUE(q.get("01HNPRNQ00000000000000A2").has_value());
}
