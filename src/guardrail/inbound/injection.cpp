#include "guardrail/inbound/injection.h"
#include "guardrail/admin/guard_admin_controller.h"
#include "guardrail/audit.h"
#include "guardrail/guard_explanation_builder.h"
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <shared_mutex>

namespace aegisgate {

InjectionDetector::InjectionDetector() = default;

void InjectionDetector::loadPatterns(const std::string& yaml_path) {
    std::unique_lock<std::shared_mutex> lock(patterns_mutex_);
    try {
        auto root = YAML::LoadFile(yaml_path);

        if (auto pats = root["patterns"]) {
            for (const auto& p : pats) {
                InjectionPattern ip;
                ip.name = p["name"].as<std::string>();
                auto sev = p["severity"].as<std::string>("medium");
                if (sev == "high") ip.severity = InjectionSeverity::High;
                else if (sev == "low") ip.severity = InjectionSeverity::Low;
                else ip.severity = InjectionSeverity::Medium;

                ip.regex = std::make_unique<RE2>(p["regex"].as<std::string>());
                if (ip.regex->ok()) {
                    patterns_.push_back(std::move(ip));
                } else {
                    spdlog::warn("Invalid injection regex '{}': {}",
                                 ip.name, ip.regex->error());
                }
            }
        }

        if (auto kws = root["keywords"]) {
            for (const auto& kw : kws) {
                auto s = kw.as<std::string>();
                if (!s.empty()) {
                    keywords_.push_back(std::move(s));
                }
            }
        }

        if (auto heur = root["heuristic_features"]) {
            heuristic_config_.nested_quotes_threshold =
                heur["nested_quotes_threshold"].as<int>(3);
            heuristic_config_.instruction_override_score =
                heur["instruction_override_score"].as<double>(0.7);
            heuristic_config_.role_switch_score =
                heur["role_switch_score"].as<double>(0.8);
            heuristic_config_.encoding_attempt_score =
                heur["encoding_attempt_score"].as<double>(0.5);
        }

        loaded_ = true;
        spdlog::info("Loaded {} injection patterns, {} keywords",
                      patterns_.size(), keywords_.size());
    } catch (const YAML::Exception& e) {
        spdlog::error("Failed to load injection patterns: {}", e.what());
    }
}

void InjectionDetector::reloadPatterns(const std::string& yaml_path) {
    std::vector<InjectionPattern> new_patterns;
    std::vector<std::string> new_keywords;
    HeuristicConfig new_heuristic;

    try {
        auto root = YAML::LoadFile(yaml_path);

        if (auto pats = root["patterns"]) {
            for (const auto& p : pats) {
                InjectionPattern ip;
                ip.name = p["name"].as<std::string>();
                auto sev = p["severity"].as<std::string>("medium");
                if (sev == "high") ip.severity = InjectionSeverity::High;
                else if (sev == "low") ip.severity = InjectionSeverity::Low;
                else ip.severity = InjectionSeverity::Medium;

                ip.regex = std::make_unique<RE2>(p["regex"].as<std::string>());
                if (ip.regex->ok()) {
                    new_patterns.push_back(std::move(ip));
                } else {
                    spdlog::warn("Invalid injection regex '{}': {}",
                                 ip.name, ip.regex->error());
                }
            }
        }

        if (auto kws = root["keywords"]) {
            for (const auto& kw : kws) {
                auto s = kw.as<std::string>();
                if (!s.empty()) {
                    new_keywords.push_back(std::move(s));
                }
            }
        }

        if (auto heur = root["heuristic_features"]) {
            new_heuristic.nested_quotes_threshold =
                heur["nested_quotes_threshold"].as<int>(3);
            new_heuristic.instruction_override_score =
                heur["instruction_override_score"].as<double>(0.7);
            new_heuristic.role_switch_score =
                heur["role_switch_score"].as<double>(0.8);
            new_heuristic.encoding_attempt_score =
                heur["encoding_attempt_score"].as<double>(0.5);
        }

        std::unique_lock<std::shared_mutex> lock(patterns_mutex_);
        patterns_ = std::move(new_patterns);
        keywords_ = std::move(new_keywords);
        heuristic_config_ = new_heuristic;
        loaded_ = true;
        spdlog::info("Reloaded {} injection patterns, {} keywords",
                      patterns_.size(), keywords_.size());
    } catch (const YAML::Exception& e) {
        spdlog::error("Failed to reload injection patterns: {}", e.what());
    }
}

void InjectionDetector::setThreshold(double threshold) {
    threshold_ = threshold;
}

InjectionResult InjectionDetector::detect(const std::string& text) const {
    std::shared_lock<std::shared_mutex> lock(patterns_mutex_);

    // L1: Keyword fast scan
    auto kw_result = keywordScan(text);
    if (kw_result.detected) return kw_result;

    // L1: Regex fast scan
    auto regex_result = regexScan(text);
    if (regex_result.detected) return regex_result;

    // L2: Heuristic analysis
    auto heur_result = heuristicScan(text);
    if (heur_result.detected) return heur_result;

    return {};
}

InjectionResult InjectionDetector::keywordScan(const std::string& text) const {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& kw : keywords_) {
        std::string lower_kw = kw;
        std::transform(lower_kw.begin(), lower_kw.end(), lower_kw.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower.find(lower_kw) != std::string::npos) {
            return {true, kw, InjectionSeverity::High, 1.0, "L1-keyword"};
        }
    }
    return {};
}

