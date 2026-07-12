#include <gtest/gtest.h>
#include "observe/cost_optimizer.h"

using namespace aegisgate;

class CostOptimizerTest : public ::testing::Test {
protected:
    void SetUp() override {
        CostOptimizerConfig cfg;
        cfg.enabled = true;
        cfg.min_quality_threshold = 0.5;
        cfg.max_quality_loss = 0.1;
        cfg.min_requests_for_recommendation = 50;
        optimizer_.setConfig(cfg);
        optimizer_.clear();
    }
    CostOptimizer optimizer_;
};

TEST_F(CostOptimizerTest, RecordAndProfile) {
    for (int i = 0; i < 10; i++) {
        optimizer_.recordUsage("gpt-4", 0.06, 0.9);
    }

    auto profiles = optimizer_.getProfiles();
    ASSERT_EQ(profiles.size(), 1u);
    EXPECT_EQ(profiles[0].model, "gpt-4");
    EXPECT_EQ(profiles[0].request_count, 10);
    EXPECT_NEAR(profiles[0].total_cost, 0.6, 1e-9);
    EXPECT_NEAR(profiles[0].avg_quality, 0.9, 1e-9);
}

TEST_F(CostOptimizerTest, GetRecommendation) {
    for (int i = 0; i < 60; i++) {
        optimizer_.recordUsage("gpt-4", 0.06, 0.9);
        optimizer_.recordUsage("gpt-3.5", 0.002, 0.85);
    }

    auto recs = optimizer_.getRecommendations();
    ASSERT_FALSE(recs.empty());

    bool found = false;
    for (const auto& r : recs) {
        if (r.current_model == "gpt-4" && r.recommended_model == "gpt-3.5") {
            found = true;
            EXPECT_GT(r.potential_savings, 0.0);
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(CostOptimizerTest, NoRecommendation) {
    for (int i = 0; i < 60; i++) {
        optimizer_.recordUsage("only-model", 0.01, 0.9);
    }

    auto recs = optimizer_.getRecommendations();
    EXPECT_TRUE(recs.empty());
}

TEST_F(CostOptimizerTest, MinRequestsRequired) {
    for (int i = 0; i < 10; i++) {
        optimizer_.recordUsage("expensive", 1.0, 0.9);
        optimizer_.recordUsage("cheap", 0.001, 0.85);
    }

    auto recs = optimizer_.getRecommendations();
    EXPECT_TRUE(recs.empty());
}

TEST_F(CostOptimizerTest, QualityThreshold) {
    for (int i = 0; i < 60; i++) {
        optimizer_.recordUsage("high-cost", 0.1, 0.8);
        optimizer_.recordUsage("low-quality", 0.001, 0.3);
    }

    auto recs = optimizer_.getRecommendations();
    bool bad_rec = false;
    for (const auto& r : recs) {
        if (r.recommended_model == "low-quality") {
            bad_rec = true;
        }
    }
    EXPECT_FALSE(bad_rec);
}

TEST_F(CostOptimizerTest, ClearResetsState) {
    for (int i = 0; i < 10; i++) {
        optimizer_.recordUsage("model-a", 0.05, 0.8);
    }
    EXPECT_EQ(optimizer_.getProfiles().size(), 1u);

    optimizer_.clear();
    EXPECT_TRUE(optimizer_.getProfiles().empty());
}

TEST_F(CostOptimizerTest, ConfigUpdate) {
    CostOptimizerConfig new_cfg;
    new_cfg.enabled = false;
    new_cfg.min_quality_threshold = 0.7;
    new_cfg.max_quality_loss = 0.05;
    new_cfg.min_requests_for_recommendation = 100;
    optimizer_.setConfig(new_cfg);

    EXPECT_FALSE(optimizer_.costOptimizerConfig().enabled);
    EXPECT_EQ(optimizer_.costOptimizerConfig().min_requests_for_recommendation, 100);
    EXPECT_NEAR(optimizer_.costOptimizerConfig().min_quality_threshold, 0.7, 1e-9);
}

TEST_F(CostOptimizerTest, MaxQualityLossRespected) {
    for (int i = 0; i < 60; i++) {
        optimizer_.recordUsage("premium", 0.1, 0.9);
        optimizer_.recordUsage("budget", 0.001, 0.7);
    }

    auto recs = optimizer_.getRecommendations();
    bool bad_rec = false;
    for (const auto& r : recs) {
        if (r.current_model == "premium" && r.recommended_model == "budget") {
            bad_rec = true;
        }
    }
    EXPECT_FALSE(bad_rec);
}
