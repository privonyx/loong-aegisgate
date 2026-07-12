// Phase 11.6 BaseAutonomyApplier (TASK-20260523-03) — Epic 1 task 1.1 (TDD RED).
//
// Unit tests for the GoF Template Method base class that all 5 Phase 11
// appliers (Cost / Bandit / Guard / Recovery / Workflow) will inherit
// from after the migration.
//
// The base is responsible for:
//   T1 — apply() delegates to applyImpl() when SR17 layer 2 is OPEN
//   T2 — apply() short-circuits to fail("autonomy_disabled") when SR17
//        is TRIPPED (AEGISGATE_DISABLE_AUTONOMY=1) without calling impl
//   T3 — apply() fills duration_ms via the base timing wrapper
//   T4 — rollback() short-circuits identically on SR17 trip
//   T5 — rollback() fills duration_ms identically
//   T6 — makeDryRunOk(details) helper sets dry_run=true
//   T7 — makeFailSchemaInvalid(err) helper uses canonical code/message
//   T8 — when applyImpl returns fail(), base still fills duration_ms
//   T9 — isLowRisk() is NOT intercepted by base (subclass-only territory)
//
// A test-only `TrackingApplier` subclass is used to assert hook delegation
// and impl-side counters.
//
// Design references:
//   docs/specs/2026-05-23-base-autonomy-applier-template-design.md §4.2, §6.2
//   docs/plans/2026-05-23-base-autonomy-applier-template.md Epic 1

#include "observe/autonomy/base_autonomy_applier.h"

#include <cstdlib>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <thread>

namespace aegisgate::autonomy {
namespace {

// Test helper — minimal concrete subclass.
class TrackingApplier : public BaseAutonomyApplier {
public:
    int  apply_calls    = 0;
    int  rollback_calls = 0;
    bool fail_next      = false;

    std::string applierName() const override { return "tracking_test"; }

protected:
    ApplyResult applyImpl(const ApprovalProposal& p, bool dry_run) override {
        ++apply_calls;
        if (fail_next) {
            return ApplyResult::fail("impl_failed", "stub");
        }
        if (dry_run) {
            return makeDryRunOk({{"observed_id", p.id}});
        }
        // Simulate some work so timing > 0.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return ApplyResult::ok({{"observed_id", p.id}});
    }

    ApplyResult rollbackImpl(const ApprovalProposal& p) override {
        ++rollback_calls;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return ApplyResult::ok({{"rolled_back_id", p.id}});
    }
};

class BaseAutonomyApplierTest : public ::testing::Test {
protected:
    void SetUp() override {
        ::unsetenv("AEGISGATE_DISABLE_AUTONOMY");
        proposal_.id = "prop-test-1";
    }
    void TearDown() override {
        ::unsetenv("AEGISGATE_DISABLE_AUTONOMY");
    }

    TrackingApplier   applier_;
    ApprovalProposal  proposal_;
};

// ---------------------------------------------------------------------------
// T1 — apply() delegates to applyImpl() when SR17 layer 2 is OPEN.
TEST_F(BaseAutonomyApplierTest, ApplyDelegatesToImplWhenAutonomyEnabled) {
    auto r = applier_.apply(proposal_, /*dry_run=*/false);
    EXPECT_TRUE(r.success) << r.error_code << ": " << r.error_message;
    EXPECT_EQ(applier_.apply_calls, 1);
    EXPECT_TRUE(r.details.contains("observed_id"));
    EXPECT_EQ(r.details["observed_id"], "prop-test-1");
}

// T2 — apply() short-circuits to fail("autonomy_disabled") under SR17 trip.
TEST_F(BaseAutonomyApplierTest, ApplyShortCircuitsWhenAutonomyDisabled) {
    ::setenv("AEGISGATE_DISABLE_AUTONOMY", "1", /*overwrite=*/1);
    auto r = applier_.apply(proposal_, /*dry_run=*/false);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_code, "autonomy_disabled");
    EXPECT_EQ(applier_.apply_calls, 0)
        << "applyImpl MUST NOT be called when SR17 is tripped";
}

// T3 — apply() fills duration_ms via the base timing wrapper.
TEST_F(BaseAutonomyApplierTest, ApplyFillsDurationMs) {
    auto r = applier_.apply(proposal_, /*dry_run=*/false);
    EXPECT_TRUE(r.success);
    EXPECT_GE(r.duration_ms.count(), 1)
        << "base apply() MUST fill duration_ms; got "
        << r.duration_ms.count();
}

// T4 — rollback() short-circuits identically on SR17 trip.
TEST_F(BaseAutonomyApplierTest, RollbackShortCircuitsWhenAutonomyDisabled) {
    ::setenv("AEGISGATE_DISABLE_AUTONOMY", "1", /*overwrite=*/1);
    auto r = applier_.rollback(proposal_);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_code, "autonomy_disabled");
    EXPECT_EQ(applier_.rollback_calls, 0);
}

// T5 — rollback() fills duration_ms identically.
TEST_F(BaseAutonomyApplierTest, RollbackFillsDurationMs) {
    auto r = applier_.rollback(proposal_);
    EXPECT_TRUE(r.success);
    EXPECT_GE(r.duration_ms.count(), 1);
}

// T6 — makeDryRunOk(details) helper sets dry_run=true and preserves details.
TEST_F(BaseAutonomyApplierTest, MakeDryRunOkSetsDryRunDetail) {
    auto r = applier_.apply(proposal_, /*dry_run=*/true);
    EXPECT_TRUE(r.success);
    ASSERT_TRUE(r.details.is_object());
    EXPECT_TRUE(r.details.value("dry_run", false));
    EXPECT_EQ(r.details.value("observed_id", std::string{}), "prop-test-1");
}

// T7 — makeFailSchemaInvalid(err) helper uses the canonical code/message.
TEST_F(BaseAutonomyApplierTest, MakeFailSchemaInvalidProducesStandardCode) {
    auto r = BaseAutonomyApplier::makeFailSchemaInvalid("missing tenant_id");
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_code, "schema_invalid");
    EXPECT_EQ(r.error_message, "missing tenant_id");
}

// T8 — when applyImpl returns fail(), base still fills duration_ms.
TEST_F(BaseAutonomyApplierTest, ApplyImplCanReturnFailAndBaseStillFillsDuration) {
    applier_.fail_next = true;
    auto r = applier_.apply(proposal_, /*dry_run=*/false);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_code, "impl_failed");
    // duration_ms is filled even when impl reported failure — required so
    // the audit trail still records how long the failing path took.
    EXPECT_GE(r.duration_ms.count(), 0);
}

// T9 — isLowRisk() is NOT intercepted by base (subclass-only territory).
TEST_F(BaseAutonomyApplierTest, IsLowRiskNotInterceptedByBase) {
    // TrackingApplier doesn't override isLowRisk; default from
    // IApprovalApplier is `false`. The base MUST NOT change that.
    EXPECT_FALSE(applier_.isLowRisk(proposal_));
}

// ---------------------------------------------------------------------------
// Extra — makeFailMissingDep helper canonical shape.
TEST_F(BaseAutonomyApplierTest, MakeFailMissingDepStandardCode) {
    auto r =
        BaseAutonomyApplier::makeFailMissingDep("router_missing", "no router");
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_code, "router_missing");
    EXPECT_EQ(r.error_message, "no router");
}

} // namespace
} // namespace aegisgate::autonomy
