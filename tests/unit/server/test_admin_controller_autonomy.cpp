// Phase 11.5 (TASK-20260518-02 E5.1) — AdminController autonomy endpoints.
//
// 6 RED→GREEN tests covering the 5 /admin/api/autonomy/* endpoints:
//   T1: GET /proposals returns list with state + source filters
//   T2: POST /{id}/approve requires TenantAdmin RBAC
//   T3: POST /{id}/reject requires non-empty reason in body
//   T4: DELETE /{id} only accepted from APPLIED → ROLLED_BACK
//   T5: GET /report aggregates by source × state + sums savings
//   T6: every endpoint writes one audit entry on the happy path
//
// Storage backing is MemoryPersistentStore so the workflow's queue.init
// works without a SQLite/PG dependency.

#include "server/admin_controller.h"

#include "auth/auth_service.h"
#include "guardrail/audit.h"
#include "observe/autonomy/approval_queue.h"
#include "observe/autonomy/approval_workflow.h"
#include "observe/autonomy/cost_autonomy_applier.h"
#include "storage/memory_persistent_store.h"

#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>

using namespace aegisgate;
using json = nlohmann::json;

namespace {

// A lightweight applier that records calls and lets us drive APPLIED →
// ROLLED_BACK without needing the real CostAutonomyApplier + MLRouter.
class FakeApplier : public autonomy::IApprovalApplier {
public:
    autonomy::ApplyResult apply(const autonomy::ApprovalProposal& /*p*/,
                                 bool /*dry_run*/) override {
        ++apply_count;
        if (next_apply_succeeds) return autonomy::ApplyResult::ok();
        return autonomy::ApplyResult::fail("fake_apply_fail", "fake apply fail");
    }
    autonomy::ApplyResult rollback(
        const autonomy::ApprovalProposal& /*p*/) override {
        ++rollback_count;
        return autonomy::ApplyResult::ok();
    }
    std::string applierName() const override { return "fake"; }
    bool isLowRisk(const autonomy::ApprovalProposal& /*p*/) const override {
        return true;
    }

    int apply_count = 0;
    int rollback_count = 0;
    bool next_apply_succeeds = true;
};

} // namespace

class AdminControllerAutonomyTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Make sure the SR17 kill switch is unset for these tests.
        unsetenv("AEGISGATE_DISABLE_AUTONOMY");

        store_.initialize();
        gate_ = std::make_unique<FeatureGate>(
            FeatureGate::createUnlocked(Edition::Enterprise));
        auth_svc_ = std::make_unique<AuthService>(&store_, nullptr,
                                                   gate_.get());
        audit_ = std::make_unique<AuditLogger>();

        queue_ = std::make_shared<autonomy::ApprovalQueue>(&store_);
        ASSERT_TRUE(queue_->initialize());

        // Non-owning shared_ptr<AuditLogger> for the workflow signature.
        auto audit_alias =
            std::shared_ptr<AuditLogger>(audit_.get(), [](AuditLogger*) {});
        workflow_ = std::make_shared<autonomy::AutonomyApprovalWorkflow>(
            queue_, audit_alias, nullptr);
        workflow_->setAutonomyEnabledOverride(true);

        applier_ = std::make_shared<FakeApplier>();
        workflow_->registerApplier(autonomy::AutonomySource::CostOptimizer,
                                    applier_);

        ctrl_ = std::make_unique<AdminController>(&store_, auth_svc_.get(),
                                                   audit_.get());
        ctrl_->setAutonomyWorkflow(workflow_.get());
        ctrl_->setAutonomyQueue(queue_.get());

        admin_ctx_.role = Role::TenantAdmin;
        admin_ctx_.tenant_id = "t1";
        admin_ctx_.user_id = "u-admin";
        admin_ctx_.is_rbac_enabled = true;

        dev_ctx_.role = Role::Developer;
        dev_ctx_.tenant_id = "t1";
        dev_ctx_.user_id = "u-dev";
        dev_ctx_.is_rbac_enabled = true;
    }

    void TearDown() override {
        if (audit_) audit_->shutdown();
    }

    // Submits a fresh PROPOSED proposal carrying the standard cost-optimizer
    // payload schema. Returns the workflow-minted id.
    std::string submit(double savings_usd_24h = 12.5,
                       const std::string& subject = "p-cost") {
        autonomy::ApprovalProposal p;
        p.source = autonomy::AutonomySource::CostOptimizer;
        p.subject = subject;
        p.payload = json{
            {"action", "override_quality_tier"},
            {"tenant_id", "t1"},
            {"current_model", "gpt-4o"},
            {"recommended_model", "gpt-4o-mini"},
            {"from_quality_tier", "premium"},
            {"to_quality_tier", "standard"},
            {"estimated_savings_usd_24h", savings_usd_24h},
            {"affected_requests_per_hour", 100},
        };
        p.decision_trace = json{
            {"source_id", "cost-optimizer-v2"},
            {"algorithm_name", "savings-ranker"},
            {"input_hash_sha256",
             "0000000000000000000000000000000000000000000000000000000000000000"},
            {"proposed_at_ms", 1700000000000},
        };
        return workflow_->propose(std::move(p));
    }

    MemoryPersistentStore store_;
    std::unique_ptr<FeatureGate> gate_;
    std::unique_ptr<AuthService> auth_svc_;
    std::unique_ptr<AuditLogger> audit_;
    std::shared_ptr<autonomy::ApprovalQueue> queue_;
    std::shared_ptr<autonomy::AutonomyApprovalWorkflow> workflow_;
    std::shared_ptr<FakeApplier> applier_;
    std::unique_ptr<AdminController> ctrl_;
    AuthContext admin_ctx_, dev_ctx_;
};

