// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 3.

#include "observe/recovery/root_cause_analyzer.h"

#include "guardrail/inbound/pii_filter.h"

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace aegisgate {

namespace {

namespace fs = std::filesystem;

std::int64_t nowMsInt() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string sha256Hex(const std::string& body) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, body.data(), body.size());
    unsigned int outlen = 0;
    EVP_DigestFinal_ex(ctx, digest, &outlen);
    EVP_MD_CTX_free(ctx);
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(outlen * 2);
    for (unsigned int i = 0; i < outlen; ++i) {
        out.push_back(hex[digest[i] >> 4]);
        out.push_back(hex[digest[i] & 0x0F]);
    }
    return out;
}

// SR-NEW1 path guard — rejects relative traversal and non-YAML extensions.
bool pathIsSafe(const std::string& yaml_path, std::string* err) {
    std::error_code ec;
    fs::path p(yaml_path);
    auto canon = fs::weakly_canonical(p, ec);
    if (ec) {
        if (err) *err = "canonical resolve failed: " + ec.message();
        return false;
    }
    const std::string s = canon.string();
    // Canonical form normalises ".." segments away. If any survive (or the
    // canonical path is empty), reject as suspicious.
    if (s.empty() || s.find("/..") != std::string::npos) {
        if (err) *err = "path traversal rejected";
        return false;
    }
    if (canon.extension() != ".yaml" && canon.extension() != ".yml") {
        if (err) *err = "extension not yaml";
        return false;
    }
    return true;
}

std::string expandTemplate(const std::string& tpl,
                             const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string out = tpl;
    for (const auto& [k, v] : kv) {
        const std::string needle = "${" + k + "}";
        for (size_t pos = out.find(needle); pos != std::string::npos;
             pos = out.find(needle, pos + v.size())) {
            out.replace(pos, needle.size(), v);
        }
    }
    return out;
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

} // anonymous

double scoreRule(const Rule& rule, std::size_t matched_count) {
    if (matched_count < rule.min_matched_conditions) return 0.0;
    double raw = rule.score_per_condition *
                  static_cast<double>(matched_count);
    return std::min(raw, rule.score_when_all_matched);
}

RootCauseAnalyzer::RootCauseAnalyzer(std::shared_ptr<PIIFilter> pii_filter)
    : pii_filter_(std::move(pii_filter)) {}

