#include <gtest/gtest.h>
#include <chrono>
#include "observe/cost_tracker.h"
#include "observe/cost_attribution.h"
#include "observe/anomaly_detector.h"
#include "observe/cost_optimizer.h"
#include "core/context.h"
#include "storage/memory_persistent_store.h"

using namespace aegisgate;

// P0-5: CostTracker::process is the per-request cost收尾; wiring a
// CostAttribution must feed it one entry per request so its per-app/tenant/model
// query API stops being a dead, never-written component.
TEST(CostTrackerWiringTest, FeedsCostAttributionWhenWired) {
    CostAttribution attr;
    CostTracker tracker;
    tracker.setPricing("gpt-4o", 0.01, 0.03);
    tracker.setCostAttribution(&attr);

    RequestContext ctx;
    ctx.request_id = "r1";
    ctx.tenant_id = "t1";
    ctx.app_id = "a1";
    ctx.target_model = "gpt-4o";
    ctx.token_usage.prompt_tokens = 1000;
    ctx.token_usage.completion_tokens = 1000;

    tracker.process(ctx);

    EXPECT_EQ(attr.size(), 1u);
    EXPECT_GT(attr.getCostByModel("gpt-4o"), 0.0);
    EXPECT_GT(attr.getCostByTenant("t1"), 0.0);
}

TEST(CostTrackerWiringTest, NoAttributionIsSafe) {
    CostTracker tracker;  // no attribution wired
    tracker.setPricing("gpt-4o", 0.01, 0.03);
    RequestContext ctx;
    ctx.request_id = "r1";
    ctx.target_model = "gpt-4o";
    ctx.token_usage.prompt_tokens = 10;
    ctx.token_usage.completion_tokens = 10;
    EXPECT_EQ(tracker.process(ctx), StageResult::Continue);
}

TEST(CostTrackerWiringTest, FeedsCostOptimizerWhenWired) {
    CostOptimizer optimizer;
    CostTracker tracker;
    tracker.setPricing("gpt-4o", 0.01, 0.03);
    tracker.setCostOptimizer(&optimizer);

    RequestContext ctx;
    ctx.request_id = "r-optimizer";
    ctx.tenant_id = "t1";
    ctx.app_id = "a1";
    ctx.target_model = "gpt-4o";
    ctx.quality_score = 0.8;
    ctx.token_usage.prompt_tokens = 1000;
    ctx.token_usage.completion_tokens = 1000;

    EXPECT_EQ(tracker.process(ctx), StageResult::Continue);

    auto profiles = optimizer.getProfiles();
    ASSERT_EQ(profiles.size(), 1u);
    EXPECT_EQ(profiles[0].model, "gpt-4o");
    EXPECT_DOUBLE_EQ(profiles[0].total_cost, 0.04);
    EXPECT_DOUBLE_EQ(profiles[0].avg_quality, 0.8);
    EXPECT_EQ(profiles[0].request_count, 1);
}

TEST(CostTrackerWiringTest, NullObserversAreSafe) {
    CostTracker tracker;
    tracker.setPricing("gpt-4o", 0.01, 0.03);

    RequestContext ctx;
    ctx.request_id = "r-null-observers";
    ctx.target_model = "gpt-4o";
    ctx.token_usage.prompt_tokens = 1000;
    ctx.token_usage.completion_tokens = 1000;

    EXPECT_EQ(tracker.process(ctx), StageResult::Continue);
    ASSERT_EQ(tracker.records().size(), 1u);
    EXPECT_DOUBLE_EQ(tracker.records()[0].total_cost, 0.04);
}

TEST(CostTrackerWiringTest, FeedsAnomalyDetectorCostSpikeWhenWired) {
    AnomalyDetectorConfig cfg;
    cfg.enabled = true;
    cfg.z_score_threshold = 0.5;
    cfg.window_size = 4;
    AnomalyDetector detector(cfg);

    CostTracker tracker;
    tracker.setPricing("gpt-4o", 1.0, 1.0);
    tracker.setAnomalyDetector(&detector);

    for (int i = 0; i < 3; ++i) {
        RequestContext baseline;
        baseline.request_id = "baseline-" + std::to_string(i);
        baseline.target_model = "gpt-4o";
        baseline.token_usage.prompt_tokens = 100;
        baseline.token_usage.completion_tokens = 100;
        EXPECT_EQ(tracker.process(baseline), StageResult::Continue);
    }

    RequestContext spike;
    spike.request_id = "spike";
    spike.target_model = "gpt-4o";
    spike.token_usage.prompt_tokens = 5000;
    spike.token_usage.completion_tokens = 5000;
    EXPECT_EQ(tracker.process(spike), StageResult::Continue);

    auto events = detector.recentEvents();
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().type, AnomalyType::CostSpike);
}

