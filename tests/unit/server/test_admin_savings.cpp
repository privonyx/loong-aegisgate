#include <gtest/gtest.h>

#include <chrono>
#include <memory>

#include "auth/auth_service.h"
#include "guardrail/audit.h"
#include "observe/cost_tracker.h"
#include "observe/savings_aggregator.h"
#include "server/admin_controller.h"
#include "storage/memory_persistent_store.h"

using namespace aegisgate;

class AdminSavingsTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_.initialize();
        gate_ = std::make_unique<FeatureGate>(
            FeatureGate::createUnlocked(Edition::Enterprise));
        auth_svc_ = std::make_unique<AuthService>(&store_, nullptr, gate_.get());
        audit_ = std::make_unique<AuditLogger>();

        tracker_.setPricing("gpt-4", 0.03, 0.06);
        tracker_.setPricing("gpt-3.5", 0.001, 0.002);
        aggregator_ = std::make_unique<SavingsAggregator>(&tracker_);

        ctrl_ = std::make_unique<AdminController>(
            &store_, auth_svc_.get(), audit_.get(),
            nullptr, nullptr, aggregator_.get());

        super_ctx_.role = Role::SuperAdmin;
        super_ctx_.tenant_id = "t-super";
        super_ctx_.is_rbac_enabled = true;

        tenant_admin_ctx_.role = Role::TenantAdmin;
        tenant_admin_ctx_.tenant_id = "tenant-A";
        tenant_admin_ctx_.is_rbac_enabled = true;

        viewer_ctx_.role = Role::Viewer;
        viewer_ctx_.tenant_id = "tenant-X";
        viewer_ctx_.is_rbac_enabled = true;
    }

    void TearDown() override {
        if (audit_) audit_->shutdown();
    }

    MemoryPersistentStore store_;
    std::unique_ptr<FeatureGate> gate_;
    std::unique_ptr<AuthService> auth_svc_;
    std::unique_ptr<AuditLogger> audit_;
    CostTracker tracker_;
    std::unique_ptr<SavingsAggregator> aggregator_;
    std::unique_ptr<AdminController> ctrl_;

    AuthContext super_ctx_, tenant_admin_ctx_, viewer_ctx_;
};

TEST_F(AdminSavingsTest, EmptyAggregator_ReturnsZeroSkeleton) {
    auto res = ctrl_->getSavingsSummary(super_ctx_, "", "", "");
    ASSERT_FALSE(res.is_error) << res.body.dump();
    EXPECT_DOUBLE_EQ(0.0, res.body["total_cost_saved"].get<double>());
    EXPECT_EQ(0, res.body["total_tokens_saved"].get<int>());
    EXPECT_EQ(0, res.body["total_cache_hits"].get<int>());
    EXPECT_TRUE(res.body["by_type"].is_array());
    EXPECT_TRUE(res.body["by_model"].is_array());
    EXPECT_TRUE(res.body["time_series"].is_array());
    EXPECT_TRUE(res.body["top_tenants"].is_array());
}

TEST_F(AdminSavingsTest, SuperAdmin_GlobalView_TopTenantsSorted) {
    aggregator_->recordCacheHit("gpt-4", 100, 100, "tenant-A");  // tokens=200
    aggregator_->recordCacheHit("gpt-4", 200, 200, "tenant-B");  // tokens=400
    aggregator_->recordCacheHit("gpt-4", 50, 50, "tenant-C");    // tokens=100

    auto res = ctrl_->getSavingsSummary(super_ctx_, "", "", "");
    ASSERT_FALSE(res.is_error);
    ASSERT_EQ(3u, res.body["top_tenants"].size());
    // 排序按 cost_saved 降序：gpt-4 单价相同，故 token 多者 cost 多
    EXPECT_EQ("tenant-B", res.body["top_tenants"][0]["tenant_id"]);
    EXPECT_EQ("tenant-A", res.body["top_tenants"][1]["tenant_id"]);
    EXPECT_EQ("tenant-C", res.body["top_tenants"][2]["tenant_id"]);
    EXPECT_EQ(700, res.body["total_tokens_saved"].get<int>());
    EXPECT_EQ(3, res.body["total_cache_hits"].get<int>());
}