bool RootCauseAnalyzer::reloadRules(const std::string& yaml_path) {
    std::string err;
    if (!pathIsSafe(yaml_path, &err)) {
        spdlog::warn("[RCA] reloadRules rejected: {} (path={})", err, yaml_path);
        return false;
    }
    std::ifstream in(yaml_path);
    if (!in) {
        spdlog::warn("[RCA] reloadRules: cannot open {}", yaml_path);
        return false;
    }
    std::stringstream body;
    body << in.rdbuf();
    const std::string body_str = body.str();
    if (body_str.empty()) {
        spdlog::warn("[RCA] reloadRules: empty file {}", yaml_path);
        return false;
    }
    YAML::Node root;
    try {
        root = YAML::Load(body_str);
    } catch (const YAML::Exception& e) {
        spdlog::warn("[RCA] reloadRules: yaml parse error {}: {}", yaml_path, e.what());
        return false;
    }
    if (!root["rules"] || !root["rules"].IsSequence()) {
        spdlog::warn("[RCA] reloadRules: missing 'rules' sequence in {}", yaml_path);
        return false;
    }

    std::vector<Rule> parsed;
    parsed.reserve(root["rules"].size());
    for (const auto& rn : root["rules"]) {
        Rule r;
        if (!rn["id"]) {
            spdlog::warn("[RCA] rule missing id, skipping reload");
            return false;
        }
        r.id = rn["id"].as<std::string>();
        r.category = rn["category"] ? rn["category"].as<std::string>() : "";
        r.summary  = rn["summary"]  ? rn["summary"].as<std::string>()  : "";
        if (rn["min_matched_conditions"]) {
            r.min_matched_conditions = rn["min_matched_conditions"].as<std::size_t>();
        }
        if (rn["score_per_condition"]) {
            r.score_per_condition = rn["score_per_condition"].as<double>();
        }
        if (rn["score_when_all_matched"]) {
            r.score_when_all_matched = rn["score_when_all_matched"].as<double>();
        }
        if (rn["suggested_actions"] && rn["suggested_actions"].IsSequence()) {
            for (const auto& a : rn["suggested_actions"]) {
                r.suggested_actions.push_back(a.as<std::string>());
            }
        }
        if (!rn["conditions"] || !rn["conditions"].IsSequence() ||
            rn["conditions"].size() == 0) {
            spdlog::warn("[RCA] rule {} has no conditions", r.id);
            return false;
        }
        for (const auto& cn : rn["conditions"]) {
            const std::string kind = cn["kind"] ? cn["kind"].as<std::string>() : "";
            Condition c;
            if (kind == "metric_threshold") {
                MetricThresholdCondition m;
                m.signal = cn["signal"] ? cn["signal"].as<std::string>() : "";
                m.op     = cn["op"]     ? cn["op"].as<std::string>()     : ">";
                m.value  = cn["value"]  ? cn["value"].as<double>()       : 0.0;
                m.evidence_label_tpl =
                    cn["evidence_label"] ? cn["evidence_label"].as<std::string>() : "";
                c.kind = ConditionKind::MetricThreshold;
                c.impl = std::move(m);
            } else if (kind == "feedback_event_count") {
                FeedbackEventCountCondition f;
                f.event_type = parseEventType(
                    cn["event_type"] ? cn["event_type"].as<std::string>() : "");
                f.topic_match =
                    cn["topic_match"] ? cn["topic_match"].as<std::string>() : "";
                f.window_seconds = cn["window_seconds"]
                                       ? cn["window_seconds"].as<int>()
                                       : 300;
                f.count_threshold = cn["count_threshold"]
                                        ? cn["count_threshold"].as<int>()
                                        : 1;
                f.evidence_label_tpl =
                    cn["evidence_label"] ? cn["evidence_label"].as<std::string>() : "";
                c.kind = ConditionKind::FeedbackEventCount;
                c.impl = std::move(f);
            } else if (kind == "log_pattern_count") {
                LogPatternCountCondition l;
                l.pattern_text =
                    cn["pattern"] ? cn["pattern"].as<std::string>() : "";
                if (l.pattern_text.empty()) {
                    spdlog::warn("[RCA] rule {} log_pattern empty", r.id);
                    return false;
                }
                l.pattern = std::make_shared<RE2>(l.pattern_text);
                if (!l.pattern->ok()) {
                    spdlog::warn("[RCA] rule {} log_pattern invalid: {}",
                                  r.id, l.pattern_text);
                    return false;
                }
                l.window_seconds = cn["window_seconds"]
                                       ? cn["window_seconds"].as<int>()
                                       : 300;
                l.count_threshold = cn["count_threshold"]
                                        ? cn["count_threshold"].as<int>()
                                        : 1;
                l.evidence_label_tpl =
                    cn["evidence_label"] ? cn["evidence_label"].as<std::string>() : "";
                c.kind = ConditionKind::LogPatternCount;
                c.impl = std::move(l);
            } else {
                spdlog::warn("[RCA] rule {} unknown condition kind '{}'",
                              r.id, kind);
                return false;
            }
            r.conditions.push_back(std::move(c));
        }
        parsed.push_back(std::move(r));
    }

    spdlog::info("[RCA] reloaded {} rules from {} sha256={}",
                  parsed.size(), yaml_path, sha256Hex(body_str));

    {
        std::unique_lock<std::shared_mutex> g(mu_);
        rules_ = std::move(parsed);
        last_loaded_ms_ = nowMsInt();
    }
    return true;
}

std::pair<std::size_t, std::int64_t> RootCauseAnalyzer::health() const {
    std::shared_lock<std::shared_mutex> g(mu_);
    return {rules_.size(), last_loaded_ms_};
}