class CostTrackerTest : public ::testing::Test {
protected:
    void SetUp() override {
        tracker_.clear();
        tracker_.setPricing("gpt-4", 0.03, 0.06);
        tracker_.setPricing("gpt-3.5-turbo", 0.001, 0.002);
        tracker_.setPricing("claude-3-sonnet", 0.003, 0.015);
    }
    CostTracker tracker_;
};

TEST_F(CostTrackerTest, CalculatesCostCorrectly) {
    auto rec = tracker_.calculate("gpt-4", 1000, 500);
    EXPECT_DOUBLE_EQ(rec.input_cost, 0.03);
    EXPECT_DOUBLE_EQ(rec.output_cost, 0.03);
    EXPECT_DOUBLE_EQ(rec.total_cost, 0.06);
}

TEST_F(CostTrackerTest, CalculatesCheapModel) {
    auto rec = tracker_.calculate("gpt-3.5-turbo", 2000, 1000);
    EXPECT_DOUBLE_EQ(rec.input_cost, 0.002);
    EXPECT_DOUBLE_EQ(rec.output_cost, 0.002);
    EXPECT_DOUBLE_EQ(rec.total_cost, 0.004);
}

TEST_F(CostTrackerTest, UnknownModelZeroCost) {
    auto rec = tracker_.calculate("unknown-model", 1000, 500);
    EXPECT_DOUBLE_EQ(rec.total_cost, 0.0);
}

TEST_F(CostTrackerTest, RecordsAndSummarizes) {
    CostRecord r1;
    r1.request_id = "r1"; r1.tenant_id = "t1"; r1.model = "gpt-4";
    r1.input_tokens = 1000; r1.output_tokens = 500; r1.total_cost = 0.06;

    CostRecord r2;
    r2.request_id = "r2"; r2.tenant_id = "t1"; r2.model = "gpt-3.5-turbo";
    r2.input_tokens = 2000; r2.output_tokens = 1000; r2.total_cost = 0.004;

    CostRecord r3;
    r3.request_id = "r3"; r3.tenant_id = "t2"; r3.model = "gpt-4";
    r3.input_tokens = 500; r3.output_tokens = 200; r3.total_cost = 0.027;

    tracker_.record(r1);
    tracker_.record(r2);
    tracker_.record(r3);

    auto t1_summary = tracker_.summaryByTenant("t1");
    EXPECT_EQ(t1_summary.request_count, 2);
    EXPECT_NEAR(t1_summary.total_cost, 0.064, 0.001);

    auto gpt4_summary = tracker_.summaryByModel("gpt-4");
    EXPECT_EQ(gpt4_summary.request_count, 2);

    auto total = tracker_.totalSummary();
    EXPECT_EQ(total.request_count, 3);
}

TEST_F(CostTrackerTest, ClearRemovesRecords) {
    CostRecord r;
    r.request_id = "r1"; r.total_cost = 0.01;
    tracker_.record(r);
    EXPECT_EQ(tracker_.records().size(), 1u);
    tracker_.clear();
    EXPECT_EQ(tracker_.records().size(), 0u);
    EXPECT_DOUBLE_EQ(tracker_.getTotalCostInWindow(std::chrono::seconds{60}), 0.0);
}

TEST_F(CostTrackerTest, PipelineTracksContext) {
    RequestContext ctx;
    ctx.request_id = "req-cost";
    ctx.tenant_id = "tenant-a";
    ctx.chat_request.model = "gpt-4";
    ctx.token_usage = {1000, 500, 1500};
    ctx.start_time = std::chrono::steady_clock::now();

    EXPECT_EQ(tracker_.process(ctx), StageResult::Continue);
    ASSERT_EQ(tracker_.records().size(), 1u);
    EXPECT_EQ(tracker_.records()[0].request_id, "req-cost");
    EXPECT_NEAR(tracker_.records()[0].total_cost, 0.06, 0.001);
}

