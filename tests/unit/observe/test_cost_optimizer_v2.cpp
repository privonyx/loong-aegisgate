// Phase 11.5 TASK-20260518-02 Epic 2.1 — CostOptimizer v2 propose path.
//
// 3 mandatory scenarios per plan §D Task 2.1 step 1:
//   1. propose_returns_proposal_ids        (every rec → ULID)
//   2. propose_decision_trace_contains_5_fields  (C4 schema)
//   3. propose_idempotent                  (dedup within window)
//
// The v1 getRecommendations() API surface stays untouched and is exercised
// by test_cost_optimizer.cpp; these tests target the new propose path only.

#include "observe/cost_optimizer.h"

#include "guardrail/audit.h"
#include "observe/autonomy/approval_queue.h"
#include "observe/autonomy/approval_workflow.h"
#include "storage/memory_persistent_store.h"

#include <gtest/gtest.h>
#include <memory>

using namespace aegisgate;
using namespace aegisgate::autonomy;

namespace {

CostOptimizerConfig defaultConfig() {
    CostOptimizerConfig c;
    c.enabled = true;
    c.min_quality_threshold       = 0.5;
    c.max_quality_loss            = 0.2;
    c.min_requests_for_recommendation = 5;  // low bar for tests
    return c;
}

// Seed two models with skewed cost/quality so the v1 algorithm emits a
// recommendation: current=expensive (high cost, marginally higher quality),
// recommended=cheap (low cost, slightly lower quality but still above
// min_quality_threshold).
void seedRecommendationData(CostOptimizer& opt) {
    for (int i = 0; i < 6; ++i) {
        opt.recordUsage("gpt-4o",      0.020, 0.95);  // expensive
        opt.recordUsage("gpt-4o-mini", 0.001, 0.85);  // cheap, lower quality
    }
}

struct V2Fixture {
    std::shared_ptr<MemoryPersistentStore> store;
    std::shared_ptr<ApprovalQueue>         queue;
    std::shared_ptr<AuditLogger>           audit;
    std::shared_ptr<AutonomyApprovalWorkflow> wf;

    V2Fixture()
        : store(std::make_shared<MemoryPersistentStore>()),
          audit(std::make_shared<AuditLogger>()) {
        store->initialize();
        queue = std::make_shared<ApprovalQueue>(store.get());
        queue->initialize();
        wf = std::make_shared<AutonomyApprovalWorkflow>(queue, audit);
        wf->setAutonomyEnabledOverride(true);
    }
};

} // namespace

TEST(CostOptimizerV2Test, ProposeReturnsProposalIds) {
    CostOptimizer opt(defaultConfig());
    seedRecommendationData(opt);
    V2Fixture f;

    auto ids = opt.proposeRecommendations("tenant-A", f.wf);
    ASSERT_FALSE(ids.empty()) << "v1 algo must emit at least one rec";
    for (const auto& id : ids) {
        EXPECT_EQ(id.size(), 26u);
        auto p = f.wf->get(id);
        ASSERT_TRUE(p.has_value());
        EXPECT_EQ(p->source, AutonomySource::CostOptimizer);
        EXPECT_EQ(p->state,  ApprovalState::PROPOSED);
    }
}

TEST(CostOptimizerV2Test, RecordUsageDoesNotAutoCreateProposals) {
    CostOptimizer opt(defaultConfig());
    V2Fixture f;

    seedRecommendationData(opt);
    ASSERT_FALSE(opt.getRecommendations().empty())
        << "test setup must be rich enough to produce a manual recommendation";
    EXPECT_EQ(f.queue->size(), 0u)
        << "recordUsage is data-plane only; proposals require explicit proposeRecommendations";
}

TEST(CostOptimizerV2Test, ProposeDecisionTraceContains5Fields) {
    CostOptimizer opt(defaultConfig());
    seedRecommendationData(opt);
    V2Fixture f;

    auto ids = opt.proposeRecommendations("tenant-B", f.wf);
    ASSERT_FALSE(ids.empty());
    auto p = f.wf->get(ids[0]);
    ASSERT_TRUE(p.has_value());

    const auto& trace = p->decision_trace;
    EXPECT_TRUE(trace.contains("source_id"))         << trace.dump();
    EXPECT_TRUE(trace.contains("algorithm_name"));
    EXPECT_TRUE(trace.contains("input_hash_sha256"));
    EXPECT_TRUE(trace.contains("proposed_at_ms"));
    EXPECT_TRUE(trace.contains("notes"))             << "optional but emitted by v2";
    EXPECT_EQ(trace.value("source_id", std::string{}),      "cost_optimizer");
    EXPECT_EQ(trace.value("algorithm_name", std::string{}), "cost_per_quality_v1");
    EXPECT_EQ(trace.value("input_hash_sha256", std::string{}).size(), 64u);
    EXPECT_GT(trace.value("proposed_at_ms", std::int64_t{0}), 0);
}

TEST(CostOptimizerV2Test, ProposeIsIdempotentWithinWindow) {
    CostOptimizer opt(defaultConfig());
    seedRecommendationData(opt);
    V2Fixture f;

    auto first = opt.proposeRecommendations(
        "tenant-C", f.wf, std::chrono::hours{1});
    ASSERT_FALSE(first.empty());

    // Re-propose immediately — every rec should be deduped, no new IDs.
    auto second = opt.proposeRecommendations(
        "tenant-C", f.wf, std::chrono::hours{1});
    EXPECT_TRUE(second.empty())
        << "dedup window must suppress repeat propose";

    // Different tenant uses an independent dedup key.
    auto other = opt.proposeRecommendations(
        "tenant-D", f.wf, std::chrono::hours{1});
    EXPECT_FALSE(other.empty()) << "tenant isolation in dedup";

    // Zero window disables dedup → re-propose succeeds again.
    auto third = opt.proposeRecommendations(
        "tenant-C", f.wf, std::chrono::seconds{0});
    EXPECT_FALSE(third.empty()) << "zero window bypasses dedup";
}