namespace {

bool evaluateMetricThreshold(const MetricThresholdCondition& m,
                                const RcaSignals& signals,
                                RcaEvidence& out_ev) {
    auto cur = readMetric(signals.metrics_now, m.signal);
    if (!cur.has_value()) return false;
    const bool match = compareOp(*cur, m.op, m.value);
    if (!match) return false;
    out_ev.source = "metric:" + m.signal;
    out_ev.current_value = std::to_string(*cur);
    out_ev.expected_value = m.op + std::to_string(m.value);
    out_ev.label = expandTemplate(m.evidence_label_tpl,
                                    {{"current",  out_ev.current_value},
                                     {"expected", out_ev.expected_value}});
    return true;
}

bool evaluateFeedbackEventCount(const FeedbackEventCountCondition& f,
                                  const RcaSignals& signals,
                                  RcaEvidence& out_ev) {
    int count = 0;
    for (const auto& e : signals.recent_feedback_events) {
        if (e.type != f.event_type) continue;
        if (!f.topic_match.empty() && e.topic.find(f.topic_match) ==
                                          std::string::npos) {
            continue;
        }
        ++count;
    }
    if (count < f.count_threshold) return false;
    out_ev.source = "feedback:" + FeedbackEvent::topicOf(f.event_type);
    out_ev.current_value = std::to_string(count);
    out_ev.expected_value = ">=" + std::to_string(f.count_threshold);
    out_ev.label = expandTemplate(f.evidence_label_tpl,
                                    {{"count",  out_ev.current_value},
                                     {"window", std::to_string(f.window_seconds)}});
    return true;
}

bool evaluateLogPatternCount(const LogPatternCountCondition& l,
                                const RcaSignals& signals,
                                RcaEvidence& out_ev) {
    int count = 0;
    std::string sample_match;
    for (const auto& e : signals.recent_log_entries) {
        if (l.pattern && RE2::PartialMatch(e.msg_masked, *l.pattern)) {
            if (sample_match.empty()) sample_match = e.msg_masked;
            ++count;
        }
    }
    if (count < l.count_threshold) return false;
    out_ev.source = "log_pattern";
    out_ev.current_value = std::to_string(count);
    out_ev.expected_value = ">=" + std::to_string(l.count_threshold);
    out_ev.label = expandTemplate(l.evidence_label_tpl,
                                    {{"count",  out_ev.current_value},
                                     {"sample", sample_match}});
    return true;
}

} // anonymous

std::vector<RootCauseHypothesis>
RootCauseAnalyzer::analyze(const RcaSignals& signals) const {
    std::vector<RootCauseHypothesis> out;
    std::shared_lock<std::shared_mutex> g(mu_);
    for (const auto& rule : rules_) {
        std::vector<RcaEvidence> matched;
        matched.reserve(rule.conditions.size());
        for (const auto& cond : rule.conditions) {
            RcaEvidence ev;
            bool matched_one = false;
            switch (cond.kind) {
                case ConditionKind::MetricThreshold:
                    matched_one = evaluateMetricThreshold(
                        std::get<MetricThresholdCondition>(cond.impl),
                        signals, ev);
                    break;
                case ConditionKind::FeedbackEventCount:
                    matched_one = evaluateFeedbackEventCount(
                        std::get<FeedbackEventCountCondition>(cond.impl),
                        signals, ev);
                    break;
                case ConditionKind::LogPatternCount:
                    matched_one = evaluateLogPatternCount(
                        std::get<LogPatternCountCondition>(cond.impl),
                        signals, ev);
                    break;
            }
            if (matched_one) {
                // SR-NEW2 defence-in-depth: mask AGAIN even if the source
                // (LogRingbufferSink etc.) already masked.
                if (pii_filter_) {
                    ev.label          = pii_filter_->mask(ev.label);
                    ev.current_value  = pii_filter_->mask(ev.current_value);
                    ev.expected_value = pii_filter_->mask(ev.expected_value);
                }
                matched.push_back(std::move(ev));
            }
        }
        const double s = scoreRule(rule, matched.size());
        if (s < kRcaOutputScoreThreshold) continue;

        RootCauseHypothesis h;
        h.rule_id           = rule.id;
        h.score             = s;
        h.category          = rule.category;
        h.summary           = rule.summary;
        h.evidence          = std::move(matched);
        h.suggested_actions = rule.suggested_actions;
        out.push_back(std::move(h));
    }
    std::sort(out.begin(), out.end(),
              [](const RootCauseHypothesis& a, const RootCauseHypothesis& b) {
                  return a.score > b.score;
              });
    return out;
}

std::optional<RootCauseHypothesis>
RootCauseAnalyzer::findHypothesis(
    const std::vector<RootCauseHypothesis>& results,
    const std::string& rule_id) const {
    for (const auto& h : results) {
        if (h.rule_id == rule_id) return h;
    }
    return std::nullopt;
}

} // namespace aegisgate