TEST_F(CostTrackerTest, SummaryByModelFiltersCorrectly) {
    CostRecord r1, r2;
    r1.model = "gpt-4"; r1.total_cost = 0.1; r1.input_tokens = 100; r1.output_tokens = 50;
    r2.model = "claude-3-sonnet"; r2.total_cost = 0.05; r2.input_tokens = 200; r2.output_tokens = 100;
    tracker_.record(r1);
    tracker_.record(r2);

    auto s = tracker_.summaryByModel("claude-3-sonnet");
    EXPECT_EQ(s.request_count, 1);
    EXPECT_NEAR(s.total_cost, 0.05, 0.001);
}

TEST_F(CostTrackerTest, ZeroTokensCostZero) {
    auto rec = tracker_.calculate("gpt-4", 0, 0);
    EXPECT_DOUBLE_EQ(rec.total_cost, 0.0);
}

TEST_F(CostTrackerTest, PersistentStoreReceivesRecords) {
    MemoryPersistentStore store;
    store.initialize();
    tracker_.setPersistentStore(&store);

    CostRecord r;
    r.request_id = "r-persist";
    r.model = "gpt-4";
    r.total_cost = 0.06;
    tracker_.record(r);

    EXPECT_EQ(store.costRecordCount(), 1);
    auto results = store.queryCosts();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].request_id, "r-persist");
}

TEST_F(CostTrackerTest, NullStoreDoesNotCrash) {
    tracker_.setPersistentStore(nullptr);
    CostRecord r;
    r.request_id = "r-null";
    tracker_.record(r);
    EXPECT_EQ(tracker_.records().size(), 1u);
}

TEST_F(CostTrackerTest, CostInWindowReturnsRecent) {
    tracker_.setPricing("gpt-4o", 0.03, 0.06);

    RequestContext ctx;
    ctx.request_id = "req-window";
    ctx.target_model = "gpt-4o";
    ctx.token_usage = {100, 50, 150};
    ctx.start_time = std::chrono::steady_clock::now();

    tracker_.process(ctx);

    auto cost = tracker_.getTotalCostInWindow(std::chrono::seconds{60});
    EXPECT_GT(cost, 0.0);
    EXPECT_GT(tracker_.getCostInWindow("gpt-4o", std::chrono::seconds{60}), 0.0);
}

TEST_F(CostTrackerTest, PipelineWithPersistentStore) {
    MemoryPersistentStore store;
    store.initialize();
    tracker_.setPersistentStore(&store);

    RequestContext ctx;
    ctx.request_id = "req-pipe-cost";
    ctx.tenant_id = "tenant-a";
    ctx.chat_request.model = "gpt-4";
    ctx.token_usage = {1000, 500, 1500};
    ctx.start_time = std::chrono::steady_clock::now();

    tracker_.process(ctx);
    EXPECT_EQ(store.costRecordCount(), 1);
    EXPECT_EQ(tracker_.records().size(), 1u);
}

// Phase 11.5 E3.0 — getTenantCostInWindow coverage (3 mandatory cases).

TEST_F(CostTrackerTest, TenantCostInWindowIsolatesTenants) {
    RequestContext ctxA;
    ctxA.request_id  = "req-tA";
    ctxA.tenant_id   = "tenant-A";
    ctxA.target_model = "gpt-4";
    ctxA.token_usage = {1000, 500, 1500};
    ctxA.start_time  = std::chrono::steady_clock::now();
    tracker_.process(ctxA);

    RequestContext ctxB;
    ctxB.request_id  = "req-tB";
    ctxB.tenant_id   = "tenant-B";
    ctxB.target_model = "gpt-3.5-turbo";
    ctxB.token_usage = {1000, 1000, 2000};
    ctxB.start_time  = std::chrono::steady_clock::now();
    tracker_.process(ctxB);

    auto costA = tracker_.getTenantCostInWindow("tenant-A", std::chrono::hours{1});
    auto costB = tracker_.getTenantCostInWindow("tenant-B", std::chrono::hours{1});
    EXPECT_GT(costA, 0.0);
    EXPECT_GT(costB, 0.0);
    EXPECT_NE(costA, costB)
        << "different models / tokens must produce distinct costs";
    EXPECT_DOUBLE_EQ(
        tracker_.getTenantCostInWindow("tenant-C", std::chrono::hours{1}),
        0.0);
}

