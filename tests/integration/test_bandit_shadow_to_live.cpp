// Phase 11.2 TASK-20260521-03 — Integration test: BanditRouter shadow-to-live
// lifecycle through the full AutonomyApprovalWorkflow stack.
//
// Validates that:
//   1. A proposal flows propose → approve → apply through the real
//      AutonomyApprovalWorkflow + AuditLogger + ApprovalQueue stack.
//   2. BanditRouter mode transitions correctly under the workflow's
//      dispatch, and rollback restores the mode.
//   3. The full chain is gated by AEGISGATE_DISABLE_AUTONOMY (SR17 reuse,
//      end-to-end this time — not just unit-level).

#include "observe/autonomy/approval_workflow.h"
#include "observe/autonomy/approval_queue.h"
#include "observe/autonomy/bandit_autonomy_applier.h"
#include "observe/autonomy/approval_proposal.h"
#include "gateway/bandit_router.h"
#include "gateway/router.h"
#include "guardrail/audit.h"
#include "storage/memory_persistent_store.h"

#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>

using namespace aegisgate;
using namespace aegisgate::autonomy;

namespace {

class StubBaseRouter : public Router {
public:
    std::string selectModel(RequestContext&,
                             const ConnectorRegistry&) override {
        return "stub-base";
    }
};

ApprovalProposal makeBanditProp(const std::string& action,
                                  double canary_pct,
                                  double win_rate,
                                  int shadow_duration_min,
                                  double cost_delta_pct) {
    ApprovalProposal p;
    p.source = AutonomySource::BanditRouter;
    p.subject = "phase11.2 integration";
    p.payload = {
        {"action", action},
        {"strategy", "cost-first"},
        {"canary_pct", canary_pct},
        {"shadow_metrics", {
            {"win_rate", win_rate},
            {"shadow_duration_min", shadow_duration_min},
            {"cost_delta_pct", cost_delta_pct}}}};
    p.decision_trace = {
        {"source_id", "BanditRouter"},
        {"algorithm_name", "thompson"},
        {"input_hash_sha256", std::string(64, 'b')},
        {"proposed_at_ms", 1716030000000LL}};
    return p;
}

struct IntegrationFixture {
    std::shared_ptr<MemoryPersistentStore> store;
    std::shared_ptr<ApprovalQueue> queue;
    std::shared_ptr<AuditLogger> audit;
    std::shared_ptr<AutonomyApprovalWorkflow> wf;
    std::shared_ptr<StubBaseRouter> base;
    std::shared_ptr<BanditRouter> bandit;
    std::shared_ptr<BanditAutonomyApplier> applier;

    IntegrationFixture()
        : store(std::make_shared<MemoryPersistentStore>()),
          audit(std::make_shared<AuditLogger>()),
          base(std::make_shared<StubBaseRouter>()),
          bandit(std::make_shared<BanditRouter>(base.get(),
                                                  BanditRouter::Config{})),
          applier(std::make_shared<BanditAutonomyApplier>(bandit)) {
        unsetenv("AEGISGATE_DISABLE_AUTONOMY");
        store->initialize();
        queue = std::make_shared<ApprovalQueue>(store.get());
        queue->initialize();
        wf = std::make_shared<AutonomyApprovalWorkflow>(queue, audit);
        wf->setAutonomyEnabledOverride(true);
        wf->registerApplier(AutonomySource::BanditRouter, applier);
    }
};

}  // namespace

TEST(BanditShadowToLiveIntegrationTest, FullLifecyclePromotesBandit) {
    IntegrationFixture f;
    ASSERT_EQ(f.bandit->getMode(), BanditMode::Shadow);

    auto p = makeBanditProp("shadow_to_live", 0.05, 0.65, 60, -10);
    auto id = f.wf->propose(p);
    ASSERT_FALSE(id.empty());
    EXPECT_EQ(f.wf->get(id)->state, ApprovalState::PROPOSED);

    ASSERT_TRUE(f.wf->approve(id, "alice"));
    EXPECT_EQ(f.wf->get(id)->state, ApprovalState::APPROVED);

    ASSERT_TRUE(f.wf->apply(id));
    EXPECT_EQ(f.wf->get(id)->state, ApprovalState::APPLIED);
    EXPECT_EQ(f.bandit->getMode(), BanditMode::Live);

    ASSERT_TRUE(f.wf->rollback(id));
    EXPECT_EQ(f.wf->get(id)->state, ApprovalState::ROLLED_BACK);
    EXPECT_EQ(f.bandit->getMode(), BanditMode::Shadow)
        << "BanditAutonomyApplier::rollback must restore Shadow mode";
}

TEST(BanditShadowToLiveIntegrationTest, HighRiskProposalRequiresManualApproval) {
    IntegrationFixture f;

    // canary too aggressive — isLowRisk MUST refuse auto-approval.
    auto p = makeBanditProp("shadow_to_live", 0.50, 0.65, 60, -10);
    EXPECT_FALSE(f.applier->isLowRisk(p))
        << "R3 canary_pct <= 0.05 must block 50% rollout";
}

TEST(BanditShadowToLiveIntegrationTest, EnvKillswitchBlocksFullChain) {
    IntegrationFixture f;
    auto p = makeBanditProp("shadow_to_live", 0.05, 0.65, 60, -10);
    auto id = f.wf->propose(p);
    ASSERT_FALSE(id.empty());
    ASSERT_TRUE(f.wf->approve(id, "alice"));

    setenv("AEGISGATE_DISABLE_AUTONOMY", "1", 1);
    bool applied = f.wf->apply(id);
    // The applier itself fail-closes; the workflow records ROLLED_BACK.
    EXPECT_FALSE(applied);
    EXPECT_EQ(f.bandit->getMode(), BanditMode::Shadow)
        << "Killswitch must prevent any live transition end-to-end";
    unsetenv("AEGISGATE_DISABLE_AUTONOMY");
}