// --- T1: list returns proposals with filter ---------------------------------

TEST_F(AdminControllerAutonomyTest, ListProposalsWithFilter) {
    ASSERT_FALSE(submit(10.0, "p-A").empty());
    ASSERT_FALSE(submit(20.0, "p-B").empty());

    auto all = ctrl_->listAutonomyProposals(admin_ctx_, "", "", 100, 0);
    EXPECT_EQ(all.status, 200);
    EXPECT_EQ(all.body["data"].size(), 2u);

    // Filter by state = PROPOSED → still 2
    auto proposed = ctrl_->listAutonomyProposals(
        admin_ctx_, "PROPOSED", "", 100, 0);
    EXPECT_EQ(proposed.status, 200);
    EXPECT_EQ(proposed.body["data"].size(), 2u);

    // Filter by source = CostOptimizer → still 2
    auto by_src = ctrl_->listAutonomyProposals(
        admin_ctx_, "", "CostOptimizer", 100, 0);
    EXPECT_EQ(by_src.status, 200);
    EXPECT_EQ(by_src.body["data"].size(), 2u);

    // Filter by state = APPLIED → 0 (none applied yet)
    auto applied = ctrl_->listAutonomyProposals(
        admin_ctx_, "APPLIED", "", 100, 0);
    EXPECT_EQ(applied.status, 200);
    EXPECT_EQ(applied.body["data"].size(), 0u);

    // Bogus state filter → 400
    auto bad = ctrl_->listAutonomyProposals(admin_ctx_, "MOOSE", "", 100, 0);
    EXPECT_EQ(bad.status, 400);
}

// TASK-20260605-02 P1：total 反映过滤后的全量，独立于 limit/offset（供 FinOps 翻页）。
TEST_F(AdminControllerAutonomyTest, ListProposalsReturnsTotalIndependentOfPaging) {
    for (int i = 0; i < 5; ++i) {
        ASSERT_FALSE(submit(static_cast<double>(i + 1), "p-" + std::to_string(i)).empty());
    }

    // 第一页 limit=2 → data 2 条，但 total=5。
    auto page = ctrl_->listAutonomyProposals(admin_ctx_, "", "", 2, 0);
    ASSERT_EQ(page.status, 200);
    EXPECT_EQ(page.body["data"].size(), 2u);
    ASSERT_TRUE(page.body.contains("total"));
    EXPECT_EQ(page.body["total"].get<int>(), 5);

    // state 过滤后的 total。
    auto applied = ctrl_->listAutonomyProposals(admin_ctx_, "APPLIED", "", 100, 0);
    ASSERT_EQ(applied.status, 200);
    EXPECT_EQ(applied.body["total"].get<int>(), 0);
}