TEST_F(CostTrackerTest, TenantCostInWindowFiltersByWindow) {
    RequestContext ctx;
    ctx.request_id  = "req-win";
    ctx.tenant_id   = "tenant-W";
    ctx.target_model = "gpt-4";
    ctx.token_usage = {1000, 500, 1500};
    ctx.start_time  = std::chrono::steady_clock::now();
    tracker_.process(ctx);

    EXPECT_GT(tracker_.getTenantCostInWindow("tenant-W",
                                              std::chrono::hours{1}), 0.0);
    // Zero-second window must exclude the just-recorded entry (cutoff = now).
    EXPECT_DOUBLE_EQ(
        tracker_.getTenantCostInWindow("tenant-W", std::chrono::seconds{0}),
        0.0);
}

TEST_F(CostTrackerTest, TenantCostInWindowEmptyTenantReturnsZero) {
    RequestContext ctx;
    ctx.request_id  = "req-empty";
    ctx.tenant_id   = "tenant-X";
    ctx.target_model = "gpt-4";
    ctx.token_usage = {500, 250, 750};
    ctx.start_time  = std::chrono::steady_clock::now();
    tracker_.process(ctx);

    EXPECT_DOUBLE_EQ(
        tracker_.getTenantCostInWindow("", std::chrono::hours{1}), 0.0);
}

// === TASK-20260527-02 — Case Study Numbers signals ===
// baseline_cost 与 routing_decision_reason 字段是 MVP-5 case-study 数据骨架的
// 后端基础（spec §3.2.1）。SR2: baseline_cost 必须由 CostTracker 内部根据
// pricing 表计算，不接受外部输入；外部传入的 baseline_cost 会被 record() 覆盖。

TEST_F(CostTrackerTest, RecordsBaselineCostFromPricing) {
    // baseline_model_ 在 SetUp() 第一次 setPricing("gpt-4", 0.03, 0.06) 时
    // 自动记录为 "gpt-4"。所以即便 calculate() 用 cheaper model，baseline_cost
    // 也按 gpt-4 价格算（即"如果用 baseline 会花多少"的反事实成本）。
    auto rec = tracker_.calculate("gpt-3.5-turbo", 1000, 500);
    // actual cost: gpt-3.5-turbo @ (0.001 * 1 + 0.002 * 0.5) = 0.002
    EXPECT_DOUBLE_EQ(rec.total_cost, 0.002);
    // baseline cost: gpt-4 @ (0.03 * 1 + 0.06 * 0.5) = 0.06
    EXPECT_DOUBLE_EQ(rec.baseline_cost, 0.06);
}

TEST_F(CostTrackerTest, RoutingDecisionReasonPropagation) {
    // 用户/上游 stage 标注路由决策原因；CostTracker 透传到 record。
    CostRecord r;
    r.request_id = "r-route";
    r.tenant_id = "t-r";
    r.model = "gpt-3.5-turbo";
    r.input_tokens = 1000;
    r.output_tokens = 500;
    r.total_cost = 0.002;
    r.routing_decision_reason = "router_economy";

    tracker_.record(r);
    ASSERT_EQ(tracker_.records().size(), 1u);
    EXPECT_EQ(tracker_.records()[0].routing_decision_reason, "router_economy");
}

