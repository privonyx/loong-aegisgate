// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 4.
//
// CapacityPredictor tests (8).

#include "observe/recovery/capacity_predictor.h"

#include <gtest/gtest.h>

#include <vector>

using namespace aegisgate;

namespace {

std::vector<QpsSample> makeLinearTrend(double start_qps, double per_minute_delta,
                                         int n_points,
                                         std::int64_t base_ts_ms = 0) {
    std::vector<QpsSample> v;
    v.reserve(n_points);
    for (int i = 0; i < n_points; ++i) {
        QpsSample s;
        s.ts_ms = base_ts_ms + i * 60 * 1000;
        s.qps = start_qps + per_minute_delta * i;
        v.push_back(s);
    }
    return v;
}

} // namespace

// --- 1: linear extrapolation -----------------------------------------------

TEST(CapacityPredictorTest, PredictQpsLinearExtrapolation) {
    CapacityPredictor cp;
    // 60 → 70 → 80 ... over 6 mins (6 points, +10 qps/min ≈ 0.167 qps/s).
    // 30min after latest sample (t=5m → t=35m) → 60 + 0.167*2100 ≈ 410.
    auto hist = makeLinearTrend(/*start=*/60, /*delta=*/10, /*n=*/6);
    double predicted = cp.predictQps(hist, /*horizon_s=*/30 * 60);
    EXPECT_GE(predicted, 350.0);
    EXPECT_LE(predicted, 500.0);
}

// --- 2: empty history ------------------------------------------------------

TEST(CapacityPredictorTest, PredictQpsEmptyHistoryReturnsZero) {
    CapacityPredictor cp;
    EXPECT_DOUBLE_EQ(cp.predictQps({}, 30 * 60), 0.0);
}

// --- 3: HPA proposal schema -------------------------------------------------

TEST(CapacityPredictorTest, ProposeHpaReturnsValidSchema) {
    CapacityPredictor cp;
    auto hist = makeLinearTrend(50, 5, 10);  // climbs to ~95
    auto p = cp.proposeHpa(hist, /*current_replicas=*/2);
    EXPECT_TRUE(p.contains("generated_at_ms"));
    EXPECT_TRUE(p.contains("current_qps"));
    EXPECT_TRUE(p.contains("predicted_qps_30min"));
    EXPECT_TRUE(p.contains("current_replicas"));
    EXPECT_TRUE(p.contains("proposed_replicas"));
    EXPECT_TRUE(p.contains("safety_margin"));
    EXPECT_TRUE(p.contains("estimated_cost_increase_usd_24h"));
    EXPECT_TRUE(p.contains("suggested_kubectl_command"));
    EXPECT_TRUE(p.contains("rationale"));
    EXPECT_EQ(p.value("current_replicas", -1), 2);
}

// --- 4: SR-NEW3 max replicas cap (M6 mutation target) ----------------------

TEST(CapacityPredictorTest, ProposeHpaRespectsMaxReplicas) {
    CapacityPredictor::Config cfg;
    cfg.max_replicas            = 5;
    cfg.target_qps_per_replica  = 50;
    cfg.safety_margin           = 0.0;
    CapacityPredictor cp(cfg);

    // History climbing to absurdly high QPS.
    auto hist = makeLinearTrend(/*start=*/500, /*delta=*/100, /*n=*/10);
    auto p = cp.proposeHpa(hist, /*current_replicas=*/3);
    EXPECT_LE(p.value("proposed_replicas", -1), 5)
        << "M6 mutation target: max_replicas cap";
}

// --- 5: SR-NEW3 min replicas floor -----------------------------------------

TEST(CapacityPredictorTest, ProposeHpaRespectsMinReplicas) {
    CapacityPredictor::Config cfg;
    cfg.min_replicas            = 2;
    cfg.target_qps_per_replica  = 100;
    CapacityPredictor cp(cfg);

    // History trending downward.
    auto hist = makeLinearTrend(/*start=*/30, /*delta=*/-1, /*n=*/10);
    auto p = cp.proposeHpa(hist, /*current_replicas=*/3);
    EXPECT_GE(p.value("proposed_replicas", -1), 2);
}

// --- 6: kubectl command suggestion -----------------------------------------

TEST(CapacityPredictorTest, ProposeHpaIncludesKubectlCommand) {
    CapacityPredictor cp;
    auto hist = makeLinearTrend(50, 10, 10);
    auto p = cp.proposeHpa(hist, /*current_replicas=*/2);
    auto cmd = p.value("suggested_kubectl_command", std::string{});
    EXPECT_NE(cmd.find("kubectl"), std::string::npos);
    EXPECT_NE(cmd.find("scale"), std::string::npos);
}

// --- 7: cost increase estimate ---------------------------------------------

TEST(CapacityPredictorTest, ProposeHpaCostIncreaseEstimateAccurate) {
    CapacityPredictor::Config cfg;
    cfg.target_qps_per_replica       = 100;
    cfg.safety_margin                = 0.0;
    cfg.max_cost_per_replica_usd_24h = 1.0;
    CapacityPredictor cp(cfg);

    auto hist = makeLinearTrend(150, 0, 10);
    auto p = cp.proposeHpa(hist, /*current_replicas=*/1);
    int proposed = p.value("proposed_replicas", -1);
    int current  = p.value("current_replicas", -1);
    EXPECT_GE(proposed, current);
    double extra = p.value("estimated_cost_increase_usd_24h", -1.0);
    EXPECT_DOUBLE_EQ(extra, (proposed - current) * 1.0);
}

// --- 8: scale-up triggered when saturated -----------------------------------

TEST(CapacityPredictorTest, ProposeHpaWhenSaturatedExceedsCurrentReplicas) {
    CapacityPredictor::Config cfg;
    cfg.target_qps_per_replica = 100;
    cfg.safety_margin          = 0.2;
    CapacityPredictor cp(cfg);

    // Predicted ≥ 250 QPS; with 1 replica @ 100 cap and 20% margin must
    // propose ≥ 3 replicas: ceil(250 / (100 / 1.2)) = ceil(3.0) = 3.
    auto hist = makeLinearTrend(/*start=*/200, /*delta=*/5, /*n=*/10);
    auto p = cp.proposeHpa(hist, /*current_replicas=*/1);
    EXPECT_GT(p.value("proposed_replicas", -1), 1);
}
