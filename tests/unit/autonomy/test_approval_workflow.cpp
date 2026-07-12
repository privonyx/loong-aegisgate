// Phase 11.5 TASK-20260518-02 Epic 1.5 — AutonomyApprovalWorkflow tests.
//
// 7 mandatory scenarios from plan §D Task 1.5 step 1:
//   1. state_machine_legal_transitions      (all legal edges PASS)
//   2. state_machine_illegal_transitions    (all illegal edges blocked)
//   3. propose_returns_unique_ulid          (no collisions across 100 calls)
//   4. payload_sha256_tampering_rejected    (T01 mitigation)
//   5. apply_failure_auto_rollback          (C1 decision verification)
//   6. env_disable_autonomy_blocks_propose  (SR17 + T06)
//   7. audit_logger_records_all_transitions (T03, chain_hash linked)
//
// Plus a missing-applier safeguard test to cover the dispatch table edge.

#include "observe/autonomy/approval_workflow.h"

#include "guardrail/audit.h"
#include "observe/autonomy/approval_queue.h"
#include "storage/memory_persistent_store.h"

#include <gtest/gtest.h>
#include <cstdlib>
#include <memory>
#include <unordered_set>

using namespace aegisgate;
using namespace aegisgate::autonomy;

namespace {

class MockApplier : public IApprovalApplier {
public:
    explicit MockApplier(bool succeed = true) : succeed_(succeed) {}
    ApplyResult apply(const ApprovalProposal& p, bool dry_run) override {
        (void)p; (void)dry_run;
        ++apply_calls_;
        last_dry_run_ = dry_run;
        if (succeed_) return ApplyResult::ok();
        return ApplyResult::fail("mock_failure", "intentional");
    }
    ApplyResult rollback(const ApprovalProposal& p) override {
        (void)p;
        ++rollback_calls_;
        return ApplyResult::ok();
    }
    std::string applierName() const override { return "mock"; }

    int apply_calls_    = 0;
    int rollback_calls_ = 0;
    bool last_dry_run_  = false;
    bool succeed_;
};

ApprovalProposal makeProp(
    AutonomySource src = AutonomySource::CostOptimizer,
    const nlohmann::json& payload = nlohmann::json{{"k", "v"}}) {
    ApprovalProposal p;
    p.source         = src;
    p.subject        = "unit-test";
    p.payload        = payload;
    p.decision_trace = nlohmann::json{
        {"source_id",         "test"},
        {"algorithm_name",    "v1"},
        {"input_hash_sha256", std::string(64, 'a')},
        {"proposed_at_ms",    1716030000000LL}};
    return p;
}

struct WorkflowFixture {
    std::shared_ptr<MemoryPersistentStore> store;
    std::shared_ptr<ApprovalQueue>         queue;
    std::shared_ptr<AuditLogger>           audit;
    std::shared_ptr<AutonomyApprovalWorkflow> wf;
    std::shared_ptr<MockApplier>           applier;

    WorkflowFixture()
        : store(std::make_shared<MemoryPersistentStore>()),
          audit(std::make_shared<AuditLogger>()),
          applier(std::make_shared<MockApplier>(true)) {
        store->initialize();
        queue = std::make_shared<ApprovalQueue>(store.get());
        queue->initialize();
        wf = std::make_shared<AutonomyApprovalWorkflow>(queue, audit);
        wf->setAutonomyEnabledOverride(true);
        wf->registerApplier(AutonomySource::CostOptimizer, applier);
    }
};

} // namespace

// ---------- 1. legal state transitions ------------------------------------

TEST(AutonomyApprovalWorkflowTest, StateMachineLegalTransitions) {
    WorkflowFixture f;

    // PROPOSED → APPROVED → APPLIED → ROLLED_BACK
    auto id1 = f.wf->propose(makeProp());
    ASSERT_FALSE(id1.empty());
    ASSERT_EQ(f.wf->get(id1)->state, ApprovalState::PROPOSED);
    EXPECT_TRUE(f.wf->approve(id1, "alice"));
    EXPECT_EQ(f.wf->get(id1)->state, ApprovalState::APPROVED);
    EXPECT_TRUE(f.wf->apply(id1));
    EXPECT_EQ(f.wf->get(id1)->state, ApprovalState::APPLIED);
    EXPECT_TRUE(f.wf->rollback(id1));
    EXPECT_EQ(f.wf->get(id1)->state, ApprovalState::ROLLED_BACK);

    // PROPOSED → REJECTED
    auto id2 = f.wf->propose(makeProp());
    EXPECT_TRUE(f.wf->reject(id2, "alice", "no thanks"));
    EXPECT_EQ(f.wf->get(id2)->state, ApprovalState::REJECTED);

    // APPROVED → REJECTED (allowed pre-apply per matrix §3.1)
    auto id3 = f.wf->propose(makeProp());
    EXPECT_TRUE(f.wf->approve(id3, "alice"));
    EXPECT_TRUE(f.wf->reject(id3, "alice", "changed mind"));
    EXPECT_EQ(f.wf->get(id3)->state, ApprovalState::REJECTED);
}

