#include "observe/cost_tracker.h"
#include <gtest/gtest.h>

using namespace aegisgate;

namespace {

CostRecord rec(std::string tenant, std::string modality, std::string model,
               double cost, int in_tok = 0, int out_tok = 0) {
    CostRecord r;
    r.tenant_id = std::move(tenant);
    r.modality = std::move(modality);
    r.model = std::move(model);
    r.total_cost = cost;
    r.input_tokens = in_tok;
    r.output_tokens = out_tok;
    return r;
}

} // namespace

TEST(CostTrackerModalityTest, DefaultModalityIsChat) {
    CostRecord r;
    EXPECT_EQ(r.modality, "chat");
}

TEST(CostTrackerModalityTest, RecordsCarryModalityLabel) {
    CostTracker t;
    t.record(rec("t1", "embedding", "text-embedding-3-small", 0.10));
    t.record(rec("t1", "image_gen", "dall-e-3",                0.04));
    t.record(rec("t1", "chat",      "gpt-4o-mini",             0.001));

    EXPECT_EQ(t.summaryByModality("embedding").total_cost, 0.10);
    EXPECT_EQ(t.summaryByModality("image_gen").total_cost, 0.04);
    EXPECT_EQ(t.summaryByModality("chat").total_cost,      0.001);
}

TEST(CostTrackerModalityTest, SummaryByModalityAggregatesAcrossTenants) {
    CostTracker t;
    t.record(rec("t1", "embedding", "m1", 1.0));
    t.record(rec("t2", "embedding", "m1", 2.0));
    auto s = t.summaryByModality("embedding");
    EXPECT_DOUBLE_EQ(s.total_cost, 3.0);
    EXPECT_EQ(s.request_count, 2);
}

TEST(CostTrackerModalityTest, SummariesByModalityReturnsAllSeenLabels) {
    CostTracker t;
    t.record(rec("t1", "embedding", "m1", 0.5));
    t.record(rec("t1", "moderation","m1", 0.0));
    t.record(rec("t1", "chat",      "m1", 0.1));
    auto m = t.summariesByModality();
    EXPECT_EQ(m.size(), 3u);
    EXPECT_DOUBLE_EQ(m["embedding"].total_cost, 0.5);
    EXPECT_DOUBLE_EQ(m["moderation"].total_cost, 0.0);
    EXPECT_DOUBLE_EQ(m["chat"].total_cost, 0.1);
}

// SR5 RBAC: Admin role calls with tenant_filter so they see only their tenant's data.
TEST(CostTrackerModalityTest, SummariesByModalityFiltersToTenant_SR5) {
    CostTracker t;
    t.record(rec("tenant-a", "embedding", "m", 1.0));
    t.record(rec("tenant-b", "embedding", "m", 2.0));
    t.record(rec("tenant-a", "image_gen", "m", 0.5));

    auto admin_a = t.summariesByModality("tenant-a");
    EXPECT_DOUBLE_EQ(admin_a["embedding"].total_cost, 1.0);
    EXPECT_DOUBLE_EQ(admin_a["image_gen"].total_cost, 0.5);
    // tenant-b's record must NOT leak into admin-a's view
    EXPECT_EQ(admin_a.count("embedding"), 1u);

    auto admin_b = t.summariesByModality("tenant-b");
    EXPECT_DOUBLE_EQ(admin_b["embedding"].total_cost, 2.0);
    EXPECT_EQ(admin_b.count("image_gen"), 0u);
}

// SuperAdmin (empty filter) aggregates across all tenants.
TEST(CostTrackerModalityTest, SummariesByModality_EmptyFilterIsSuperAdminView_SR5) {
    CostTracker t;
    t.record(rec("tenant-a", "embedding", "m", 1.0));
    t.record(rec("tenant-b", "embedding", "m", 2.0));

    auto super_view = t.summariesByModality("");
    EXPECT_DOUBLE_EQ(super_view["embedding"].total_cost, 3.0);
    EXPECT_EQ(super_view["embedding"].request_count, 2);
}

TEST(CostTrackerModalityTest, UnknownModalityReturnsEmptySummary) {
    CostTracker t;
    t.record(rec("t1", "embedding", "m", 1.0));
    auto s = t.summaryByModality("nonexistent");
    EXPECT_DOUBLE_EQ(s.total_cost, 0.0);
    EXPECT_EQ(s.request_count, 0);
}

TEST(CostTrackerModalityTest, ModalityFieldSurvivesRecordRoundTrip) {
    CostTracker t;
    auto r = rec("t1", "audio_transcribe", "whisper-1", 0.02, 0, 0);
    t.record(r);
    ASSERT_EQ(t.records().size(), 1u);
    EXPECT_EQ(t.records()[0].modality, "audio_transcribe");
}
