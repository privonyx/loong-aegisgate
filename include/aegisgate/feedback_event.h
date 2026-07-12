#pragma once
#include <chrono>
#include <nlohmann/json.hpp>
#include <string>

namespace aegisgate {

// Canonical taxonomy of feedback events carried on the FeedbackBus.
// New variants are accepted in a backwards-compatible way: adding a value
// requires a topic string in the stable topicOf / typeOf mapping. Any
// topic string not known to typeOf() is routed as FeedbackEventType::Custom
// with the raw topic string preserved.
enum class FeedbackEventType {
    // Guard feedback — Phase 11.1
    GuardFeedback,
    GuardAnomalyFlagged,

    // Routing outcome — Phase 11.2
    RouterOutcome,
    RouterDecision,

    // Answer quality — Phase 11.3 (re-uses Phase 8 quality scorer)
    QualityFeedback,
    QualityDrift,

    // Cost attribution — Phase 11.5
    CostObservation,
    BudgetAlert,

    // Operations / self-healing — Phase 11.4
    OpsIncident,
    OpsRollbackTriggered,

    // Generic fallback
    Custom,
};

struct FeedbackEvent {
    FeedbackEventType type = FeedbackEventType::Custom;
    std::string topic;
    std::string request_id;
    std::string tenant_id;
    std::string source;
    std::chrono::system_clock::time_point timestamp;
    nlohmann::json payload;

    // Canonical mapping — stable public API. Adding a new topic string is a
    // MINOR version change; renaming or removing one is MAJOR.
    static std::string topicOf(FeedbackEventType t);
    static FeedbackEventType typeOf(const std::string& topic);

    nlohmann::json toJson() const;
    static FeedbackEvent fromJson(const nlohmann::json& j);
};

} // namespace aegisgate
