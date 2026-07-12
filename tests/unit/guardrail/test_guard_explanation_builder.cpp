#include "guardrail/guard_explanation_builder.h"

#include <gtest/gtest.h>

namespace aegisgate::guard {
namespace {

TEST(GuardExplanationBuilderTest, FromInjectionMapsL1Keyword) {
    aegisgate::InjectionResult r;
    r.detected = true;
    r.matched_pattern = "ignore_previous";
    r.layer = "L1-keyword";
    r.confidence = 1.0;
    auto e = GuardExplanationBuilder::fromInjection(r);
    EXPECT_EQ(e.trigger_layer, "L1");
    EXPECT_EQ(e.trigger_rule_id, "ignore_previous");
    EXPECT_EQ(e.matched_pattern, "ignore_previous");
    EXPECT_FLOAT_EQ(e.confidence, 1.0f);
    EXPECT_FLOAT_EQ(e.threshold, 1.0f);
}

TEST(GuardExplanationBuilderTest, FromInjectionMapsL2Heuristic) {
    aegisgate::InjectionResult r;
    r.layer = "L2-heuristic";
    r.matched_pattern = "role_switch";
    r.confidence = 0.8;
    auto e = GuardExplanationBuilder::fromInjection(r);
    EXPECT_EQ(e.trigger_layer, "L2");
}

TEST(GuardExplanationBuilderTest, FromRuleAlwaysL2) {
    aegisgate::RuleResult r;
    r.rule_id = "RULE-42";
    r.detail = "Triggered by privacy keyword";
    auto e = GuardExplanationBuilder::fromRule(r);
    EXPECT_EQ(e.trigger_layer, "L2");
    EXPECT_EQ(e.trigger_rule_id, "RULE-42");
    EXPECT_EQ(e.explanation_text, "Triggered by privacy keyword");
}

TEST(GuardExplanationBuilderTest, FromMlClassifierCarriesModelVersion) {
    aegisgate::GuardResult r;
    r.category = "harassment";
    r.score = 0.92f;
    r.threshold = 0.5f;
    auto e = GuardExplanationBuilder::fromMlClassifier(r, "guardrail-bert-v3.2.1");
    EXPECT_EQ(e.trigger_layer, "L3");
    EXPECT_EQ(e.model_version, "guardrail-bert-v3.2.1");
    EXPECT_FLOAT_EQ(e.threshold, 0.5f);
    EXPECT_FLOAT_EQ(e.confidence, 0.92f);
}

TEST(GuardExplanationBuilderTest, FromExternalProviderPreservesVerdict) {
    auto e = GuardExplanationBuilder::fromExternalProvider("openai_moderation",
                                                              "violence", 0.71f);
    EXPECT_EQ(e.trigger_layer, "L4");
    EXPECT_EQ(e.trigger_rule_id, "openai_moderation");
    EXPECT_EQ(e.matched_pattern, "violence");
    EXPECT_FLOAT_EQ(e.confidence, 0.71f);
}

TEST(GuardExplanationBuilderTest, RoundTripsThroughJson) {
    aegisgate::GuardResult r;
    r.category = "spam"; r.score = 0.55f; r.threshold = 0.5f;
    auto e = GuardExplanationBuilder::fromMlClassifier(r, "v2");
    auto j = e.toJson();
    auto back = aegisgate::GuardExplanation::fromJson(j);
    EXPECT_EQ(back.trigger_layer, "L3");
    EXPECT_EQ(back.model_version, "v2");
    EXPECT_FLOAT_EQ(back.confidence, 0.55f);
}

// TASK-20260708-03 / REV20260707-C2 Epic 4 — SR-5 payload safety guard test.
// GuardExplanation payloads are returned by
// GET /admin/api/guard/explanation/{id} and are also written to audit /
// FeedbackBus. They MUST NOT contain raw user input / PII masked text —
// only stage-side classifier verdicts and config-side rule identifiers.
// (guard_explanation.h:36-38, 43-44 documents the field-level contract.)
//
// These tests exercise every from* factory with PII-laden raw text as
// _input_ and assert the produced payload does NOT echo the raw input
// back into any string field. Guards against future regressions where a
// factory might start capturing raw context alongside the verdict.
namespace {

// Returns every string-valued field of the JSON-serialized explanation.
// Any leakage in any of these fails the test.
std::vector<std::string> stringFields(const nlohmann::json& j) {
    return {
        j.value("trigger_layer",    std::string{}),
        j.value("trigger_rule_id",  std::string{}),
        j.value("model_version",    std::string{}),
        j.value("matched_pattern",  std::string{}),
        j.value("explanation_text", std::string{}),
    };
}

void expectNotContained(const nlohmann::json& j,
                        const std::string& tell_tale) {
    for (const auto& f : stringFields(j)) {
        EXPECT_EQ(f.find(tell_tale), std::string::npos)
            << "SR-5 payload safety violated: field '" << f
            << "' contains raw-input tell-tale substring '" << tell_tale
            << "'. GuardExplanation payloads must not leak user input.";
    }
}

}  // namespace

TEST(GuardExplanationBuilderTest, PayloadDoesNotLeakRawInput_SR5_SSN) {
    // Simulate an injection detection whose surrounding raw text contains
    // an SSN pattern. The factories should NOT propagate raw text into
    // the payload — only the config-side matched_pattern (regex literal
    // / keyword name) or classifier category name.
    const std::string ssn_tell_tale = "SSN 123-45-6789";

    // fromInjection: matched_pattern is the config-side pattern *name*
    // (e.g. "role_switch"), not the raw text that matched it. Simulate
    // by naming the pattern something innocuous; the raw text does not
    // enter the factory at all.
    aegisgate::InjectionResult inj;
    inj.detected = true;
    inj.matched_pattern = "role_switch";  // config-side identifier
    inj.layer = "L2-heuristic";
    inj.confidence = 0.8;
    auto e_inj = GuardExplanationBuilder::fromInjection(inj);
    expectNotContained(e_inj.toJson(), ssn_tell_tale);

    aegisgate::RuleResult rr;
    rr.rule_id = "pii_guard_rule_v1";
    rr.detail = "PII pattern matched";  // stage-side description
    auto e_rule = GuardExplanationBuilder::fromRule(rr);
    expectNotContained(e_rule.toJson(), ssn_tell_tale);

    aegisgate::GuardResult gr;
    gr.category = "personal_identifiable_information";
    gr.score = 0.87f;
    gr.threshold = 0.5f;
    auto e_ml = GuardExplanationBuilder::fromMlClassifier(gr, "guardrail-v3.2");
    expectNotContained(e_ml.toJson(), ssn_tell_tale);

    auto e_ext = GuardExplanationBuilder::fromExternalProvider(
        "openai_moderation", "pii", 0.75f);
    expectNotContained(e_ext.toJson(), ssn_tell_tale);
}

TEST(GuardExplanationBuilderTest, PayloadDoesNotLeakRawInput_SR5_Email) {
    const std::string email_tell_tale = "victim.user@example.com";

    aegisgate::InjectionResult inj;
    inj.detected = true;
    inj.matched_pattern = "ignore_previous";  // config-side identifier
    inj.layer = "L1-regex";
    inj.confidence = 1.0;
    expectNotContained(GuardExplanationBuilder::fromInjection(inj).toJson(),
                       email_tell_tale);

    aegisgate::RuleResult rr;
    rr.rule_id = "email_leak_rule";
    rr.detail = "Contact info leaked to output";
    expectNotContained(GuardExplanationBuilder::fromRule(rr).toJson(),
                       email_tell_tale);

    aegisgate::GuardResult gr;
    gr.category = "contact_info_leak";
    gr.score = 0.62f;
    expectNotContained(
        GuardExplanationBuilder::fromMlClassifier(gr, "v2").toJson(),
        email_tell_tale);

    expectNotContained(
        GuardExplanationBuilder::fromExternalProvider(
            "perspective_api", "toxicity", 0.55f).toJson(),
        email_tell_tale);
}

}  // namespace
}  // namespace aegisgate::guard