// ---------- 2. illegal state transitions ---------------------------------

TEST(AutonomyApprovalWorkflowTest, StateMachineIllegalTransitions) {
    WorkflowFixture f;

    auto id = f.wf->propose(makeProp());
    ASSERT_FALSE(id.empty());

    // PROPOSED → apply (must go via approve first)
    EXPECT_FALSE(f.wf->apply(id));
    EXPECT_EQ(f.wf->get(id)->state, ApprovalState::PROPOSED);

    // PROPOSED → rollback
    EXPECT_FALSE(f.wf->rollback(id));

    // APPROVED → approve again (no double-approve)
    ASSERT_TRUE(f.wf->approve(id, "alice"));
    EXPECT_FALSE(f.wf->approve(id, "bob"));

    // APPLIED → approve / reject / apply again
    ASSERT_TRUE(f.wf->apply(id));
    EXPECT_FALSE(f.wf->approve(id, "alice"));
    EXPECT_FALSE(f.wf->reject(id, "alice", "x"));
    EXPECT_FALSE(f.wf->apply(id));

    // ROLLED_BACK is terminal
    ASSERT_TRUE(f.wf->rollback(id));
    EXPECT_FALSE(f.wf->rollback(id));
    EXPECT_FALSE(f.wf->approve(id, "alice"));
    EXPECT_FALSE(f.wf->reject(id, "alice", "x"));
    EXPECT_FALSE(f.wf->apply(id));

    // REJECTED is terminal
    auto id_rej = f.wf->propose(makeProp());
    ASSERT_TRUE(f.wf->reject(id_rej, "alice", "nope"));
    EXPECT_FALSE(f.wf->approve(id_rej, "alice"));
    EXPECT_FALSE(f.wf->reject(id_rej, "alice", "again"));
    EXPECT_FALSE(f.wf->apply(id_rej));
    EXPECT_FALSE(f.wf->rollback(id_rej));
}

// ---------- 3. propose returns unique ULID -------------------------------

TEST(AutonomyApprovalWorkflowTest, ProposeReturnsUniqueUlid) {
    WorkflowFixture f;
    std::unordered_set<std::string> ids;
    for (int i = 0; i < 100; ++i) {
        auto id = f.wf->propose(makeProp());
        ASSERT_EQ(id.size(), 26u) << "ULID is 26 Crockford Base32 chars";
        EXPECT_TRUE(ids.insert(id).second) << "duplicate id: " << id;
    }
    EXPECT_EQ(ids.size(), 100u);
}

// ---------- 4. payload_sha256 tamper rejection (T01) ---------------------

TEST(AutonomyApprovalWorkflowTest, PayloadSha256TamperingRejected) {
    WorkflowFixture f;

    auto id = f.wf->propose(makeProp());
    ASSERT_TRUE(f.wf->approve(id, "alice"));

    // Tamper directly in the queue, bypassing the workflow API.
    auto cur = f.queue->get(id);
    ASSERT_TRUE(cur.has_value());
    cur->payload["k"] = "TAMPERED";
    ASSERT_TRUE(f.queue->update(*cur));

    EXPECT_FALSE(f.wf->apply(id)) << "tampered payload must be rejected";
    // State stays APPROVED — not transitioned to ROLLED_BACK because the
    // applier was never invoked, only the integrity gate fired.
    EXPECT_EQ(f.wf->get(id)->state, ApprovalState::APPROVED);
    EXPECT_EQ(f.applier->apply_calls_, 0);
}

// ---------- 5. apply failure → auto rollback (C1) ------------------------

TEST(AutonomyApprovalWorkflowTest, ApplyFailureAutoRollback) {
    WorkflowFixture f;
    f.applier->succeed_ = false;  // force failure

    auto id = f.wf->propose(makeProp());
    ASSERT_TRUE(f.wf->approve(id, "alice"));

    EXPECT_FALSE(f.wf->apply(id));
    EXPECT_EQ(f.wf->get(id)->state, ApprovalState::ROLLED_BACK);
    EXPECT_EQ(f.applier->apply_calls_, 1);
    EXPECT_EQ(f.applier->rollback_calls_, 1);
}

// ---------- 6. env AEGISGATE_DISABLE_AUTONOMY blocks propose (SR17) -----

