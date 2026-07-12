#include "aegisgate/feedback_event.h"
#include <ctime>
#include <iomanip>
#include <sstream>

namespace aegisgate {

namespace {

// Format a system_clock::time_point as ISO-8601 with millisecond precision, UTC.
std::string formatIsoUtc(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    const auto time_t = system_clock::to_time_t(tp);
    const auto ms =
        duration_cast<milliseconds>(tp.time_since_epoch()).count() % 1000;
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &time_t);
#else
    gmtime_r(&time_t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setw(3) << std::setfill('0') << ms << 'Z';
    return oss.str();
}

// Parse ISO-8601 UTC back to system_clock. Returns now() on parse failure.
std::chrono::system_clock::time_point parseIsoUtc(const std::string& s) {
    std::tm tm_buf{};
    std::istringstream iss(s);
    iss >> std::get_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
    if (iss.fail()) return std::chrono::system_clock::now();
#ifdef _WIN32
    const auto tt = _mkgmtime(&tm_buf);
#else
    const auto tt = timegm(&tm_buf);
#endif
    auto tp = std::chrono::system_clock::from_time_t(tt);
    // Parse optional .mmm fraction.
    auto dot = s.find('.');
    if (dot != std::string::npos && dot + 1 < s.size()) {
        int ms_val = 0;
        std::istringstream ms_iss(s.substr(dot + 1, 3));
        if (ms_iss >> ms_val) {
            tp += std::chrono::milliseconds(ms_val);
        }
    }
    return tp;
}

} // namespace

std::string FeedbackEvent::topicOf(FeedbackEventType t) {
    switch (t) {
        case FeedbackEventType::GuardFeedback:        return "guard.feedback";
        case FeedbackEventType::GuardAnomalyFlagged:  return "guard.anomaly";
        case FeedbackEventType::RouterOutcome:        return "router.outcome";
        case FeedbackEventType::RouterDecision:       return "router.decision";
        case FeedbackEventType::QualityFeedback:      return "quality.feedback";
        case FeedbackEventType::QualityDrift:         return "quality.drift";
        case FeedbackEventType::CostObservation:      return "cost.observation";
        case FeedbackEventType::BudgetAlert:          return "cost.budget_alert";
        case FeedbackEventType::OpsIncident:          return "ops.incident";
        case FeedbackEventType::OpsRollbackTriggered: return "ops.rollback";
        case FeedbackEventType::Custom:               return "custom";
    }
    return "custom";
}

FeedbackEventType FeedbackEvent::typeOf(const std::string& topic) {
    if (topic == "guard.feedback")      return FeedbackEventType::GuardFeedback;
    if (topic == "guard.anomaly")       return FeedbackEventType::GuardAnomalyFlagged;
    if (topic == "router.outcome")      return FeedbackEventType::RouterOutcome;
    if (topic == "router.decision")     return FeedbackEventType::RouterDecision;
    if (topic == "quality.feedback")    return FeedbackEventType::QualityFeedback;
    if (topic == "quality.drift")       return FeedbackEventType::QualityDrift;
    if (topic == "cost.observation")    return FeedbackEventType::CostObservation;
    if (topic == "cost.budget_alert")   return FeedbackEventType::BudgetAlert;
    if (topic == "ops.incident")        return FeedbackEventType::OpsIncident;
    if (topic == "ops.rollback")        return FeedbackEventType::OpsRollbackTriggered;
    if (topic == "custom")              return FeedbackEventType::Custom;
    return FeedbackEventType::Custom;
}

nlohmann::json FeedbackEvent::toJson() const {
    return nlohmann::json{
        {"topic", topic.empty() ? topicOf(type) : topic},
        {"request_id", request_id},
        {"tenant_id", tenant_id},
        {"source", source},
        {"timestamp", formatIsoUtc(timestamp)},
        {"payload", payload.is_null() ? nlohmann::json::object() : payload},
    };
}

FeedbackEvent FeedbackEvent::fromJson(const nlohmann::json& j) {
    FeedbackEvent e;
    if (j.is_object()) {
        if (j.contains("topic") && j["topic"].is_string()) {
            e.topic = j["topic"].get<std::string>();
            e.type = typeOf(e.topic);
        }
        if (j.contains("request_id") && j["request_id"].is_string()) {
            e.request_id = j["request_id"].get<std::string>();
        }
        if (j.contains("tenant_id") && j["tenant_id"].is_string()) {
            e.tenant_id = j["tenant_id"].get<std::string>();
        }
        if (j.contains("source") && j["source"].is_string()) {
            e.source = j["source"].get<std::string>();
        }
        if (j.contains("timestamp") && j["timestamp"].is_string()) {
            e.timestamp = parseIsoUtc(j["timestamp"].get<std::string>());
        } else {
            e.timestamp = std::chrono::system_clock::now();
        }
        if (j.contains("payload")) {
            e.payload = j["payload"];
        }
    }
    return e;
}

} // namespace aegisgate
