// Phase 11.1 TASK-20260523-01 — GuardExplanation struct tests.
//
// Verifies the 7-field schema (D3=B "中等" decision) and default values.

#include "aegisgate/guard_explanation.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using aegisgate::GuardExplanation;

TEST(GuardExplanationTest, DefaultsAreEmpty) {
    GuardExplanation e;
    EXPECT_EQ(e.trigger_layer, "");
    EXPECT_EQ(e.trigger_rule_id, "");
    EXPECT_EQ(e.model_version, "");
    EXPECT_FLOAT_EQ(e.threshold, 0.5f);
    EXPECT_EQ(e.matched_pattern, "");
    EXPECT_FLOAT_EQ(e.confidence, 0.0f);
    EXPECT_EQ(e.explanation_text, "");
}

TEST(GuardExplanationTest, FieldsAssignable) {
    GuardExplanation e;
    e.trigger_layer    = "L3";
    e.trigger_rule_id  = "model_classifier_v3";
    e.model_version    = "guardrail-bert-v3.2.1";
    e.threshold        = 0.75f;
    e.matched_pattern  = "high_toxicity";
    e.confidence       = 0.92f;
    e.explanation_text = "Output flagged by ML classifier (toxicity > threshold).";

    EXPECT_EQ(e.trigger_layer, "L3");
    EXPECT_EQ(e.model_version, "guardrail-bert-v3.2.1");
    EXPECT_FLOAT_EQ(e.threshold, 0.75f);
    EXPECT_FLOAT_EQ(e.confidence, 0.92f);
}

TEST(GuardExplanationTest, JsonSerializationRoundTrip) {
    GuardExplanation e;
    e.trigger_layer    = "L1";
    e.trigger_rule_id  = "injection_pattern_A";
    e.model_version    = "";       // L1 has no model
    e.threshold        = 1.0f;     // hard rule
    e.matched_pattern  = "ignore previous instructions";
    e.confidence       = 1.0f;
    e.explanation_text = "Prompt injection regex hit";

    auto j = e.toJson();
    EXPECT_EQ(j["trigger_layer"], "L1");
    EXPECT_EQ(j["trigger_rule_id"], "injection_pattern_A");
    EXPECT_EQ(j["model_version"], "");
    EXPECT_FLOAT_EQ(j["threshold"].get<float>(), 1.0f);
    EXPECT_EQ(j["matched_pattern"], "ignore previous instructions");
    EXPECT_FLOAT_EQ(j["confidence"].get<float>(), 1.0f);
    EXPECT_EQ(j["explanation_text"], "Prompt injection regex hit");

    auto round = GuardExplanation::fromJson(j);
    EXPECT_EQ(round.trigger_layer, e.trigger_layer);
    EXPECT_EQ(round.trigger_rule_id, e.trigger_rule_id);
    EXPECT_EQ(round.matched_pattern, e.matched_pattern);
    EXPECT_FLOAT_EQ(round.threshold, e.threshold);
    EXPECT_FLOAT_EQ(round.confidence, e.confidence);
}

TEST(GuardExplanationTest, FromJsonWithMissingFieldsUsesDefaults) {
    nlohmann::json j = {
        {"trigger_layer", "L4"},
        // other fields missing
    };
    auto e = GuardExplanation::fromJson(j);
    EXPECT_EQ(e.trigger_layer, "L4");
    EXPECT_EQ(e.trigger_rule_id, "");
    EXPECT_FLOAT_EQ(e.threshold, 0.5f);
    EXPECT_FLOAT_EQ(e.confidence, 0.0f);
}