TEST_F(CostTrackerTest, RejectsExternalBaselineCost) {
    // SR2 反向锚点：恶意构造 baseline_cost=9999.0 注入，期望 record() 用
    // pricing 表内部值覆盖（baseline_model_=gpt-4 / 0.03 + 0.06 = 0.09）。
    CostRecord malicious;
    malicious.request_id = "r-evil";
    malicious.tenant_id = "t-evil";
    malicious.model = "gpt-3.5-turbo";
    malicious.input_tokens = 1000;
    malicious.output_tokens = 1000;
    malicious.total_cost = 0.003;
    malicious.baseline_cost = 9999.0;  // 恶意注入

    tracker_.record(malicious);
    ASSERT_EQ(tracker_.records().size(), 1u);
    // baseline_cost 必须是 gpt-4 reprice：0.03 * 1 + 0.06 * 1 = 0.09
    EXPECT_DOUBLE_EQ(tracker_.records()[0].baseline_cost, 0.09);
    EXPECT_NE(tracker_.records()[0].baseline_cost, 9999.0);
}

TEST_F(CostTrackerTest, SavedVsBaselineSummary) {
    // totalSummary() 累加 baseline_cost，让 case-study headline 能拼出
    // total_baseline_cost - total_cost = saved_vs_baseline。
    CostRecord r1;
    r1.request_id = "r1"; r1.tenant_id = "t1"; r1.model = "gpt-3.5-turbo";
    r1.input_tokens = 1000; r1.output_tokens = 500; r1.total_cost = 0.002;
    tracker_.record(r1);

    CostRecord r2;
    r2.request_id = "r2"; r2.tenant_id = "t1"; r2.model = "gpt-3.5-turbo";
    r2.input_tokens = 2000; r2.output_tokens = 1000; r2.total_cost = 0.004;
    tracker_.record(r2);

    auto s = tracker_.totalSummary();
    EXPECT_DOUBLE_EQ(s.total_cost, 0.006);
    // baseline = gpt-4: r1=0.06 + r2=0.12 = 0.18
    EXPECT_DOUBLE_EQ(s.total_baseline_cost, 0.18);
}

// --- P1-5: loadPricing must inject a baseline model (production path) ---
// Root cause: only setPricing() set baseline_model_; the runtime uses
// loadPricing() which left baseline_model_ empty -> baseline_cost == 0 forever.
TEST(CostTrackerBaselineInjection, LoadPricingInjectsBaselineP1_5) {
    CostTracker tracker;
    tracker.loadPricing("config/models.yaml");

    // Baseline must be non-empty after loadPricing (was empty -> P1-5 bug).
    EXPECT_FALSE(tracker.baselineModel().empty());
    // Default heuristic: most expensive model by (input+output) per-1k.
    // In config/models.yaml that is gpt-4o (0.005 + 0.015 = 0.02).
    EXPECT_EQ(tracker.baselineModel(), "gpt-4o");

    auto rec = tracker.calculate("gpt-4o-mini", 1000, 1000);
    rec.request_id = "p1-5";
    tracker.record(rec);
    ASSERT_FALSE(tracker.records().empty());
    // baseline_cost recomputed from gpt-4o pricing -> strictly positive.
    EXPECT_GT(tracker.records()[0].baseline_cost, 0.0);
    // And larger than the cheap model's own cost (saved_vs_baseline > 0).
    EXPECT_GT(tracker.records()[0].baseline_cost,
              tracker.records()[0].total_cost);
}

TEST(CostTrackerBaselineInjection, ExplicitBaselineNotOverriddenP1_5) {
    CostTracker tracker;
    tracker.setBaselineModel("gpt-4o-mini");
    tracker.loadPricing("config/models.yaml");
    // Explicit baseline survives loadPricing.
    EXPECT_EQ(tracker.baselineModel(), "gpt-4o-mini");
}

// P1-7: CostTracker::process must copy ctx.routing_decision_reason into the
// persisted CostRecord (previously the field was declared but never written
// on the hot path -> always "").
TEST(CostTrackerRoutingReason, ProcessCopiesRoutingReasonP1_7) {
    CostTracker tracker;
    tracker.setPricing("gpt-4o-mini", 0.00015, 0.0006);

    RequestContext ctx;
    ctx.request_id = "p1-7";
    ctx.target_model = "gpt-4o-mini";
    ctx.token_usage.prompt_tokens = 100;
    ctx.token_usage.completion_tokens = 50;
    ctx.routing_decision_reason = "router_economy";

    EXPECT_EQ(tracker.process(ctx), StageResult::Continue);
    ASSERT_FALSE(tracker.records().empty());
    EXPECT_EQ(tracker.records().back().routing_decision_reason, "router_economy");
}
