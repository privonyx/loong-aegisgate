// TASK-20260527-02 Epic 3.2 — Case Study Numbers endpoint.
//
// 测试 AdminController::caseStudyHeadline(ctx) 应用层逻辑（spec §3.3）：
//   - HeadlineReturnsAllFields: 3 顶层字段（saved_vs_baseline / cache_hit_by_type /
//     quality_reason）+ scope 全部存在
//   - RbacFiltersTenant (SR1): Admin 仅看本租户 / SuperAdmin 看全局聚合
//   - EmptyDataReturnsZeroSkeleton: 数据源未注入时不 500，返回 0 占位 schema
//
// 注：HTTP 401（未认证）由 AdminHttpController::authenticateRequest 处理，
// 控制器层进入 caseStudyHeadline 时 ctx 必非空。Role::Viewer 是最小角色
// (= 0)，所以"InsufficientPermissions 403"在此 endpoint 路径上不会触发。

#include "auth/auth_service.h"
#include "cache/embedder.h"
#include "cache/hnsw_vector_store.h"
#include "cache/semantic_cache.h"
#include "guardrail/audit.h"
#include "observe/cost_tracker.h"
#include "observe/quality_monitor.h"
#include "observe/savings_aggregator.h"
#include "server/admin_controller.h"
#include "storage/memory_persistent_store.h"
#include <gtest/gtest.h>
#include <chrono>

using namespace aegisgate;
using namespace std::chrono_literals;
using json = nlohmann::json;

class AdminCaseStudyTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_.initialize();
        gate_ = std::make_unique<FeatureGate>(
            FeatureGate::createUnlocked(Edition::Enterprise));
        auth_svc_ = std::make_unique<AuthService>(&store_, nullptr, gate_.get());
        audit_ = std::make_unique<AuditLogger>();

        // 喂数据：CostTracker 收 2 条 record（不同 tenant）。
        tracker_.setPricing("gpt-4", 0.03, 0.06);          // 第一个 = baseline
        tracker_.setPricing("gpt-3.5-turbo", 0.001, 0.002);

        CostRecord r_a;
        r_a.request_id = "rA"; r_a.tenant_id = "tenant-A";
        r_a.model = "gpt-3.5-turbo";
        r_a.input_tokens = 1000; r_a.output_tokens = 500;
        r_a.total_cost = 0.002;  // gpt-3.5
        tracker_.record(r_a);  // baseline_cost auto = 0.06 (gpt-4)

        CostRecord r_b;
        r_b.request_id = "rB"; r_b.tenant_id = "tenant-B";
        r_b.model = "gpt-3.5-turbo";
        r_b.input_tokens = 2000; r_b.output_tokens = 1000;
        r_b.total_cost = 0.004;
        tracker_.record(r_b);  // baseline_cost auto = 0.12

        // 喂数据：QualityMonitor 3 reason 计数。
        monitor_.recordQuality("gpt-3.5-turbo", 0.85, "factuality");
        monitor_.recordQuality("gpt-3.5-turbo", 0.80, "factuality");
        monitor_.recordQuality("gpt-3.5-turbo", 0.78, "refusal");
        monitor_.recordQuality("gpt-3.5-turbo", 0.70, "latency_degraded");

        // 喂数据：SemanticCache 1 exact + 1 conversation hit。
        embedder_ = std::make_unique<HashEmbedder>(32);
        vstore_ = std::make_unique<HnswVectorStore>(32, 50000);
        vstore_->initialize();
        cache_ = std::make_unique<SemanticCache>(*embedder_, *vstore_, 0.9f, 3600s, 100);
        cache_->put("hi", "hello");
        cache_->get("hi");                          // exact
        cache_->put("hey", "yo", "", "conv:tA:u");
        // conversation hit: conversation_scoped now explicit (TASK-20260609-01
        // D1=C decoupled the conversation signal from a non-empty partition_key).
        cache_->get("hey", "", "conv:tA:u", true);  // conversation

        ctrl_ = std::make_unique<AdminController>(
            &store_, auth_svc_.get(), audit_.get());
        ctrl_->setCostTracker(&tracker_);
        ctrl_->setQualityMonitor(&monitor_);
        ctrl_->setSemanticCache(cache_.get());

        super_ctx_.role = Role::SuperAdmin;
        super_ctx_.tenant_id = "t-super";
        super_ctx_.is_rbac_enabled = true;

        admin_a_ctx_.role = Role::TenantAdmin;
        admin_a_ctx_.tenant_id = "tenant-A";
        admin_a_ctx_.is_rbac_enabled = true;

        admin_b_ctx_.role = Role::TenantAdmin;
        admin_b_ctx_.tenant_id = "tenant-B";
        admin_b_ctx_.is_rbac_enabled = true;

    }

    void TearDown() override {
        if (audit_) audit_->shutdown();
    }

    MemoryPersistentStore store_;
    std::unique_ptr<FeatureGate> gate_;
    std::unique_ptr<AuthService> auth_svc_;
    std::unique_ptr<AuditLogger> audit_;
    CostTracker tracker_;
    QualityMonitor monitor_;
    std::unique_ptr<HashEmbedder> embedder_;
    std::unique_ptr<HnswVectorStore> vstore_;
    std::unique_ptr<SemanticCache> cache_;
    std::unique_ptr<AdminController> ctrl_;

    AuthContext super_ctx_, admin_a_ctx_, admin_b_ctx_;
};