InjectionResult InjectionDetector::regexScan(const std::string& text) const {
    for (const auto& pat : patterns_) {
        if (RE2::PartialMatch(text, *pat.regex)) {
            return {true, pat.name, pat.severity, 0.9, "L1-regex"};
        }
    }
    return {};
}

InjectionResult InjectionDetector::heuristicScan(const std::string& text) const {
    double score = 0.0;
    std::string reason;

    // Nested quotes detection (symmetric open/close on same quote char)
    int quote_depth = 0, max_depth = 0;
    bool in_quote = false;
    for (char c : text) {
        if (c == '"' || c == '\'' || c == '`') {
            if (in_quote) {
                quote_depth--;
                in_quote = false;
            } else {
                quote_depth++;
                in_quote = true;
            }
            max_depth = std::max(max_depth, quote_depth);
        }
    }
    if (max_depth >= heuristic_config_.nested_quotes_threshold) {
        score += 0.3;
        reason = "nested_quotes";
    }

    // Instruction override patterns (case-insensitive manual check)
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (lower.find("instruction") != std::string::npos &&
        (lower.find("override") != std::string::npos ||
         lower.find("replace") != std::string::npos ||
         lower.find("new ") != std::string::npos)) {
        score += heuristic_config_.instruction_override_score;
        reason = "instruction_override";
    }

    // Role switching patterns
    if (lower.find("you are now") != std::string::npos ||
        lower.find("act as") != std::string::npos ||
        lower.find("pretend to be") != std::string::npos) {
        score += heuristic_config_.role_switch_score;
        reason = "role_switch";
    }

    // Base64/encoding evasion
    if (lower.find("base64") != std::string::npos ||
        lower.find("encoded") != std::string::npos) {
        score += heuristic_config_.encoding_attempt_score;
        reason = "encoding_evasion";
    }

    if (score >= threshold_) {
        return {true, reason, InjectionSeverity::Medium, score, "L2-heuristic"};
    }
    return {};
}

StageResult InjectionDetector::process(RequestContext& ctx) {
    if (!loaded_ && !patterns_.empty()) loaded_ = true;
    if (!loaded_ && patterns_.empty() && keywords_.empty()) {
        // P0-2: previously this rejected every request and logged per request,
        // turning a missing/corrupt patterns YAML into a full-traffic outage
        // plus a log storm. Log the degraded mode exactly once and honour the
        // configured policy (default fail-closed for security).
        std::call_once(no_rules_log_flag_, [this]() {
            spdlog::error(
                "InjectionDetector: no patterns loaded — operating in {} mode",
                fail_open_ ? "FAIL-OPEN (requests pass through)"
                           : "FAIL-CLOSED (requests rejected)");
        });
        return fail_open_ ? StageResult::Continue : StageResult::Reject;
    }
    // P0-4 / C3: scan InputPreprocessor's canonicalised text (via ctx.scanText,
    // which falls back to raw content when preprocessing didn't run or arrays are
    // misaligned) so unicode/homoglyph obfuscation can't slip an injection past.
    const auto& messages = ctx.chat_request.messages;
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        if (isToolMessage(msg)) continue;
        auto result = detect(ctx.scanText(i));
        // TASK-20260707-03 / REV20260707-N19: also scan the vision image
        // reference channel (image_url / decoded data: URI text) so an injection
        // can't ride in on an image reference and bypass this stage.
        if (!result.detected) {
            std::string image_ref = ctx.scanImageText(i);
            if (!image_ref.empty()) result = detect(image_ref);
        }
        if (result.detected) {
            spdlog::warn("Injection detected in request {}: pattern='{}', "
                         "layer={}, confidence={}",
                         ctx.request_id, result.matched_pattern,
                         result.layer, result.confidence);
            if (audit_logger_) {
                audit_logger_->logAction(
                    ctx.request_id, ctx.tenant_id, name(), "blocked",
                    "injection pattern='" + result.matched_pattern +
                        "' layer=" + result.layer);
            }
            // TASK-20260708-03 / REV20260707-C2: record structured
            // GuardExplanation so `GET /admin/api/guard/explanation/{id}`
            // can return machine-readable "why was this blocked" details.
            // Nullable-safe (SR-3): controller may be unwired in community
            // edition; audit_logger path stays intact (SR-2 parallel).
            if (guard_admin_controller_) {
                guard_admin_controller_->recordExplanation(
                    ctx.request_id,
                    guard::GuardExplanationBuilder::fromInjection(result));
            }
            return StageResult::Reject;
        }
    }
    return StageResult::Continue;
}

} // namespace aegisgate
