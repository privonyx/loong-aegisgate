#pragma once

// Phase 11.1 TASK-20260523-01 — GuardExplanation
//
// Public, structured "why was this prompt/response blocked" payload returned
// alongside L1/L2/L3/L4 guardrail decisions and consumed by the audit log,
// admin UI, and feedback bus (FeedbackEventType::GuardFeedback).
//
// Decision D3 (=B "中等"): 7 stable fields are emitted for every block.
// Adding a field is a MINOR API change; renaming or removing one is MAJOR.
//
// Design reference: docs/specs/2026-05-23-phase11.1-adaptive-guard-design.md §3.

#include <nlohmann/json.hpp>
#include <string>

namespace aegisgate {

struct GuardExplanation {
    // Which layer fired (e.g. "L1", "L2", "L3", "L4", "L5").
    std::string trigger_layer;

    // Stable rule / pattern identifier (e.g. "injection_pattern_A",
    // "model_classifier_v3"). Empty when not applicable.
    std::string trigger_rule_id;

    // Model version that produced the verdict, e.g. "guardrail-bert-v3.2.1".
    // Empty for purely rule-based layers (L1/L2).
    std::string model_version;

    // Decision threshold. For ML classifiers this is the cutoff confidence;
    // for hard rules it is 1.0. Default 0.5 matches the global classifier
    // default.
    float threshold = 0.5f;

    // Specific text snippet / pattern that matched. Already PII-masked by
    // the producer (E1.6 / SR2 reuse).
    std::string matched_pattern;

    // Model confidence in the block decision. 1.0 for hard rules.
    float confidence = 0.0f;

    // Human-readable, PII-masked explanation surfaced to admins.
    std::string explanation_text;

    nlohmann::json toJson() const {
        return {
            {"trigger_layer",    trigger_layer},
            {"trigger_rule_id",  trigger_rule_id},
            {"model_version",    model_version},
            {"threshold",        threshold},
            {"matched_pattern",  matched_pattern},
            {"confidence",       confidence},
            {"explanation_text", explanation_text},
        };
    }

    static GuardExplanation fromJson(const nlohmann::json& j) {
        GuardExplanation e;
        if (j.contains("trigger_layer"))    e.trigger_layer    = j.value("trigger_layer", std::string{});
        if (j.contains("trigger_rule_id"))  e.trigger_rule_id  = j.value("trigger_rule_id", std::string{});
        if (j.contains("model_version"))    e.model_version    = j.value("model_version", std::string{});
        if (j.contains("threshold"))        e.threshold        = j.value("threshold", 0.5f);
        if (j.contains("matched_pattern"))  e.matched_pattern  = j.value("matched_pattern", std::string{});
        if (j.contains("confidence"))       e.confidence       = j.value("confidence", 0.0f);
        if (j.contains("explanation_text")) e.explanation_text = j.value("explanation_text", std::string{});
        return e;
    }
};

}  // namespace aegisgate