// SR-3：list 端点最低 TenantAdmin；Developer → 403（不得读 total/列表）。
TEST_F(AdminControllerAutonomyTest, ListProposalsRequiresTenantAdmin) {
    EXPECT_EQ(ctrl_->listAutonomyProposals(dev_ctx_, "", "", 100, 0).status, 403);
}

// --- T2: approve requires TenantAdmin RBAC ----------------------------------

TEST_F(AdminControllerAutonomyTest, ApproveRequiresAdminRole) {
    auto id = submit();
    ASSERT_FALSE(id.empty());

    // Developer is denied
    auto denied = ctrl_->approveAutonomyProposal(dev_ctx_, id);
    EXPECT_EQ(denied.status, 403);

    // TenantAdmin succeeds — state PROPOSED → APPROVED
    auto ok = ctrl_->approveAutonomyProposal(admin_ctx_, id);
    EXPECT_EQ(ok.status, 200);
    EXPECT_EQ(ok.body["state"], "APPROVED");
    EXPECT_EQ(ok.body["reviewer_user_id"], "u-admin");

    auto after = queue_->get(id);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->state, autonomy::ApprovalState::APPROVED);
}

// --- T3: reject requires reason ---------------------------------------------

TEST_F(AdminControllerAutonomyTest, RejectRequiresReason) {
    auto id = submit();
    ASSERT_FALSE(id.empty());

    auto missing = ctrl_->rejectAutonomyProposal(admin_ctx_, id,
                                                  json::object());
    EXPECT_EQ(missing.status, 400);

    auto empty = ctrl_->rejectAutonomyProposal(admin_ctx_, id,
                                                json{{"reason", ""}});
    EXPECT_EQ(empty.status, 400);

    auto ok = ctrl_->rejectAutonomyProposal(
        admin_ctx_, id, json{{"reason", "Risk too high"}});
    EXPECT_EQ(ok.status, 200);
    EXPECT_EQ(ok.body["state"], "REJECTED");
    EXPECT_EQ(ok.body["reject_reason"], "Risk too high");
}

// --- T4: rollback only from APPLIED, calls applier->rollback ---------------

TEST_F(AdminControllerAutonomyTest, RollbackTransitionsAppliedToRolledBack) {
    auto id = submit();
    ASSERT_FALSE(id.empty());

    // Try to rollback from PROPOSED → 409 ApprovalStateInvalid
    auto wrong = ctrl_->rollbackAutonomyProposal(admin_ctx_, id);
    EXPECT_EQ(wrong.status, 409);

    // Drive PROPOSED → APPROVED → APPLIED, then rollback.
    ASSERT_TRUE(workflow_->approve(id, "u-admin"));
    ASSERT_TRUE(workflow_->apply(id));
    EXPECT_EQ(applier_->apply_count, 1);

    auto ok = ctrl_->rollbackAutonomyProposal(admin_ctx_, id);
    EXPECT_EQ(ok.status, 200);
    EXPECT_EQ(ok.body["state"], "ROLLED_BACK");
    EXPECT_EQ(applier_->rollback_count, 1);

    auto after = queue_->get(id);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->state, autonomy::ApprovalState::ROLLED_BACK);
}

// --- T5: report aggregates by source + sums savings -------------------------