TEST_F(AdminCaseStudyTest, HeadlineReturnsAllFields) {
    auto r = ctrl_->caseStudyHeadline(super_ctx_);
    ASSERT_FALSE(r.is_error) << r.body.dump();
    EXPECT_EQ(r.status, 200);

    EXPECT_TRUE(r.body.contains("saved_vs_baseline"));
    EXPECT_TRUE(r.body.contains("cache_hit_by_type"));
    EXPECT_TRUE(r.body.contains("quality_reason"));
    EXPECT_TRUE(r.body.contains("scope"));

    // saved_vs_baseline 内层结构
    auto svb = r.body["saved_vs_baseline"];
    EXPECT_TRUE(svb.contains("cost_saved"));
    EXPECT_TRUE(svb.contains("baseline_cost"));
    EXPECT_TRUE(svb.contains("actual_cost"));
    EXPECT_TRUE(svb.contains("savings_percent"));

    // cache_hit_by_type 3 类
    auto ch = r.body["cache_hit_by_type"];
    EXPECT_TRUE(ch.contains("hit_exact"));
    EXPECT_TRUE(ch.contains("hit_semantic"));
    EXPECT_TRUE(ch.contains("hit_conversation"));
    EXPECT_TRUE(ch.contains("total_hit_rate"));
    EXPECT_GE(ch["hit_exact"].get<uint64_t>(), 1u);
    EXPECT_GE(ch["hit_conversation"].get<uint64_t>(), 1u);

    // quality_reason 3 档
    auto q = r.body["quality_reason"];
    EXPECT_TRUE(q.contains("reason_factuality"));
    EXPECT_TRUE(q.contains("reason_refusal"));
    EXPECT_TRUE(q.contains("reason_latency_degraded"));
    EXPECT_GE(q["reason_factuality"].get<size_t>(), 2u);
}

TEST_F(AdminCaseStudyTest, RbacFiltersTenant) {
    // SR1: Admin 仅看本租户。
    auto rA = ctrl_->caseStudyHeadline(admin_a_ctx_);
    ASSERT_FALSE(rA.is_error) << rA.body.dump();
    EXPECT_EQ(rA.body["scope"], "tenant");
    EXPECT_EQ(rA.body["tenant_id"], "tenant-A");
    // tenant-A baseline = gpt-4 reprice (1k+0.5k tokens) = 0.06
    EXPECT_DOUBLE_EQ(rA.body["saved_vs_baseline"]["baseline_cost"].get<double>(),
                     0.06);
    EXPECT_DOUBLE_EQ(rA.body["saved_vs_baseline"]["actual_cost"].get<double>(),
                     0.002);

    auto rB = ctrl_->caseStudyHeadline(admin_b_ctx_);
    ASSERT_FALSE(rB.is_error);
    EXPECT_EQ(rB.body["tenant_id"], "tenant-B");
    // tenant-B baseline = gpt-4 reprice (2k+1k tokens) = 0.12
    EXPECT_DOUBLE_EQ(rB.body["saved_vs_baseline"]["baseline_cost"].get<double>(),
                     0.12);

    // SuperAdmin 看全局聚合（A + B）。
    auto rSuper = ctrl_->caseStudyHeadline(super_ctx_);
    ASSERT_FALSE(rSuper.is_error);
    EXPECT_EQ(rSuper.body["scope"], "global");
    EXPECT_DOUBLE_EQ(rSuper.body["saved_vs_baseline"]["baseline_cost"].get<double>(),
                     0.18);
    EXPECT_DOUBLE_EQ(rSuper.body["saved_vs_baseline"]["actual_cost"].get<double>(),
                     0.006);
}

// TASK-20260528-01 Epic 1 — aggregator_since 字段非回归
//
// caseStudyHeadline 必须返回 aggregator_since 字段（与 dashboardSummary /
// getSavingsSummary 对齐 / spec §4.3 D8=B）。
// - SavingsAggregator 未注入时：字段存在 + 值为 null（前端兜底 "自启动以来"）
// - 注入时：字段存在 + 值为 ISO 8601 字符串
TEST_F(AdminCaseStudyTest, IncludesAggregatorSinceField) {
    // Path A: 未注入 SavingsAggregator → aggregator_since == null
    auto r = ctrl_->caseStudyHeadline(super_ctx_);
    ASSERT_FALSE(r.is_error) << r.body.dump();
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(r.body.contains("aggregator_since"));
    EXPECT_TRUE(r.body["aggregator_since"].is_null());
}

TEST_F(AdminCaseStudyTest, EmptyDataReturnsZeroSkeleton) {
    // 当 cost_tracker / quality_monitor / semantic_cache 都未注入时，
    // endpoint 必须返回 200 + 0 占位 schema（不能 500）。这保证 admin
    // dashboard 在新部署 / 测试 stub 场景下仍可工作。
    AdminController bare_ctrl(&store_, auth_svc_.get(), audit_.get());
    auto r = bare_ctrl.caseStudyHeadline(super_ctx_);
    ASSERT_FALSE(r.is_error) << r.body.dump();
    EXPECT_EQ(r.status, 200);

    EXPECT_DOUBLE_EQ(r.body["saved_vs_baseline"]["cost_saved"].get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(r.body["saved_vs_baseline"]["baseline_cost"].get<double>(), 0.0);
    EXPECT_EQ(r.body["cache_hit_by_type"]["hit_exact"].get<uint64_t>(), 0u);
    EXPECT_EQ(r.body["quality_reason"]["reason_factuality"].get<size_t>(), 0u);
}
