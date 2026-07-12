// Phase 11.5 TASK-20260518-02 Epic 1.3 — IApprovalApplier compile + helper
// smoke tests.
//
// Plan §D Task 1.3 explicitly says "no tests (pure interface)", but we
// pull the header into a TU so it gets type-checked on every build and
// the ApplyResult::ok / fail factories don't silently rot. A minimal
// in-memory applier mock also serves as the seed example for subclasses.

#include "observe/autonomy/i_approval_applier.h"

#include <gtest/gtest.h>
#include <chrono>

using namespace aegisgate::autonomy;

namespace {

class NoopApplier : public IApprovalApplier {
public:
    ApplyResult apply(const ApprovalProposal& p, bool dry_run) override {
        (void)p;
        return ApplyResult::ok(
            nlohmann::json{{"dry_run", dry_run}, {"effect", "noop"}},
            std::chrono::milliseconds{1});
    }
    ApplyResult rollback(const ApprovalProposal& p) override {
        (void)p;
        return ApplyResult::ok();
    }
    std::string applierName() const override { return "NoopApplier"; }
};

class FailingApplier : public IApprovalApplier {
public:
    ApplyResult apply(const ApprovalProposal&, bool) override {
        return ApplyResult::fail("router_unavailable", "ml_router missing");
    }
    ApplyResult rollback(const ApprovalProposal&) override {
        return ApplyResult::ok();
    }
    std::string applierName() const override { return "FailingApplier"; }
    bool isLowRisk(const ApprovalProposal&) const override { return true; }
};

} // namespace

TEST(IApprovalApplierTest, OkFactoryPopulatesSuccess) {
    auto r = ApplyResult::ok(nlohmann::json{{"k", "v"}},
                              std::chrono::milliseconds{42});
    EXPECT_TRUE(r.success);
    EXPECT_TRUE(r.error_code.empty());
    EXPECT_EQ(r.details["k"], "v");
    EXPECT_EQ(r.duration_ms.count(), 42);
}

TEST(IApprovalApplierTest, FailFactoryPopulatesError) {
    auto r = ApplyResult::fail("router_unavailable", "ml_router is null",
                                nlohmann::json{{"attempt", 1}});
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_code, "router_unavailable");
    EXPECT_EQ(r.error_message, "ml_router is null");
    EXPECT_EQ(r.details["attempt"], 1);
}

TEST(IApprovalApplierTest, NoopApplierApplyAndRollbackBothSucceed) {
    NoopApplier applier;
    ApprovalProposal p;  // default-constructed is fine for a no-op applier
    auto a = applier.apply(p, /*dry_run=*/true);
    EXPECT_TRUE(a.success);
    EXPECT_EQ(a.details["dry_run"], true);

    auto rb = applier.rollback(p);
    EXPECT_TRUE(rb.success);

    EXPECT_EQ(applier.applierName(), "NoopApplier");
    EXPECT_FALSE(applier.isLowRisk(p)) << "default base behaviour";
}

TEST(IApprovalApplierTest, IsLowRiskCanBeOverridden) {
    FailingApplier f;
    ApprovalProposal p;
    EXPECT_TRUE(f.isLowRisk(p));

    auto a = f.apply(p, /*dry_run=*/false);
    EXPECT_FALSE(a.success);
    EXPECT_EQ(a.error_code, "router_unavailable");
}