TEST_F(AdminControllerAutonomyTest, ReportAggregatesBySourceAndState) {
    // 3 proposals: 1 stays PROPOSED, 1 becomes REJECTED, 1 becomes APPLIED.
    auto pid_proposed = submit(5.0,  "p-stay");
    auto pid_reject   = submit(7.0,  "p-rej");
    auto pid_apply    = submit(11.0, "p-app");
    ASSERT_FALSE(pid_proposed.empty());
    ASSERT_FALSE(pid_reject.empty());
    ASSERT_FALSE(pid_apply.empty());

    ASSERT_TRUE(workflow_->reject(pid_reject, "u-admin", "test"));
    ASSERT_TRUE(workflow_->approve(pid_apply, "u-admin"));
    ASSERT_TRUE(workflow_->apply(pid_apply));

    auto r = ctrl_->autonomyReport(admin_ctx_);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["sample_size"], 3);
    EXPECT_EQ(r.body["totals"]["PROPOSED"], 1);
    EXPECT_EQ(r.body["totals"]["REJECTED"], 1);
    EXPECT_EQ(r.body["totals"]["APPLIED"], 1);

    EXPECT_EQ(r.body["by_source"]["CostOptimizer"]["PROPOSED"], 1);
    EXPECT_EQ(r.body["by_source"]["CostOptimizer"]["REJECTED"], 1);
    EXPECT_EQ(r.body["by_source"]["CostOptimizer"]["APPLIED"], 1);

    // Only the APPLIED proposal contributes to estimated_savings_24h_usd.
    EXPECT_DOUBLE_EQ(
        r.body["estimated_savings_24h_usd"].get<double>(), 11.0);
}

// --- T6: every endpoint writes audit ----------------------------------------

TEST_F(AdminControllerAutonomyTest, EveryEndpointWritesAudit) {
    auto id = submit();
    ASSERT_FALSE(id.empty());
    // Capture the pre-test audit-entry count so we only assert on the
    // entries written by *this* test sequence.  AuditLogger queues entries
    // on a background thread; flush() before reading.
    ASSERT_TRUE(audit_->flush(std::chrono::seconds{2}));
    const size_t baseline = audit_->entries().size();

    EXPECT_EQ(
        ctrl_->listAutonomyProposals(admin_ctx_, "", "", 10, 0).status, 200);
    EXPECT_EQ(ctrl_->approveAutonomyProposal(admin_ctx_, id).status, 200);

    // Submit a second one to test the reject path without disturbing the
    // first.
    auto id2 = submit();
    ASSERT_FALSE(id2.empty());
    ASSERT_TRUE(audit_->flush(std::chrono::seconds{2}));
    const size_t after_proposes_and_first_two =
        audit_->entries().size();

    EXPECT_EQ(ctrl_->rejectAutonomyProposal(
                  admin_ctx_, id2, json{{"reason", "no"}}).status, 200);

    // Apply id then rollback so we exercise both the apply audit (from the
    // workflow internals) and the rollback audit (from the controller).
    ASSERT_TRUE(workflow_->apply(id));
    EXPECT_EQ(ctrl_->rollbackAutonomyProposal(admin_ctx_, id).status, 200);
    EXPECT_EQ(ctrl_->autonomyReport(admin_ctx_).status, 200);

    ASSERT_TRUE(audit_->flush(std::chrono::seconds{2}));
    const auto total = audit_->entries().size();

    // The controller wrote >= 5 audit entries: list / approve / reject /
    // rollback / report. Workflow internals add a few more (propose,
    // approve, apply, rollback transitions) which is fine — we just need
    // at least the 5 controller entries to be present, beyond the noise
    // already in the queue.
    EXPECT_GE(total - baseline, 5u);
    EXPECT_GT(total, after_proposes_and_first_two);

    // Spot-check that an "autonomy.list" action is in there.
    bool saw_list = false;
    bool saw_report = false;
    for (const auto& e : audit_->entries()) {
        if (e.action == "autonomy.list") saw_list = true;
        if (e.action == "autonomy.report") saw_report = true;
    }
    EXPECT_TRUE(saw_list);
    EXPECT_TRUE(saw_report);
}

// --- Bonus: when autonomy wiring is null, every endpoint returns 6002 -------

TEST_F(AdminControllerAutonomyTest, NullWiringYieldsAutonomyDisabled) {
    AdminController bare(&store_, auth_svc_.get(), audit_.get());
    // No setAutonomyWorkflow / setAutonomyQueue → both null.
    auto r = bare.listAutonomyProposals(admin_ctx_, "", "", 10, 0);
    EXPECT_EQ(r.status, 503);
    EXPECT_TRUE(r.is_error);
    EXPECT_EQ(r.error_code, ErrorCode::AutonomyDisabled);
}