TEST_F(AdminSavingsTest, TenantAdmin_OnlySelfTenant_NoTopTenants) {
    aggregator_->recordCacheHit("gpt-4", 100, 100, "tenant-A");
    aggregator_->recordCacheHit("gpt-4", 200, 200, "tenant-B");

    auto res = ctrl_->getSavingsSummary(tenant_admin_ctx_, "", "", "");
    ASSERT_FALSE(res.is_error);
    EXPECT_EQ(0u, res.body["top_tenants"].size());  // SR1：非 SuperAdmin 无排行
    // 仅 tenant-A 数据：100+100=200 tokens
    EXPECT_EQ(200, res.body["total_tokens_saved"].get<int>());
}

TEST_F(AdminSavingsTest, Viewer_CrossTenantQuery_Rejected) {
    // viewer 在 tenant-X，试图查 tenant-Y → 403
    auto res = ctrl_->getSavingsSummary(viewer_ctx_, "", "", "tenant-Y");
    EXPECT_TRUE(res.is_error);
    EXPECT_EQ(ErrorCode::InsufficientPermissions, res.error_code);
}

TEST_F(AdminSavingsTest, TimeWindow_ExceedingMax_Rejected) {
    auto res = ctrl_->getSavingsSummary(
        super_ctx_, "2020-01-01T00:00:00", "2026-12-31T23:59:59", "");
    ASSERT_TRUE(res.is_error);  // SR-NEW3：> 365 天
    EXPECT_EQ(ErrorCode::InvalidRequest, res.error_code);
}

TEST_F(AdminSavingsTest, FallbackPricingCount_Exposed) {
    // unknown-model 在 CostTracker 中无 pricing → cost=0 + fallback_pricing=true
    aggregator_->recordCacheHit("unknown-model", 100, 100, "tenant-X");
    aggregator_->recordCacheHit("gpt-4", 100, 100, "tenant-X");  // 已知

    auto res = ctrl_->getSavingsSummary(viewer_ctx_, "", "", "");
    ASSERT_FALSE(res.is_error);
    EXPECT_EQ(1, res.body["fallback_pricing_count"].get<int>());
}

TEST_F(AdminSavingsTest, ByType_ContainsCacheHit_Compression_Routing) {
    aggregator_->recordCacheHit("gpt-4", 100, 100, "tenant-X");
    aggregator_->recordCompression("gpt-4", 50, "tenant-X");
    aggregator_->recordRouting("gpt-4", "gpt-3.5", 1.5, "tenant-X");

    auto res = ctrl_->getSavingsSummary(viewer_ctx_, "", "", "");
    ASSERT_FALSE(res.is_error);
    ASSERT_EQ(3u, res.body["by_type"].size());

    bool has_cache_hit = false, has_compression = false, has_routing = false;
    for (const auto& entry : res.body["by_type"]) {
        const auto t = entry["type"].get<std::string>();
        if (t == "cache_hit") has_cache_hit = true;
        if (t == "compression") has_compression = true;
        if (t == "routing_potential") has_routing = true;
    }
    EXPECT_TRUE(has_cache_hit);
    EXPECT_TRUE(has_compression);
    EXPECT_TRUE(has_routing);

    // routing_recommendations 数组应含 "gpt-4->gpt-3.5"
    ASSERT_GE(res.body["routing_recommendations"].size(), 1u);
    EXPECT_EQ("gpt-4->gpt-3.5",
              res.body["routing_recommendations"][0]["route"].get<std::string>());
}

TEST_F(AdminSavingsTest, NoAggregatorInjected_ReturnsEmptySkeleton) {
    // 用未注入 aggregator 的 ctrl 验证 graceful degradation
    AdminController ctrl_no_agg(
        &store_, auth_svc_.get(), audit_.get(),
        nullptr, nullptr, nullptr);
    auto res = ctrl_no_agg.getSavingsSummary(super_ctx_, "", "", "");
    ASSERT_FALSE(res.is_error);
    EXPECT_DOUBLE_EQ(0.0, res.body["total_cost_saved"].get<double>());
    EXPECT_TRUE(res.body["aggregator_since"].is_null());
}

// --- Security events ---

