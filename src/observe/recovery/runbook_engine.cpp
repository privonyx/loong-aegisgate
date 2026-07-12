// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 5.

#include "observe/recovery/runbook_engine.h"

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace aegisgate {

namespace {

namespace fs = std::filesystem;

bool dirIsSafe(const std::string& path, std::string* err) {
    std::error_code ec;
    fs::path p(path);
    auto canon = fs::weakly_canonical(p, ec);
    if (ec) {
        if (err) *err = "canonical resolve failed: " + ec.message();
        return false;
    }
    const std::string s = canon.string();
    if (s.empty() || s.find("/..") != std::string::npos) {
        if (err) *err = "path traversal rejected";
        return false;
    }
    return true;
}

FeedbackEventType parseEventType(const std::string& s) {
    if (s == "GuardFeedback")        return FeedbackEventType::GuardFeedback;
    if (s == "GuardAnomalyFlagged")  return FeedbackEventType::GuardAnomalyFlagged;
    if (s == "RouterOutcome")        return FeedbackEventType::RouterOutcome;
    if (s == "RouterDecision")       return FeedbackEventType::RouterDecision;
    if (s == "QualityFeedback")      return FeedbackEventType::QualityFeedback;
    if (s == "QualityDrift")         return FeedbackEventType::QualityDrift;
    if (s == "CostObservation")      return FeedbackEventType::CostObservation;
    if (s == "BudgetAlert")          return FeedbackEventType::BudgetAlert;
    if (s == "OpsIncident")          return FeedbackEventType::OpsIncident;
    if (s == "OpsRollbackTriggered") return FeedbackEventType::OpsRollbackTriggered;
    return FeedbackEventType::Custom;
}

bool compareOp(double lhs, const std::string& op, double rhs) {
    if (op == ">")  return lhs > rhs;
    if (op == ">=") return lhs >= rhs;
    if (op == "<")  return lhs < rhs;
    if (op == "<=") return lhs <= rhs;
    if (op == "==") return lhs == rhs;
    return false;
}

std::optional<double> readMetric(const SignalSnapshot& s,
                                   const std::string& signal) {
    if (signal == "p99_latency_ms") return s.p99_latency_ms;
    if (signal == "error_rate")     return s.error_rate;
    if (signal == "sample_count")   return s.sample_count;
    auto it = s.custom_metrics.find(signal);
    if (it != s.custom_metrics.end()) return it->second;
    return std::nullopt;
}

nlohmann::json yamlToJson(const YAML::Node& n) {
    using nlohmann::json;
    if (n.IsNull() || !n.IsDefined()) return json{};
    if (n.IsScalar()) {
        if (n.Tag() == "tag:yaml.org,2002:bool" ||
            n.Tag() == "!") {
            try { return n.as<bool>(); } catch (...) {}
        }
        try { return n.as<int>();    } catch (...) {}
        try { return n.as<double>(); } catch (...) {}
        try { return n.as<bool>();   } catch (...) {}
        return n.as<std::string>();
    }
    if (n.IsSequence()) {
        json arr = json::array();
        for (const auto& c : n) arr.push_back(yamlToJson(c));
        return arr;
    }
    if (n.IsMap()) {
        json obj = json::object();
        for (auto it = n.begin(); it != n.end(); ++it) {
            obj[it->first.as<std::string>()] = yamlToJson(it->second);
        }
        return obj;
    }
    return json{};
}

bool parseRunbook(const YAML::Node& root, Runbook& rb) {
    if (!root["id"]) return false;
    rb.id = root["id"].as<std::string>();
    rb.description = root["description"]
                         ? root["description"].as<std::string>() : "";
    if (root["approval_required"]) {
        rb.approval_required = root["approval_required"].as<bool>();
    }
    if (root["cooldown_seconds"]) {
        rb.cooldown_seconds = root["cooldown_seconds"].as<int>();
    }
    if (!root["triggers"] || !root["triggers"].IsSequence() ||
        root["triggers"].size() == 0) {
        return false;
    }
    for (const auto& tn : root["triggers"]) {
        RunbookTrigger t;
        const std::string kind = tn["kind"] ? tn["kind"].as<std::string>() : "";
        if (kind == "metric") {
            t.kind = RunbookTriggerKind::Metric;
            t.signal = tn["signal"] ? tn["signal"].as<std::string>() : "";
            t.op     = tn["op"]     ? tn["op"].as<std::string>()     : ">";
            t.value  = tn["value"]  ? tn["value"].as<double>()       : 0.0;
            if (tn["hold_seconds"]) t.hold_seconds = tn["hold_seconds"].as<int>();
        } else if (kind == "feedback_event_count") {
            t.kind = RunbookTriggerKind::FeedbackEventCount;
            t.event_type = parseEventType(
                tn["event_type"] ? tn["event_type"].as<std::string>() : "");
            t.topic_match = tn["topic_match"] ? tn["topic_match"].as<std::string>() : "";
            if (tn["window_seconds"])  t.window_seconds = tn["window_seconds"].as<int>();
            if (tn["count_threshold"]) t.count_threshold = tn["count_threshold"].as<int>();
        } else if (kind == "rca_required") {
            t.kind = RunbookTriggerKind::RcaRequired;
            t.rca_rule_id = tn["rca_rule_id"] ? tn["rca_rule_id"].as<std::string>() : "";
            if (tn["rca_min_score"]) t.rca_min_score = tn["rca_min_score"].as<double>();
        } else {
            return false;
        }
        rb.triggers.push_back(std::move(t));
    }
    if (root["actions"] && root["actions"].IsSequence()) {
        for (const auto& an : root["actions"]) {
            RunbookAction a;
            a.action = an["action"] ? an["action"].as<std::string>() : "";
            if (a.action.empty()) return false;
            a.payload = an["payload"] ? yamlToJson(an["payload"])
                                      : nlohmann::json::object();
            rb.actions.push_back(std::move(a));
        }
    }
    if (root["rollback_actions"] && root["rollback_actions"].IsSequence()) {
        for (const auto& an : root["rollback_actions"]) {
            RunbookAction a;
            a.action = an["action"] ? an["action"].as<std::string>() : "";
            if (a.action.empty()) continue;
            a.payload = an["payload"] ? yamlToJson(an["payload"])
                                      : nlohmann::json::object();
            rb.rollback_actions.push_back(std::move(a));
        }
    }
    if (rb.actions.empty()) return false;
    return true;
}

bool evalMetric(const RunbookTrigger& t, const RcaSignals& s,
                  std::string& summary) {
    auto cur = readMetric(s.metrics_now, t.signal);
    if (!cur) return false;
    if (!compareOp(*cur, t.op, t.value)) return false;
    std::ostringstream os;
    os << "metric:" << t.signal << "=" << *cur << t.op << t.value;
    summary = os.str();
    return true;
}

bool evalFeedback(const RunbookTrigger& t, const RcaSignals& s,
                    std::string& summary) {
    int count = 0;
    for (const auto& e : s.recent_feedback_events) {
        if (e.type != t.event_type) continue;
        if (!t.topic_match.empty() &&
            e.topic.find(t.topic_match) == std::string::npos) {
            continue;
        }
        ++count;
    }
    if (count < t.count_threshold) return false;
    std::ostringstream os;
    os << "feedback:" << FeedbackEvent::topicOf(t.event_type)
        << " count=" << count;
    summary = os.str();
    return true;
}

bool evalRcaRequired(const RunbookTrigger& t,
                       const std::vector<RootCauseHypothesis>& rca,
                       std::string& summary) {
    for (const auto& h : rca) {
        if (h.rule_id != t.rca_rule_id) continue;
        if (h.score < t.rca_min_score)  continue;
        std::ostringstream os;
        os << "rca:" << t.rca_rule_id << " score=" << h.score;
        summary = os.str();
        return true;
    }
    return false;
}

} // anonymous

RunbookEngine::RunbookEngine(common::Clock* clock) : clock_(clock) {}

bool RunbookEngine::reloadRunbooks(const std::string& yaml_dir) {
    std::string err;
    if (!dirIsSafe(yaml_dir, &err)) {
        spdlog::warn("[Runbook] reload rejected: {} (path={})", err, yaml_dir);
        return false;
    }
    std::error_code ec;
    if (!fs::is_directory(yaml_dir, ec)) {
        spdlog::warn("[Runbook] reload: {} is not a directory", yaml_dir);
        return false;
    }

    std::vector<Runbook> parsed;
    bool any_failure = false;
    for (const auto& entry : fs::directory_iterator(yaml_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const auto& p = entry.path();
        if (p.extension() != ".yaml" && p.extension() != ".yml") continue;

        std::ifstream in(p);
        std::stringstream body;
        body << in.rdbuf();
        const std::string body_str = body.str();
        if (body_str.empty()) {
            spdlog::warn("[Runbook] {} empty, skipping load", p.string());
            any_failure = true;
            break;
        }
        YAML::Node root;
        try { root = YAML::Load(body_str); }
        catch (const YAML::Exception& e) {
            spdlog::warn("[Runbook] {} parse error: {}", p.string(), e.what());
            any_failure = true;
            break;
        }
        Runbook rb;
        if (!parseRunbook(root, rb)) {
            spdlog::warn("[Runbook] {} schema invalid", p.string());
            any_failure = true;
            break;
        }
        parsed.push_back(std::move(rb));
    }
    if (any_failure) return false;
    if (parsed.empty()) return false;  // no valid runbook in dir

    spdlog::info("[Runbook] reloaded {} runbooks from {}",
                  parsed.size(), yaml_dir);
    {
        std::lock_guard<std::mutex> g(mu_);
        runbooks_ = std::move(parsed);
    }
    return true;
}

std::size_t RunbookEngine::runbookCount() const {
    std::lock_guard<std::mutex> g(mu_);
    return runbooks_.size();
}

std::vector<RunbookMatch>
RunbookEngine::evaluate(const RcaSignals& signals,
                          const std::vector<RootCauseHypothesis>& rca) const {
    std::vector<RunbookMatch> out;
    const std::int64_t now_ms = clock_ ? clock_->nowMillis() : 0;
    std::lock_guard<std::mutex> g(mu_);
    for (const auto& rb : runbooks_) {
        // Cooldown gate (M3).
        auto it = last_triggered_ms_.find(rb.id);
        if (it != last_triggered_ms_.end()) {
            const std::int64_t cd_ms =
                static_cast<std::int64_t>(rb.cooldown_seconds) * 1000;
            if (now_ms - it->second < cd_ms) continue;
        }
        std::string summary;
        bool fired = false;
        for (const auto& t : rb.triggers) {
            switch (t.kind) {
                case RunbookTriggerKind::Metric:
                    fired = evalMetric(t, signals, summary); break;
                case RunbookTriggerKind::FeedbackEventCount:
                    fired = evalFeedback(t, signals, summary); break;
                case RunbookTriggerKind::RcaRequired:
                    fired = evalRcaRequired(t, rca, summary); break;
            }
            if (fired) break;  // OR semantics
        }
        if (!fired) continue;
        RunbookMatch m;
        m.runbook_id      = rb.id;
        m.runbook         = rb;
        m.trigger_summary = std::move(summary);
        out.push_back(std::move(m));
    }
    return out;
}

void RunbookEngine::markTriggered(const std::string& runbook_id,
                                     std::int64_t steady_now_ms) {
    std::lock_guard<std::mutex> g(mu_);
    last_triggered_ms_[runbook_id] = steady_now_ms;
}

autonomy::ApprovalProposal
RunbookEngine::buildProposal(const RunbookMatch& m) const {
    using nlohmann::json;
    autonomy::ApprovalProposal p;
    p.source  = autonomy::AutonomySource::AutoRecovery;
    p.subject = m.runbook_id;

    json actions_arr = json::array();
    for (const auto& a : m.runbook.actions) {
        actions_arr.push_back({{"action", a.action}, {"payload", a.payload}});
    }
    json rb_arr = json::array();
    for (const auto& a : m.runbook.rollback_actions) {
        rb_arr.push_back({{"action", a.action}, {"payload", a.payload}});
    }
    p.payload = {
        {"runbook_id",        m.runbook_id},
        {"description",       m.runbook.description},
        {"trigger_summary",   m.trigger_summary},
        {"approval_required", m.runbook.approval_required},
        {"actions",           actions_arr},
        {"rollback_actions",  rb_arr},
    };

    // First action's "action" key is also surfaced top-level so the
    // RecoveryApplier (which dispatches on payload.action) can run when
    // the runbook has exactly one action — keeps the propose-first /
    // apply-via-applier wiring simple. Multi-action runbooks need the
    // RunbookEngine to drive sequential apply().
    if (!m.runbook.actions.empty()) {
        p.payload["action"]         = m.runbook.actions[0].action;
        for (auto it = m.runbook.actions[0].payload.begin();
             it != m.runbook.actions[0].payload.end(); ++it) {
            p.payload[it.key()] = it.value();
        }
    }

    p.proposed_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    p.proposer_user_id = "system.autorecovery";
    return p;
}

} // namespace aegisgate