TEST(AutonomyApprovalWorkflowTest, EnvDisableAutonomyBlocksPropose) {
    // Sanity check the static reader directly first.
    ::setenv("AEGISGATE_DISABLE_AUTONOMY", "1", 1);
    EXPECT_FALSE(AutonomyApprovalWorkflow::isAutonomyEnabled());
    ::unsetenv("AEGISGATE_DISABLE_AUTONOMY");
    EXPECT_TRUE(AutonomyApprovalWorkflow::isAutonomyEnabled());

    // End-to-end: an instance with no override defers to env.
    WorkflowFixture f;
    f.wf->setAutonomyEnabledOverride(std::nullopt);

    ::setenv("AEGISGATE_DISABLE_AUTONOMY", "1", 1);
    EXPECT_EQ(f.wf->propose(makeProp()), std::string())
        << "propose must be blocked when env kill switch is on";

    // Approve / apply / rollback are also disabled.
    auto seeded_id = std::string("01HNSEED000000000000000A1");
    {
        auto p = makeProp();
        p.id = seeded_id;
        p.proposer_user_id = "system";
        p.proposed_at_ms = 1716030000000LL;
        p.payload_sha256 = computePayloadSha256(p.payload);
        ASSERT_FALSE(f.queue->insert(p).empty());  // direct queue insert
    }
    EXPECT_FALSE(f.wf->approve(seeded_id, "alice"));
    EXPECT_FALSE(f.wf->reject(seeded_id, "alice", "x"));
    EXPECT_FALSE(f.wf->apply(seeded_id));
    EXPECT_FALSE(f.wf->rollback(seeded_id));

    ::unsetenv("AEGISGATE_DISABLE_AUTONOMY");
    // After unsetting, propose works again.
    auto id = f.wf->propose(makeProp());
    EXPECT_FALSE(id.empty());
}

// ---------- 7. audit logger records all transitions (T03) ---------------

TEST(AutonomyApprovalWorkflowTest, AuditLoggerRecordsAllTransitions) {
    WorkflowFixture f;

    auto id = f.wf->propose(makeProp());
    ASSERT_FALSE(id.empty());
    ASSERT_TRUE(f.wf->approve(id, "alice"));
    ASSERT_TRUE(f.wf->apply(id));
    ASSERT_TRUE(f.wf->rollback(id));

    auto entries = f.audit->entries();

    // Filter to entries belonging to this proposal.
    std::vector<AuditEntry> mine;
    for (const auto& e : entries) {
        if (e.request_id == id) mine.push_back(e);
    }
    ASSERT_EQ(mine.size(), 4u) << "propose+approve+apply+rollback = 4";
    EXPECT_EQ(mine[0].action, "propose");
    EXPECT_EQ(mine[1].action, "approve");
    EXPECT_EQ(mine[2].action, "apply");
    EXPECT_EQ(mine[3].action, "rollback");
    for (const auto& e : mine) {
        EXPECT_EQ(e.stage_name, "autonomy");
        EXPECT_EQ(e.tenant_id, "system");
        EXPECT_FALSE(e.detail.empty()) << "decision_trace + state recorded";
    }
    // T03: chain hash links must verify.
    EXPECT_TRUE(f.audit->verifyChain());
}

// ---------- 8. missing applier safeguard ---------------------------------

TEST(AutonomyApprovalWorkflowTest, MissingApplierRejectsApply) {
    WorkflowFixture f;

    auto p = makeProp(AutonomySource::AutoRecovery);  // not registered
    auto id = f.wf->propose(p);
    ASSERT_TRUE(f.wf->approve(id, "alice"));
    EXPECT_FALSE(f.wf->apply(id));
    EXPECT_EQ(f.wf->get(id)->state, ApprovalState::APPROVED)
        << "no applier → no state mutation";
}

// ---------- count() total matching, filter-aware, ignores paging ----------
// TASK-20260605-02 P1：FinOps 真分页需要过滤后的总数（不受 limit/offset 影响）。
TEST(AutonomyApprovalWorkflowTest, CountMatchesFilterIgnoringPaging) {
    WorkflowFixture f;
    f.wf->registerApplier(AutonomySource::AutoRecovery,
                          std::make_shared<MockApplier>(true));

    for (int i = 0; i < 3; ++i) f.wf->propose(makeProp(AutonomySource::CostOptimizer));
    for (int i = 0; i < 2; ++i) f.wf->propose(makeProp(AutonomySource::AutoRecovery));

    // 无过滤 → 全部 5 条，且与 limit/offset 无关。
    EXPECT_EQ(f.wf->count(std::nullopt, std::nullopt), 5);
    EXPECT_EQ(f.wf->list(std::nullopt, std::nullopt, 2, 0).size(), 2u);
    EXPECT_EQ(f.wf->count(std::nullopt, std::nullopt), 5);

    // source 过滤生效。
    EXPECT_EQ(f.wf->count(std::nullopt, AutonomySource::CostOptimizer), 3);
    EXPECT_EQ(f.wf->count(std::nullopt, AutonomySource::AutoRecovery), 2);

    // state 过滤生效（全部初始为 PROPOSED）。
    EXPECT_EQ(f.wf->count(ApprovalState::PROPOSED, std::nullopt), 5);
    EXPECT_EQ(f.wf->count(ApprovalState::APPLIED, std::nullopt), 0);
}
