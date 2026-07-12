#include "guardrail/guard_explanation_builder.h"

namespace aegisgate::guard {

namespace {

std::string canonicalLayer(const std::string& legacy_layer) {
    // InjectionDetector emits "L1-keyword" / "L1-regex" / "L2-heuristic".
    // GuardExplanation surfaces canonical "L1" / "L2" / "L3" / "L4" / "L5".
    if (legacy_layer.rfind("L1", 0) == 0) return "L1";
    if (legacy_layer.rfind("L2", 0) == 0) return "L2";
    if (legacy_layer.rfind("L3", 0) == 0) return "L3";
    if (legacy_layer.rfind("L4", 0) == 0) return "L4";
    return legacy_layer.empty() ? "L1" : legacy_layer;
}

}  // namespace

GuardExplanation GuardExplanationBuilder::fromInjection(const InjectionResult& r) {
    GuardExplanation e;
    e.trigger_layer = canonicalLayer(r.layer);
    e.trigger_rule_id = r.matched_pattern;
    e.matched_pattern = r.matched_pattern;
    e.confidence = static_cast<float>(r.confidence);
    e.threshold = 1.0f;  // hard rule
    e.explanation_text = "Injection signal matched at " + e.trigger_layer;
    return e;
}

GuardExplanation GuardExplanationBuilder::fromRule(const RuleResult& r) {
    GuardExplanation e;
    e.trigger_layer = "L2";
    e.trigger_rule_id = r.rule_id;
    e.matched_pattern = r.detail;
    e.confidence = 1.0f;
    e.threshold = 1.0f;
    e.explanation_text =
        r.detail.empty() ? "RuleEngine matched rule " + r.rule_id : r.detail;
    return e;
}

GuardExplanation GuardExplanationBuilder::fromMlClassifier(
    const GuardResult& r, const std::string& model_version) {
    GuardExplanation e;
    e.trigger_layer = "L3";
    e.trigger_rule_id = r.category;
    e.model_version = model_version;
    e.threshold = r.threshold;
    e.confidence = r.score;
    e.matched_pattern = r.category;
    e.explanation_text = "ML classifier verdict=" + r.category;
    return e;
}

GuardExplanation GuardExplanationBuilder::fromExternalProvider(
    const std::string& provider, const std::string& verdict, float confidence) {
    GuardExplanation e;
    e.trigger_layer = "L4";
    e.trigger_rule_id = provider;
    e.matched_pattern = verdict;
    e.confidence = confidence;
    e.threshold = 0.5f;
    e.explanation_text = "External provider '" + provider + "' verdict=" + verdict;
    return e;
}

}  // namespace aegisgate::guard
