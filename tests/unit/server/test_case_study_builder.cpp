// TASK-20260602-01 Epic 2 — CaseStudySnapshotBuilder unit tests.
//
// 验证从 admin_controller::caseStudyHeadline + admin_ws_controller::buildCaseStudySnapshot
// 抽取的共享 builder：
//   - empty inputs（4 source 全 null）→ 5 字段 schema 完整 + 全 0 占位（不 throw）
//   - include_envelope=true / SuperAdmin → 含 scope=global, 不含 tenant_id
//   - include_envelope=true / Tenant scope → 含 scope=tenant + tenant_id
//   - SR-NEW1：HTTP（include_envelope=true）+ WS（include_envelope=false）路径在
//     同 source 下产生**相同**的 saved_vs_baseline / cache_hit_by_type / quality_reason
//     数据块（保证跨通道数据一致性，仅外层 envelope 区别）
//
// 注：本 builder 测试不依赖 AdminController / AuthContext，只关心纯函数行为。
// caseStudyHeadline endpoint 的 RBAC + 数据正确性已由 test_admin_case_study_endpoint.cpp
// 覆盖；builder 重构后那些测试继续 PASS 即证明业务等价。

#include "server/case_study_builder.h"

#include "cache/embedder.h"
#include "cache/hnsw_vector_store.h"
#include "cache/semantic_cache.h"
#include "observe/cost_tracker.h"
#include "observe/quality_monitor.h"

#include <gtest/gtest.h>
#include <chrono>
#include <memory>

using namespace aegisgate;
using namespace aegisgate::admin;
using namespace std::chrono_literals;

namespace {

// 5 个 hard-coded schema keys（SR4 关键文本 4-way 锁 / spec §2 D9）。
constexpr const char* kSavedVsBaseline   = "saved_vs_baseline";
constexpr const char* kCacheHitByType    = "cache_hit_by_type";
constexpr const char* kQualityReason     = "quality_reason";
constexpr const char* kScope             = "scope";
constexpr const char* kAggregatorSince   = "aggregator_since";

} // namespace

TEST(CaseStudyBuilderTest, EmptyInputsReturnsZeroSkeletonNoEnvelope) {
    CaseStudyInputs in;
    in.include_envelope = false;
    auto j = buildCaseStudySnapshot(in);

    // 3 数据块字段必存在
    EXPECT_TRUE(j.contains(kSavedVsBaseline));
    EXPECT_TRUE(j.contains(kCacheHitByType));
    EXPECT_TRUE(j.contains(kQualityReason));
    // envelope 字段不应存在
    EXPECT_FALSE(j.contains(kScope));
    EXPECT_FALSE(j.contains(kAggregatorSince));
    EXPECT_FALSE(j.contains("tenant_id"));
    EXPECT_FALSE(j.contains("timestamp"));

    // 全 0 占位
    EXPECT_DOUBLE_EQ(j[kSavedVsBaseline]["cost_saved"].get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(j[kSavedVsBaseline]["baseline_cost"].get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(j[kSavedVsBaseline]["actual_cost"].get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(j[kSavedVsBaseline]["savings_percent"].get<double>(), 0.0);
    EXPECT_EQ(j[kCacheHitByType]["hit_exact"].get<uint64_t>(), 0u);
    EXPECT_EQ(j[kCacheHitByType]["hit_semantic"].get<uint64_t>(), 0u);
    EXPECT_EQ(j[kCacheHitByType]["hit_conversation"].get<uint64_t>(), 0u);
    EXPECT_EQ(j[kCacheHitByType]["miss"].get<uint64_t>(), 0u);
    EXPECT_DOUBLE_EQ(j[kCacheHitByType]["total_hit_rate"].get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(j[kQualityReason]["current_ema"].get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(j[kQualityReason]["slope"].get<double>(), 0.0);
    EXPECT_EQ(j[kQualityReason]["reason_factuality"].get<size_t>(), 0u);
    EXPECT_EQ(j[kQualityReason]["reason_refusal"].get<size_t>(), 0u);
    EXPECT_EQ(j[kQualityReason]["reason_latency_degraded"].get<size_t>(), 0u);
}

TEST(CaseStudyBuilderTest, EnvelopeSuperAdminGlobalScope) {
    CaseStudyInputs in;
    in.include_envelope = true;
    in.is_super = true;
    auto j = buildCaseStudySnapshot(in);

    EXPECT_EQ(j[kScope].get<std::string>(), "global");
    EXPECT_FALSE(j.contains("tenant_id"));
    EXPECT_TRUE(j.contains("timestamp"));
    EXPECT_TRUE(j.contains(kAggregatorSince));
    EXPECT_TRUE(j[kAggregatorSince].is_null());  // no aggregator wired
}

TEST(CaseStudyBuilderTest, EnvelopeTenantScopeIncludesTenantId) {
    CaseStudyInputs in;
    in.include_envelope = true;
    in.is_super = false;
    in.tenant_id = "tenant-abc";
    auto j = buildCaseStudySnapshot(in);

    EXPECT_EQ(j[kScope].get<std::string>(), "tenant");
    EXPECT_EQ(j["tenant_id"].get<std::string>(), "tenant-abc");
    EXPECT_TRUE(j.contains("timestamp"));
}

// SR-NEW1: HTTP (include_envelope=true) + WS (include_envelope=false) 在同
// source 下 3 数据块结构与值完全一致。保证跨通道（HTTP polling vs WS push）
// 用户看到同一份数据，不会因为 builder 内部分支不同而漂移。
TEST(CaseStudyBuilderTest, HttpAndWsShareDataBlocks_SR_NEW1) {
    // 喂相同数据：CostTracker + SemanticCache + QualityMonitor
    CostTracker tracker;
    tracker.setPricing("gpt-4", 0.03, 0.06);
    tracker.setPricing("gpt-3.5-turbo", 0.001, 0.002);
    CostRecord r;
    r.request_id = "r1"; r.tenant_id = "t1";
    r.model = "gpt-3.5-turbo";
    r.input_tokens = 1000; r.output_tokens = 500;
    r.total_cost = 0.002;
    tracker.record(r);

    QualityMonitor monitor;
    monitor.recordQuality("gpt-3.5-turbo", 0.85, "factuality");
    monitor.recordQuality("gpt-3.5-turbo", 0.78, "refusal");

    HashEmbedder embedder(32);
    HnswVectorStore vstore(32, 50000);
    vstore.initialize();
    SemanticCache cache(embedder, vstore, 0.9f, 3600s, 100);
    cache.put("hi", "hello");
    cache.get("hi");  // exact hit

    CaseStudyInputs in_http;
    in_http.cost_tracker = &tracker;
    in_http.semantic_cache = &cache;
    in_http.quality_monitor = &monitor;
    in_http.is_super = true;
    in_http.include_envelope = true;

    CaseStudyInputs in_ws = in_http;
    in_ws.include_envelope = false;

    auto j_http = buildCaseStudySnapshot(in_http);
    auto j_ws = buildCaseStudySnapshot(in_ws);

    // SR-NEW1 核心断言：3 数据块字节级一致
    EXPECT_EQ(j_http[kSavedVsBaseline], j_ws[kSavedVsBaseline]);
    EXPECT_EQ(j_http[kCacheHitByType], j_ws[kCacheHitByType]);
    EXPECT_EQ(j_http[kQualityReason], j_ws[kQualityReason]);

    // HTTP 多出 envelope 字段
    EXPECT_TRUE(j_http.contains(kScope));
    EXPECT_FALSE(j_ws.contains(kScope));
}