TEST_F(AdminSavingsTest, SecurityEvents_SuperAdmin_RawCounts) {
    auto res = ctrl_->getSecurityEvents(super_ctx_);
    ASSERT_FALSE(res.is_error);
    EXPECT_EQ("global", res.body["scope"].get<std::string>());
    EXPECT_TRUE(res.body.contains("guardrail_blocks_total"));
    EXPECT_TRUE(res.body.contains("preprocessor_normalized_total"));
    EXPECT_TRUE(res.body.contains("rate_limited_total"));
}

TEST_F(AdminSavingsTest, SecurityEvents_NonSuperAdmin_OnlySeverity) {
    auto res = ctrl_->getSecurityEvents(viewer_ctx_);
    ASSERT_FALSE(res.is_error);
    EXPECT_EQ("tenant", res.body["scope"].get<std::string>());
    // 非 SuperAdmin 看不到原始 counts（避免暴露其它租户安全态势）
    EXPECT_FALSE(res.body.contains("guardrail_blocks_total"));
    EXPECT_TRUE(res.body.contains("guardrail_blocks_severity"));
}

// -----------------------------------------------------------------------
// TASK-20260711-01 / REV20260707-I13 Epic 3 — Savings API edition gate.
// -----------------------------------------------------------------------

// SR-4: Community edition (no license) cannot use the Savings summary
// endpoint even though RBAC passes. Returns 403 InsufficientPermissions
// with a license-specific message. Reuses the existing error code
// (D2 = A: no error_codes.h churn; message differentiates license from
// role denial).
TEST_F(AdminSavingsTest, GetSavingsSummary_CommunityEdition_Returns403_SR4) {
    FeatureGate community_gate(Edition::Community);
    AdminController ctrl_gated(
        &store_, auth_svc_.get(), audit_.get(),
        nullptr, nullptr, aggregator_.get(),
        nullptr, &community_gate);

    // Any authenticated role reaches getSavingsSummary; RBAC would pass.
    auto res = ctrl_gated.getSavingsSummary(super_ctx_, "", "", "");
    EXPECT_EQ(res.status, 403)
        << "Community edition must be blocked from Savings summary "
        << "(AdvancedRouting license required).";
    EXPECT_TRUE(res.is_error);
    // Error message must call out the license (not a role denial) so
    // the front-end can present an upgrade CTA rather than "please
    // contact an admin".
    ASSERT_TRUE(res.body.contains("error"));
    auto message = res.body["error"].value("message", std::string{});
    EXPECT_NE(message.find("Enterprise"), std::string::npos)
        << "Error message should mention 'Enterprise' to distinguish "
        << "from role-denied. Got: " << message;
}

// SR-5: Enterprise edition (license unlocked) — savings endpoint works
// normally, no regression. The test-class default `ctrl_` uses
// createUnlocked(Enterprise) as its FeatureGate but wires it only into
// auth_svc_ (not into AdminController). This test therefore builds a
// second controller with the same unlocked gate wired for I13.
TEST_F(AdminSavingsTest, GetSavingsSummary_EnterpriseEdition_ReturnsData_SR5) {
    AdminController ctrl_gated(
        &store_, auth_svc_.get(), audit_.get(),
        nullptr, nullptr, aggregator_.get(),
        nullptr, gate_.get());

    auto res = ctrl_gated.getSavingsSummary(super_ctx_, "", "", "");
    EXPECT_EQ(res.status, 200);
    EXPECT_FALSE(res.is_error);
}

// SR-6: Nullable-safe — legacy callers not wiring the FeatureGate*
// argument continue to work exactly as before (fall-open). Every
// pre-existing test in this file uses the 7-arg AdminController ctor
// and remains untouched; this test just makes the contract explicit.
TEST_F(AdminSavingsTest, GetSavingsSummary_NullFeatureGate_FallsOpen_SR6) {
    // 7-arg constructor: feature_gate defaults to nullptr.
    AdminController ctrl_legacy(
        &store_, auth_svc_.get(), audit_.get(),
        nullptr, nullptr, aggregator_.get());

    auto res = ctrl_legacy.getSavingsSummary(super_ctx_, "", "", "");
    EXPECT_EQ(res.status, 200)
        << "Nullable FeatureGate must fall open (legacy behavior).";
    EXPECT_FALSE(res.is_error);
}
